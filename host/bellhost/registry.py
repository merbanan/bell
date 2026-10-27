"""Module registry: MAC to slot mapping, pending modules, port lookup."""

from __future__ import annotations

from typing import Dict, Optional

from . import config


class Registry:
    """Keeps the YAML registry in memory; writes back whenever a module is
    assigned a slot."""

    def __init__(self, cfg: config.HostConfig):
        self.cfg = cfg
        self.pending: Dict[str, dict] = {}     # mac -> {connected, first_seen}
        self.pending_order: list[str] = []

    def find(self, mac: bytes) -> Optional[dict]:
        s = mac.decode("ascii", "ignore")
        for m in self.cfg.modules:
            if m["mac"] == s:
                return m
        return None

    def entry_for_slot(self, slot: str) -> Optional[dict]:
        for m in self.cfg.modules:
            if m["name"] == slot:
                return m
        return None

    def note_pending(self, mac: bytes) -> None:
        s = mac.decode("ascii", "ignore")
        if s not in self.pending:
            self.pending[s] = {"connected": True, "first_seen": None}
            self.pending_order.append(s)
        self.pending[s]["connected"] = True

    def drop_pending(self, mac: bytes) -> None:
        s = mac.decode("ascii", "ignore")
        if s in self.pending:
            self.pending[s]["connected"] = False

    def assign(self, mac: bytes, name: str) -> dict:
        entry = config.add_module(self.cfg, mac.decode("ascii", "ignore"), name)
        s = mac.decode("ascii", "ignore")
        self.pending.pop(s, None)
        try:
            self.pending_order.remove(s)
        except ValueError:
            pass
        return entry

    def slots(self) -> list[str]:
        return [m["name"] for m in self.cfg.modules]
