"""Bridge: the module connect port, per-slot operator ports, WS viewers.

All modules connect to one port; the option-200 MAC they announce picks their
slot. Operators connect to a slot's RW or RO port; the first one on the RW
port owns it, the rest fall back to read-only. The live viewer on the web UI
behaves like any other operator: the first viewer per slot can type, the rest
watch.
"""

from __future__ import annotations

import asyncio
import time
from typing import Dict, List, Optional

import aiohttp

from . import config, registry as reg, logs as logs_mod, telnet

QUEUE_CAP = 4096          # per-direction byte buffer, drop-oldest like the module
DROP_LOG_MS = 5000
PING_INTERVAL = 30        # how often an idle peer is probed
PING_FRAME = b"\xff\xf1"  # telnet IAC NOP: eaten by the module's filter


def _now_ms() -> int:
    return int(time.time() * 1000)


class Session:
    """One telnet peer: a socket, a filter, an outbound buffer, a pump task."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                 kind: str, bridge: "Bridge"):
        self.reader = reader
        self.writer = writer
        self.kind = kind            # "module" | "operator"
        self.bridge = bridge
        self.alive = True
        self.gone = False           # dedupe: pump and reader both notice
        self.outq = bytearray()
        self.dropped = 0
        self.drop_log_at = 0
        self.filter = telnet.TelnetFilter(
            mode="passive" if kind == "module" else "server")
        if self.filter.mode == "server":
            self.filter.mark_greeting()
            writer.write(telnet.GREETING)
        self.last_activity = _now_ms()

    def queue_out(self, data: bytes) -> None:
        """Outbound bytes for this peer, drop-oldest when the peer is slow."""
        if not self.alive:
            return
        q = self.outq
        if len(q) + len(data) > QUEUE_CAP:
            del q[:max(0, len(data))]
            self.dropped += len(data)
            self._drop_log()
        else:
            q.extend(data)

    def _drop_log(self) -> None:
        now = _now_ms()
        if now - self.drop_log_at >= DROP_LOG_MS:
            self.drop_log_at = now
            self.bridge.logs.event(self.slot_name(),
                                   f"host queue full, dropped "
                                   f"{self.dropped} bytes ({self.kind})")

    def slot_name(self) -> str:
        raise NotImplementedError

    def _peer(self) -> str:
        """The peer IP, or '?' when the socket has already gone away."""
        try:
            info = self.writer.get_extra_info("peername")
            if isinstance(info, tuple):
                return info[0]
            return str(info) if info else "?"
        except Exception:
            return "?"

    async def pump(self) -> None:
        """Moves the outbound buffer onto the socket, backing off when the peer
        cannot keep up."""
        while self.alive:
            if self.outq:
                chunk = bytes(self.outq[:QUEUE_CAP])
                del self.outq[:QUEUE_CAP]
                try:
                    self.writer.write(chunk)
                    await self.writer.drain()
                    self.last_activity = _now_ms()
                except Exception:
                    break
            else:
                await asyncio.sleep(0.05)
        self.alive = False
        self._gone()

    async def read_loop(self) -> None:
        try:
            while self.alive:
                chunk = await self.reader.read(512)
                if not chunk:
                    break
                self.last_activity = _now_ms()
                self.on_bytes(chunk)
        except Exception:
            pass
        self.alive = False
        try:
            self.writer.close()
        except Exception:
            pass
        self._gone()

    async def ping_loop(self) -> None:
        """Probes an idle peer: a socket that quietly died never gets noticed
        otherwise, so the host would keep showing a module that is not there."""
        while self.alive:
            await asyncio.sleep(PING_INTERVAL)
            if not self.alive:
                break
            now = _now_ms()
            if now - self.last_activity < PING_INTERVAL * 1000:
                continue
            self.last_activity = now
            try:
                self.writer.write(PING_FRAME)
                await self.writer.drain()
            except Exception:
                self.alive = False
                break
        if not self.alive:
            self._gone()

    def start(self) -> None:
        asyncio.create_task(self.pump())
        asyncio.create_task(self.read_loop())
        asyncio.create_task(self.ping_loop())

    def on_bytes(self, chunk: bytes) -> None:
        raise NotImplementedError

    def _gone(self) -> None:
        if self.gone:
            return
        self.gone = True
        self.bridge.on_session_gone(self)

    def close(self) -> None:
        self.alive = False
        try:
            self.writer.close()
        except Exception:
            pass


class Module:
    """One registry slot, plus the live module session if it is online."""

    def __init__(self, entry: dict):
        self.entry = entry
        self.slot = entry["name"]
        self.session: Optional["ModuleSession"] = None
        self.ops: List["OperatorSession"] = []
        self.viewers: List["ViewerSession"] = []
        self.claimed = False       # operator claim on the RW port
        self.viewer_claimed = False

    def broadcast(self, data: bytes, skip: Optional[Session] = None) -> None:
        """UART bytes arriving from the module: to operators and viewers."""
        enc = telnet.encode(QUEUE_CAP, data)
        for op in self.ops:
            if op is not skip:
                op.queue_out(enc)
        for v in self.viewers:
            if v is not skip:
                v.queue_out(data)


class ModuleSession(Session):
    """The module's client connection on the connect port."""

    def __init__(self, reader, writer, bridge):
        super().__init__(reader, writer, "module", bridge)
        self.module: Optional[Module] = None
        self.mac: Optional[bytes] = None
        self.unknown_log_at = 0

    def slot_name(self) -> str:
        return self.module.slot if self.module else "pending"

    def on_bytes(self, chunk: bytes) -> None:
        payload, _reply = self.filter.feed(chunk)
        # Passive mode: never answer, so a stateless module cannot loop.
        if self.filter.mac() and self.mac is None:
            self.mac = self.filter.mac()
            self._bind()
        if payload and self.module is None:
            # UART bytes that arrived before the MAC: not ours to keep.
            now = _now_ms()
            if now - self.unknown_log_at >= DROP_LOG_MS:
                self.unknown_log_at = now
                self.bridge.logs.event("pending",
                                       f"module bytes before identification, "
                                       f"dropped {len(payload)}")
            return
        if payload and self.module is not None:
            self.module.broadcast(payload, skip=self)
            self.bridge.logs.burst(self.module.slot, "<", payload)

    def _bind(self) -> None:
        entry = self.bridge.registry.find(self.mac)
        if entry is None:
            self.bridge.registry.note_pending(self.mac)
            self.bridge.pending_sessions[
                self.mac.decode("ascii", "ignore")] = self
            self.bridge.logs.event("pending",
                                   f"module {self.mac.decode('ascii')} "
                                   f"connected (not assigned yet)")
            self.bridge.on_pending_changed()
            return
        m = self.bridge.slot_module(entry["name"])
        if m.session is not None:
            # A second connection from the same MAC: the old one loses.
            old = m.session
            m.session = None
            old.close()
        m.session = self
        self.module = m
        self.bridge.logs.event(m.slot,
                               f"module {self.mac.decode('ascii')} connected")
        self.bridge.on_pending_changed()


