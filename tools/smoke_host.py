#!/usr/bin/env python3
"""Smoke test for the console-server host.

Brings up the host, a fake module, an operator client, and live viewers;
checks the module round trip both ways, the log file's markers, the web
endpoints, and the viewer claim hand-off."""

import asyncio
import datetime
import fcntl
import os
import subprocess
import sys
import time

import aiohttp

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST_DIR = os.path.join(ROOT, "host")
MAC = "02:00:00:00:00:01"
SLOT = "bell-smoke"
BASE = "http://127.0.0.1:8081"
MODULE_PORT = 12399
CFG_PATH = "/tmp/opencode/smoke_config.yaml"
LOG_DIR = "/tmp/opencode/logs"

CFG_TEXT = """host:
  modulePort: 12399
  bridgeBase: 12370
  webPort: 8081
  storage: ./logs

modules: []
"""


def fail(msg: str) -> None:
    print(f"smoke FAIL: {msg}")
    sys.exit(1)


async def wait_state(sess, pred, timeout: float = 10.0):
    deadline = time.time() + timeout
    while True:
        async with sess.get(f"{BASE}/api/state") as r:
            j = await r.json()
            if pred(j):
                return j
        if time.time() > deadline:
            fail("state never matched")
        await asyncio.sleep(0.2)


def wait_stdout(proc, want: bytes, timeout: float = 10.0) -> None:
    """Polls a subprocess stdout pipe without blocking the event loop."""
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        if chunk:
            buf += chunk
            if want in buf:
                return
        time.sleep(0.05)
    fail(f"{want!r} never showed up")


async def main() -> None:
    with open(CFG_PATH, "w", encoding="utf-8") as fh:
        fh.write(CFG_TEXT)
    try:
        os.remove(os.path.join(LOG_DIR, SLOT,
                               f"{datetime.date.today().isoformat()}.log"))
    except FileNotFoundError:
        pass
    host = subprocess.Popen(
        [sys.executable, "-m", "bellhost", "--config", CFG_PATH],
        cwd=HOST_DIR, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    op = mod = None
    try:
        async with aiohttp.ClientSession() as sess:
            deadline = time.time() + 10
            while True:
                try:
                    async with sess.get(f"{BASE}/api/state") as r:
                        if r.status == 200:
                            break
                except aiohttp.ClientError:
                    pass
                if time.time() > deadline:
                    fail("web never came up")
                await asyncio.sleep(0.2)

            mod = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "fake_module.py"),
                 "--host", "127.0.0.1", "--port", str(MODULE_PORT),
                 "--mac", MAC],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            await wait_state(sess, lambda j: any(
                p["mac"] == MAC and p["connected"] for p in j["pending"]))

            async with sess.post(f"{BASE}/api/onboard",
                                 json={"mac": MAC, "name": SLOT}) as r:
                j = await r.json()
                if not j.get("ok"):
                    fail(f"onboard refused: {j}")
            rw, ro = j["rw"], j["ro"]
            print(f"onboarded as {SLOT} (rw {rw}, ro {ro})")

            await wait_state(sess, lambda s_: any(
                m["slot"] == SLOT and m["online"] for m in s_["modules"]))

            op = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "op_client.py"),
                 "--host", "127.0.0.1", "--port", str(rw)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE)

            op.stdin.write(b"hello\r\n")
            op.stdin.flush()
            wait_stdout(mod, b"hello")

            mod.stdin.write(b"uart-in\r\n")
            mod.stdin.flush()
            wait_stdout(op, b"uart-in")

            today = datetime.date.today().isoformat()
            path = os.path.join(LOG_DIR, SLOT, f"{today}.log")
            if not os.path.exists(path):
                fail("log file missing")
            text = open(path, encoding="utf-8").read()
            if "* module 02:00:00:00:00:01 connected" not in text:
                fail("no module connect event in log")
            if "> 68 65 6C 6C 6F 0D 0A" not in text:
                fail("no '>' burst line for 'hello'")
            if "< 75 61 72 74 2D 69 6E 0D 0A" not in text:
                fail("no '<' burst line for 'uart-in'")

            async with sess.ws_connect(f"{BASE}/ws/{SLOT}") as ws:
                msg = await ws.receive()
                if msg.data != "rw":
                    fail(f"first viewer mode {msg.data}")
                await ws.send_str("ws-hello\r")
                wait_stdout(mod, b"ws-hello")
                mod.stdin.write(b"uart-in2\r\n")
                mod.stdin.flush()
                msg = await ws.receive()
                if not isinstance(msg.data, bytes) or b"uart-in2" not in msg.data:
                    fail(f"viewer did not get uart bytes: {msg.data}")

                async with sess.ws_connect(f"{BASE}/ws/{SLOT}") as ws2:
                    msg = await ws2.receive()
                    if msg.data != "ro":
                        fail(f"second viewer mode {msg.data}")

            async with sess.ws_connect(f"{BASE}/ws/{SLOT}") as ws3:
                msg = await ws3.receive()
                if msg.data != "rw":
                    fail(f"viewer after detach mode {msg.data}")

            for p in ("/", "/logs", "/onboard", f"/live/{SLOT}"):
                async with sess.get(f"{BASE}{p}") as r:
                    if r.status != 200:
                        fail(f"{p} -> {r.status}")

            print("smoke ok")
    finally:
        for p in (host, op, mod):
            if p is None:
                continue
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        out = host.stdout.read() if host and host.stdout else b""
        if out:
            sys.stdout.write(out.decode("utf-8", "ignore"))


if __name__ == "__main__":
    asyncio.run(main())
