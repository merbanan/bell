"""Per-module log files: logs/<slot>/YYYY-MM-DD.log, one line per burst."""

from __future__ import annotations

import os
import threading
from datetime import datetime

HEX_LINE_CAP = 64      # bytes shown per line; longer bursts get a "+ n more"
ASCII_COL = True


class Logs:
    """Writes burst lines as they happen, flushed immediately so a live tail
    never sees stale data."""

    def __init__(self, root: str):
        self.root = root
        self._files: dict[tuple[str, str], object] = {}
        self._lock = threading.Lock()

    def _fh(self, slot: str) -> object:
        day = datetime.now().strftime("%Y-%m-%d")
        key = (slot, day)
        fh = self._files.get(key)
        if fh is None or fh.closed:
            d = os.path.join(self.root, slot)
            os.makedirs(d, exist_ok=True)
            fh = open(os.path.join(d, f"{day}.log"), "a", encoding="utf-8",
                      errors="replace")
            self._files[key] = fh
        return fh

    def _write(self, slot: str, line: str) -> None:
        fh = self._fh(slot)
        ts = datetime.now().strftime("%H:%M:%S")
        fh.write(f"{ts} {line}\n")
        fh.flush()

    def burst(self, slot: str, direction: str, data: bytes) -> None:
        """One line per burst: hex bytes, then an ASCII column."""
        with self._lock:
            shown = data[:HEX_LINE_CAP]
            hexs = " ".join(f"{b:02X}" for b in shown)
            ascii_col = "".join(
                chr(b) if 32 <= b < 127 else "." for b in shown)
            tail = ""
            if len(data) > HEX_LINE_CAP:
                tail = f"  (+ {len(data) - HEX_LINE_CAP} more)"
            self._write(slot, f"{direction} {hexs:<{HEX_LINE_CAP * 3}} | {ascii_col}{tail}")

    def event(self, slot: str, text: str) -> None:
        with self._lock:
            self._write(slot, f"* {text}")

    def close(self) -> None:
        with self._lock:
            for fh in self._files.values():
                if not fh.closed:
                    fh.close()
            self._files.clear()
