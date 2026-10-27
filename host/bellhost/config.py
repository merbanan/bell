"""Configuration for the console-server host.

Everything lives in one YAML file. Missing keys fall back to defaults, so a
fresh checkout works without editing anything; the host only ever writes back
what it has to change (port allocations, new modules).
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import List, Optional

import yaml


DEFAULT_MODULE_PORT = 12345
DEFAULT_BRIDGE_BASE = 12360
DEFAULT_WEB_PORT = 8080
DEFAULT_STORAGE = "./logs"


def _mac_ok(mac: str) -> bool:
    """17 characters, AA:BB:CC:DD:EE:FF."""
    if len(mac) != 17 or mac[2::3] != ":::::":
        return False
    parts = mac.split(":")
    if len(parts) != 6:
        return False
    for p in parts:
        if len(p) != 2:
            return False
        try:
            int(p, 16)
        except ValueError:
            return False
    return True


@dataclass
class HostConfig:
    path: str = "config.yaml"
    module_port: int = DEFAULT_MODULE_PORT
    bridge_base: int = DEFAULT_BRIDGE_BASE
    web_port: int = DEFAULT_WEB_PORT
    storage: str = DEFAULT_STORAGE
    modules: List[dict] = field(default_factory=list)


def _port(v) -> Optional[int]:
    try:
        p = int(v)
    except (TypeError, ValueError):
        return None
    return p if 1 <= p <= 65535 else None


def load(path: str = "config.yaml") -> HostConfig:
    cfg = HostConfig(path=path)
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = yaml.safe_load(fh) or {}
    except FileNotFoundError:
        return cfg
    except yaml.YAMLError as e:
        raise SystemExit(f"{path}: {e}")

    h = doc.get("host") or {}
    cfg.module_port = _port(h.get("modulePort")) or DEFAULT_MODULE_PORT
    cfg.bridge_base = _port(h.get("bridgeBase")) or DEFAULT_BRIDGE_BASE
    cfg.web_port = _port(h.get("webPort")) or DEFAULT_WEB_PORT
    cfg.storage = h.get("storage") or DEFAULT_STORAGE

    mods = doc.get("modules") or []
    out: List[dict] = []
    for m in mods:
        if not isinstance(m, dict):
            continue
        mac = str(m.get("mac", "")).strip()
        name = str(m.get("name", "")).strip()
        rw = _port(m.get("rw"))
        ro = _port(m.get("ro"))
        if not _mac_ok(mac) or not name or rw is None or ro is None:
            continue
        out.append({"mac": mac, "name": name, "rw": rw, "ro": ro})
    cfg.modules = out
    return cfg


def save(cfg: HostConfig) -> None:
    """Writes the file back. Only the fields the host manages are touched:
    module ports, bridge ports, new modules."""
    doc = {"host": {
        "modulePort": cfg.module_port,
        "bridgeBase": cfg.bridge_base,
        "webPort": cfg.web_port,
        "storage": cfg.storage,
    }, "modules": list(cfg.modules)}
    with open(cfg.path, "w", encoding="utf-8") as fh:
        yaml.safe_dump(doc, fh, default_flow_style=False, sort_keys=False)


def storage_dir(cfg: HostConfig) -> str:
    """Directory logs are written to, created on demand."""
    d = cfg.storage
    if not os.path.isabs(d):
        d = os.path.join(os.path.dirname(os.path.abspath(cfg.path)), d)
    return d


def occupied_ports(cfg: HostConfig) -> set:
    """Every port the host has a reason to avoid when handing out a pair."""
    occ = {cfg.module_port, cfg.web_port}
    for m in cfg.modules:
        occ.add(m["rw"])
        occ.add(m["ro"])
    return occ


def allocate_pair(cfg: HostConfig) -> tuple[int, int]:
    """Lowest free (rw, ro=rw+1) pair at or above the base."""
    occ = occupied_ports(cfg)
    rw = cfg.bridge_base
    while True:
        if rw in occ or rw + 1 in occ or rw == cfg.web_port or rw + 1 == cfg.web_port:
            rw += 1
            continue
        return rw, rw + 1


def add_module(cfg: HostConfig, mac: str, name: str) -> dict:
    """Assigns a pending module: name it, hand out a port pair, remember it."""
    if not _mac_ok(mac):
        raise ValueError(f"bad mac {mac!r}")
    if not name or not name.strip():
        raise ValueError("module needs a name")
    name = name.strip()
    for m in cfg.modules:
        if m["mac"] == mac:
            raise ValueError(f"module {mac} is already registered")
        if m["name"] == name:
            raise ValueError(f"name {name!r} is already taken")
    rw, ro = allocate_pair(cfg)
    entry = {"mac": mac, "name": name, "rw": rw, "ro": ro}
    cfg.modules.append(entry)
    save(cfg)
    return entry
