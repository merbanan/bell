#pragma once

#include <stddef.h>
#include <stdint.h>

// RFC 854 option and command codes. Only the handful that matter for passing a
// byte stream through untouched are handled; everything else is refused.
namespace telnet {
enum : uint8_t {
    OPT_BINARY = 0,
    OPT_ECHO   = 1,
    OPT_SGA    = 3,
    OPT_TTYPE  = 24,
    OPT_NAWS   = 31,

    IAC  = 255,
    DONT = 254,
    DO   = 253,
    WONT = 252,
    WILL = 251,
    SB   = 250,
    SE   = 240,
};
}

// Turns a raw telnet byte stream into the payload the bridge forwards, and
// collects the option replies the far end expects. Kept free of any WiFi or
// Arduino dependency so the negotiation can be exercised on a workstation -
// getting IAC handling wrong silently corrupts a serial stream, which is the
// one thing this firmware must never do.
class TelnetFilter {
public:
    void begin(bool crlfToCrlf) {
        _crlf    = crlfToCrlf;
        _state   = DATA;
        _pending = 0;
        _binary  = false;
        _subLen  = 0;
    }

    void setCrlfToCrlf(bool on) { _crlf = on; }
    bool binary() const { return _binary; }

    // The output direction: a 0xFF in the payload has to reach the wire as IAC
    // IAC, because the far end would otherwise read it as the start of a
    // command and swallow the byte after it.
    //
    // BINARY is about the eighth bit, not about IAC, so this is required in
    // both modes - ser2net doubles its IACs the same way, and sends only half
    // of each buffer from the device for exactly this reason.
    //
    // Returns how many bytes were written to `out`.
    size_t encode(uint8_t *out, size_t cap, const uint8_t *in, size_t n) const {
        size_t o = 0;
        for (size_t i = 0; i < n; i++) {
            if (o + 2 > cap) break;
            out[o++] = in[i];
            if (in[i] == telnet::IAC) out[o++] = telnet::IAC;
        }
        return o;
    }

    // Consumes `n` bytes and writes the payload into `out` (capped at
    // outCap) and any negotiation replies into `reply` (3 bytes per option,
    // whose length is returned through `replyLen`). Returns the number of
    // payload bytes produced.
    size_t feed(const uint8_t *in, size_t n,
                uint8_t *out, size_t outCap,
                uint8_t *reply, size_t replyCap, size_t *replyLen) {
        size_t used = 0;
        size_t replyLenLocal = 0;

        for (size_t i = 0; i < n; i++) {
            uint8_t b = in[i];
            switch (_state) {
                case DATA:
                    if (b == telnet::IAC) { _state = IAC_CMD; break; }
                    emit(out, outCap, used, b);
                    break;

                case IAC_CMD:
                    if (b == telnet::IAC) {          // escaped 0xFF
                        _state = DATA;
                        emit(out, outCap, used, telnet::IAC);
                    } else if (b == telnet::WILL || b == telnet::WONT ||
                               b == telnet::DO   || b == telnet::DONT) {
                        _pending = b;
                        _state = OPTION;
                    } else if (b == telnet::SB) {
                        _state = SUBNEG;
                    } else {
                        // Not a command, so this 0xFF was never an escape: it is
                        // a data byte the peer failed to double. Pass both bytes
                        // on rather than losing them - a strict reading of
                        // RFC 854 would drop the pair, but a serial bridge that
                        // keeps working for a sloppy client is worth more.
                        _state = DATA;
                        emit(out, outCap, used, telnet::IAC);
                        emit(out, outCap, used, b);
                    }
                    break;

                case OPTION:
                    negotiate(b, reply, replyCap, replyLenLocal);
                    _state = DATA;
                    break;

                case SUBNEG:
                    if (b == telnet::IAC) { _state = SUBNEG_IAC; _subLen = 0; }
                    // A peer that opens a subnegotiation and never closes it
                    // would otherwise swallow the payload for good.
                    else if (++_subLen > 255) _state = DATA;
                    break;

                case SUBNEG_IAC:
                    _state = (b == telnet::SE) ? DATA : SUBNEG;   // TTYPE, NAWS
                    break;
            }
        }
        if (replyLen) *replyLen = replyLenLocal;
        return used;
    }

private:
    enum State { DATA, IAC_CMD, OPTION, SUBNEG, SUBNEG_IAC };

    // Appends a payload byte, inserting the CR that many devices want in front
    // of a bare LF.
    void emit(uint8_t *out, size_t cap, size_t &used, uint8_t b) {
        if (_crlf && b == '\n') {
            if (used < cap) out[used++] = '\r';
        }
        if (used < cap) out[used++] = b;
    }

    static void cmd(uint8_t *reply, size_t cap, size_t &len, uint8_t verb, uint8_t opt) {
        if (len + 3 > cap) return;
        reply[len++] = telnet::IAC;
        reply[len++] = verb;
        reply[len++] = opt;
    }

    void negotiate(uint8_t opt, uint8_t *reply, size_t cap, size_t &len) {
        switch (_pending) {
            case telnet::DO:
                // Suppress go-ahead, pass binary data through, and take echo
                // over from the client: a serial line has no local echo, and
                // ser2net answers DO ECHO the same way so that the terminal
                // stops duplicating what it sends. Refuse the rest.
                if (opt == telnet::OPT_SGA || opt == telnet::OPT_BINARY ||
                    opt == telnet::OPT_ECHO)       cmd(reply, cap, len, telnet::WILL, opt);
                else                                cmd(reply, cap, len, telnet::WONT, opt);
                break;

            case telnet::WILL:
                if (opt == telnet::OPT_BINARY) {
                    _binary = true;
                    cmd(reply, cap, len, telnet::DO, telnet::OPT_BINARY);
                } else if (opt == telnet::OPT_SGA) {
                    cmd(reply, cap, len, telnet::DO, telnet::OPT_SGA);
                } else if (opt == telnet::OPT_ECHO) {
                    // We are the ones echoing now, so the client must not.
                    cmd(reply, cap, len, telnet::DONT, telnet::OPT_ECHO);
                } else if (opt == telnet::OPT_NAWS) {
                    cmd(reply, cap, len, telnet::DO, telnet::OPT_NAWS);   // accepted, then ignored
                } else {
                    cmd(reply, cap, len, telnet::DONT, opt);
                }
                break;

            default:   // WONT or DONT
                if (opt == telnet::OPT_BINARY) _binary = false;
                break;
        }
    }

    State   _state   = DATA;
    uint8_t _pending = 0;
    uint8_t _subLen  = 0;
    bool    _crlf    = false;
    bool    _binary  = false;
};