class OperatorSession(Session):
    """An operator telnet client on a slot's RW or RO port."""

    def __init__(self, reader, writer, bridge, module: Module, ro: bool):
        super().__init__(reader, writer, "operator", bridge)
        self.module = module
        self.rw = False
        self.ro = ro
        if not ro and not module.claimed:
            self.rw = True
            module.claimed = True
        module.ops.append(self)
        mode = "read/write" if self.rw else "read-only"
        bridge.logs.event(module.slot,
                          f"operator {self._peer()} attached ({mode})")

    def slot_name(self) -> str:
        return self.module.slot

    def on_bytes(self, chunk: bytes) -> None:
        payload, reply = self.filter.feed(chunk)
        if reply and self.alive:
            try:
                self.writer.write(reply)      # negotiation answers, no queue
            except Exception:
                pass
        if not payload:
            return
        if not self.rw:
            return                        # read-only: typed bytes ignored
        m = self.module
        if m.session is not None:
            m.session.queue_out(telnet.encode(QUEUE_CAP, payload))
        self.bridge.logs.burst(m.slot, ">", payload)


class ViewerSession:
    """A live-viewer websocket; typed bytes behave like an operator's."""

    def __init__(self, ws, module: Module, bridge, peer: str):
        self.ws = ws
        self.module = module
        self.bridge = bridge
        self.peer = peer
        self.alive = True
        self.rw = False
        if not module.viewer_claimed:
            self.rw = True
            module.viewer_claimed = True
        mode = "read/write" if self.rw else "read-only"
        bridge.logs.event(module.slot, f"viewer {peer} attached ({mode})")

    def queue_out(self, data: bytes) -> None:
        if not self.alive:
            return
        asyncio.create_task(self._send(data))

    async def _send(self, data: bytes) -> None:
        try:
            await self.ws.send_bytes(data)
        except Exception:
            self.alive = False

    async def run(self) -> None:
        # One text frame right after the handshake tells the viewer whether it
        # may type; the host never sends another.
        try:
            await self.ws.send_str("rw" if self.rw else "ro")
            async for msg in self.ws:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    payload = msg.data.encode("utf-8", "ignore")
                    if self.rw:
                        m = self.module
                        if m.session is not None:
                            m.session.queue_out(telnet.encode(QUEUE_CAP, payload))
                        self.bridge.logs.burst(m.slot, ">", payload)
                elif msg.type == aiohttp.WSMsgType.CLOSE:
                    break
        except Exception:
            pass
        if self.rw:
            self.module.viewer_claimed = False
        self.alive = False
        try:
            self.module.viewers.remove(self)
        except ValueError:
            pass
        self.bridge.logs.event(self.module.slot, "viewer detached")


