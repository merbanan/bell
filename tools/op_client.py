#!/usr/bin/env python3
"""Operator client for smoke tests: connects to a bridge port, forwards stdin
bytes to the module, prints the module's UART bytes (telnet framing stripped)
to stdout."""

import argparse
import os
import socket
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "host"))
from bellhost import telnet                      # noqa: E402


def decode(data: bytes) -> bytes:
    """Strip telnet framing, unescape doubled IAC."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b == telnet.IAC:
            if i + 1 < n and data[i + 1] == telnet.IAC:
                out.append(telnet.IAC)
                i += 2
                continue
            i += 3                      # swallow a command triplet
            continue
        out.append(b)
        i += 1
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    a = ap.parse_args()
    s = socket.create_connection((a.host, a.port))

    def reader() -> None:
        while True:
            d = s.recv(4096)
            if not d:
                break
            sys.stdout.buffer.write(decode(d))
            sys.stdout.buffer.flush()

    threading.Thread(target=reader, daemon=True).start()
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            break
        s.sendall(telnet.encode(65536, line))


if __name__ == "__main__":
    main()
