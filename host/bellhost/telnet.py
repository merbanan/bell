"""Telnet framing for the console-server host.

Two filters, both mirroring the firmware's TelnetFilter byte for byte:

  * ``module``  - the filter on a module connection. Strips the telnet framing
    the module sends, captures the option-200 MAC subnegotiation the module
    announces, and answers nothing: the module plays the client role and only
    ever reacts to commands, so silence keeps the negotiation from looping.
  * ``operator`` - the filter on an operator connection (telnet client or the
    live viewer). Same semantics as the firmware's telnet server, plus
    suppression of duplicate answers, so a stateless peer cannot pull the
    negotiation into a loop.

``encode`` doubles every 0xFF on the way out, in both directions: BINARY is
about the eighth bit, never about IAC.
"""

from __future__ import annotations

from typing import Optional

OPT_BINARY = 0
OPT_ECHO = 1
OPT_SGA = 3
OPT_TTYPE = 24
OPT_NAWS = 31
OPT_MAC = 200

IAC = 255
DONT = 254
DO = 253
WONT = 252
WILL = 251
SB = 250
SE = 240

# RFC 854 puts the option number first in a subnegotiation; the filter keeps
# the option in a byte so option 200 can be recognised without swallowing
# anything twice.
GREETING = bytes([
    IAC, WILL, OPT_SGA,
    IAC, WILL, OPT_ECHO,
    IAC, DONT, OPT_ECHO,
    IAC, DO,   OPT_BINARY,
])

MAC_CAP = 24
SUBNEG_MAX = 255


class TelnetFilter:
    """Turns a raw telnet byte stream into payload bytes.

    ``feed`` returns (payload, reply). ``payload`` is the UART-bound byte
    stream with the telnet framing stripped off; ``reply`` is what the peer
    expects to hear back. ``mode`` decides which of the two the filter is.
    """

    __slots__ = ("mode", "_state", "_pending", "_subopt", "_subopt_seen",
                 "_sublen", "_binary", "_mac", "_mac_ready", "_replied")

    DATA = 0
    IAC_CMD = 1
    OPTION = 2
    SUBNEG = 3
    SUBNEG_IAC = 4

    def __init__(self, mode: str = "server"):
        self.mode = mode
        self._state = self.DATA
        self._pending = None
        self._subopt = None
        self._subopt_seen = False
        self._sublen = 0
        self._binary = False
        self._mac: Optional[bytes] = None
        self._mac_ready = False
        self._replied: dict[tuple[int, int], int] = {}

    def binary(self) -> bool:
        return self._binary

    def mac(self) -> Optional[bytes]:
        """The MAC the module announced, once the terminator was seen."""
        return self._mac if self._mac_ready else None

    def feed(self, data: bytes) -> tuple[bytes, bytes]:
        payload = bytearray()
        reply = bytearray()

        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            i += 1
            st = self._state

            if st == self.DATA:
                if b == IAC:
                    self._state = self.IAC_CMD
                else:
                    payload.append(b)

            elif st == self.IAC_CMD:
                if b == IAC:
                    self._state = self.DATA
                    payload.append(IAC)
                elif b in (WILL, WONT, DO, DONT):
                    self._pending = b
                    self._state = self.OPTION
                elif b == SB:
                    self._state = self.SUBNEG
                    self._subopt_seen = False
                    self._sublen = 0
                else:
                    # Not a command, so this 0xFF was never an escape: pass
                    # both bytes on rather than losing them.
                    self._state = self.DATA
                    payload.append(IAC)
                    payload.append(b)

            elif st == self.OPTION:
                self._negotiate(b, reply)
                self._state = self.DATA

            elif st == self.SUBNEG:
                if b == IAC:
                    self._state = self.SUBNEG_IAC
                    self._sublen = 0
                elif not self._subopt_seen:
                    self._subopt = b
                    self._subopt_seen = True
                    self._sublen += 1
                    if self._sublen > SUBNEG_MAX:
                        self._state = self.DATA
                else:
                    if self._subopt == OPT_MAC and len(self._mac or b"") < MAC_CAP:
                        prev = self._mac if self._mac is not None else b""
                        self._mac = prev + bytes([b])
                    self._sublen += 1
                    if self._sublen > SUBNEG_MAX:
                        self._state = self.DATA

            elif st == self.SUBNEG_IAC:
                if b == SE:
                    self._state = self.DATA
                    if self._subopt == OPT_MAC and self._mac:
                        self._mac_ready = True
                else:
                    self._state = self.SUBNEG

        return bytes(payload), bytes(reply)

    def _negotiate(self, opt: int, reply: bytearray) -> None:
        pending = self._pending
        if self.mode == "passive":
            # Silence: the module only ever reacts to commands, and a reply
            # from us would bounce right back.
            if pending == WILL and opt == OPT_BINARY:
                self._binary = True
            elif pending in (WONT, DONT) and opt == OPT_BINARY:
                self._binary = False
            return

        verdict = self._decide(pending, opt)
        if verdict is None:
            return
        key = (pending, opt)
        if self._replied.get(key) == verdict:
            return                      # already told them; do not loop
        self._replied[key] = verdict
        reply.append(IAC)
        reply.append(verdict)
        reply.append(opt)

    def _decide(self, pending: int, opt: int) -> Optional[int]:
        """Same answers the firmware gives, so a module sees the host behave
        exactly like its own telnet server."""
        if pending == DO:
            if opt in (OPT_SGA, OPT_BINARY, OPT_ECHO):
                return WILL
            return WONT
        if pending == WILL:
            if opt == OPT_BINARY:
                self._binary = True
                return DO
            if opt == OPT_SGA:
                return DO
            if opt == OPT_ECHO:
                return DONT
            if opt == OPT_NAWS:
                return DO                     # accepted, then ignored
            return DONT
        if pending in (WONT, DONT):
            if opt == OPT_BINARY:
                self._binary = False
            return None
        return None

    def mark_greeting(self) -> None:
        """The greeting has already been sent as raw bytes, so the answers to
        the commands a client sends in response are suppressed - without this
        a stateless peer would loop forever."""
        self._replied[(WILL, OPT_SGA)] = WILL
        self._replied[(DO,   OPT_ECHO)] = WILL
        self._replied[(WILL, OPT_ECHO)] = DONT
        self._replied[(DO,   OPT_BINARY)] = DO


def encode(out_cap: int, data: bytes) -> bytes:
    """Escapes 0xFF for the wire: every payload byte that is IAC becomes IAC
    IAC. Returns at most ``out_cap`` bytes."""
    out = bytearray()
    for b in data:
        if len(out) + 2 > out_cap:
            break
        out.append(b)
        if b == IAC:
            out.append(IAC)
    return bytes(out)


def greeting_reply_expectations() -> dict[tuple[int, int], int]:
    """The verdicts a client is expected to answer the greeting with, used by
    the smoke tests."""
    return {
        (WILL, OPT_SGA): DO,
        (DO,   OPT_ECHO): WONT,
        (DO,   OPT_BINARY): WILL,
    }