class Bridge:
    """Owns every server and session."""

    def __init__(self, cfg: config.HostConfig, registry: reg.Registry,
                 logs: logs_mod.Logs):
        self.cfg = cfg
        self.registry = registry
        self.logs = logs
        self.modules: Dict[str, Module] = {}
        self.pending_sessions: Dict[str, "ModuleSession"] = {}
        self.servers: Dict[str, object] = {}
        self.module_server = None
        self.web = None
        for entry in cfg.modules:
            self.slot_module(entry["name"])

    def slot_module(self, slot: str) -> Module:
        m = self.modules.get(slot)
        if m is None:
            entry = self.registry.entry_for_slot(slot)
            m = Module(entry)
            self.modules[slot] = m
            asyncio.create_task(self._listen(m))
        return m

    def on_pending_changed(self) -> None:
        """Hook for the web UI to re-render; no-op unless web is attached."""
        if self.web is not None:
            self.web.pending_dirty()

    def on_session_gone(self, s: Session) -> None:
        if isinstance(s, ModuleSession):
            if s.module is not None and s.module.session is s:
                s.module.session = None
                self.logs.event(s.module.slot, "module went away")
            elif s.module is None and s.mac is not None:
                self.registry.drop_pending(s.mac)
                self.pending_sessions.pop(s.mac.decode("ascii", "ignore"), None)
                self.logs.event("pending",
                                f"module {s.mac.decode('ascii')} went away")
                self.on_pending_changed()
        elif isinstance(s, OperatorSession):
            try:
                s.module.ops.remove(s)
            except ValueError:
                pass
            if s.rw:
                s.module.claimed = False
            self.logs.event(s.module.slot,
                            f"operator {s._peer()} detached")

    async def _listen(self, m: Module) -> None:
        for port, ro in ((m.entry["rw"], False), (m.entry["ro"], True)):
            try:
                srv = await asyncio.start_server(
                    lambda r, w, ro=ro: self._op_conn(r, w, m, ro),
                    host="0.0.0.0", port=port)
                self.servers[f"{m.slot}:{'ro' if ro else 'rw'}"] = srv
            except OSError as e:
                self.logs.event(m.slot,
                                f"bridge port {port} unavailable: {e}")

    def _op_conn(self, reader, writer, m: Module, ro: bool) -> None:
        OperatorSession(reader, writer, self, m, ro).start()

    async def _module_conn(self, reader, writer) -> None:
        ModuleSession(reader, writer, self).start()

    async def start(self) -> None:
        self.module_server = await asyncio.start_server(
            self._module_conn, host="0.0.0.0", port=self.cfg.module_port)

    async def rebind_module_port(self, port: int) -> None:
        """The connect port can move without a restart: close the old listener
        and open a new one on the same loop."""
        if getattr(self, "module_server", None) is not None:
            self.module_server.close()
            await self.module_server.wait_closed()
        self.module_server = await asyncio.start_server(
            self._module_conn, host="0.0.0.0", port=port)

    def bind_pending(self, entry: dict) -> None:
        """A module that was pending gets a slot: re-bind its live session so
        it starts flowing without a reconnect."""
        s = self.pending_sessions.pop(entry["mac"], None)
        if s is not None and s.alive:
            s._bind()

    def module_for_viewer(self, slot: str) -> Optional[Module]:
        return self.modules.get(slot)
