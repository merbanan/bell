#!/usr/bin/env python3
"""Fake bell module for smoke tests: connects to the host, announces its MAC,
forwards stdin bytes to the socket, prints socket bytes to stdout."""

import argparse
import os
import socket
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "host"))
from bellhost import telnet                      # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=12345)
    ap.add_argument("--mac", default="02:00:00:00:00:01")
    a = ap.parse_args()
    s = socket.create_connection((a.host, a.port))
    s.sendall(bytes([255, 250, 200]) + a.mac.encode("ascii") + bytes([255, 240]))

    def reader() -> None:
        while True:
            d = s.recv(4096)
            if not d:
                break
            sys.stdout.buffer.write(d)
            sys.stdout.buffer.flush()

    threading.Thread(target=reader, daemon=True).start()
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            break
        s.sendall(telnet.encode(65536, line))


if __name__ == "__main__":
    main()
