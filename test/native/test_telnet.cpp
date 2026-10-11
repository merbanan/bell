// Tests for the telnet option negotiation in src/telnet_filter.h.
//
// A bridge that gets this wrong does not fail visibly: it just hands 0xFF
// bytes to the UART in the wrong places, or leaves a terminal in go-ahead mode
// where every third line arrives with junk in front of it. So the negotiation
// is pinned down byte for byte here, including the case that matters most -
// a payload that contains 0xFF.

#include <stdio.h>
#include <string.h>
#include <vector>

#include "telnet_filter.h"

static int g_fail = 0;

static void fail(const char *what, const std::vector<uint8_t> &got,
                 const std::vector<uint8_t> &want) {
    printf("  FAIL %s\n    got :", what);
    for (uint8_t b : got) printf(" %02X", b);
    printf("\n    want:");
    for (uint8_t b : want) printf(" %02X", b);
    printf("\n");
    g_fail++;
}

static void expect(const char *what, const std::vector<uint8_t> &got,
                   const std::vector<uint8_t> &want) {
    if (got == want) return;
    fail(what, got, want);
}

struct Result {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> reply;
};

static Result run(TelnetFilter &f, const std::vector<uint8_t> &in) {
    uint8_t out[1024], reply[64];
    size_t rlen = 0;
    size_t olen = f.feed(in.data(), in.size(), out, sizeof(out), reply, sizeof(reply), &rlen);
    Result r;
    r.payload.assign(out, out + olen);
    r.reply.assign(reply, reply + rlen);
    return r;
}

// Convenience: feed a whole string as one chunk.
static Result run(TelnetFilter &f, const char *in) {
    return run(f, std::vector<uint8_t>(in, in + strlen(in)));
}

static std::vector<uint8_t> bytes(std::initializer_list<int> l) {
    std::vector<uint8_t> v;
    for (int b : l) v.push_back((uint8_t)b);
    return v;
}

int main() {
    printf("telnet negotiation tests\n");

    // Plain payload passes through untouched.
    {
        TelnetFilter f; f.begin(false);
        expect("plain text", run(f, "AT\r\n").payload, bytes({'A', 'T', 13, 10}));
        expect("no reply", run(f, "x").reply, {});
    }

    // 0xFF must be sent as IAC IAC and delivered as a single 0xFF.
    {
        TelnetFilter f; f.begin(false);
        expect("escaped 0xFF", run(f, bytes({0x41, 0xFF, 0x42})).payload,
               bytes({0x41, 0xFF, 0x42}));
        // IAC IAC is the RFC escape for one 0xFF, so it decodes to one byte.
        expect("doubled 0xFF", run(f, bytes({0xFF, 0xFF})).payload, bytes({0xFF}));
    }

    // DO SGA / DO BINARY / DO ECHO -> WILL, anything else -> WONT.
    {
        TelnetFilter f; f.begin(false);
        expect("DO SGA", run(f, bytes({255, 253, 3})).reply, bytes({255, 251, 3}));
        expect("DO BINARY", run(f, bytes({255, 253, 0})).reply, bytes({255, 251, 0}));
        // ECHO is the server's job on a serial line, so it is taken over
        // rather than refused - this is what ser2net answers too.
        expect("DO ECHO", run(f, bytes({255, 253, 1})).reply, bytes({255, 251, 1}));
        expect("DO TTYPE", run(f, bytes({255, 253, 24})).reply, bytes({255, 252, 24}));
    }

    // WILL BINARY -> DO BINARY (and binary mode latches), WILL ECHO -> DONT ECHO.
    {
        TelnetFilter f; f.begin(false);
        Result r = run(f, bytes({255, 251, 0}));
        expect("WILL BINARY reply", r.reply, bytes({255, 253, 0}));
        if (!f.binary()) { printf("  FAIL binary mode not latched\n"); g_fail++; }

        expect("WILL ECHO", run(f, bytes({255, 251, 1})).reply, bytes({255, 254, 1}));
        expect("WILL SGA", run(f, bytes({255, 251, 3})).reply, bytes({255, 253, 3}));
        expect("WILL NAWS", run(f, bytes({255, 251, 31})).reply, bytes({255, 253, 31}));
        expect("WILL LINEMODE", run(f, bytes({255, 251, 34})).reply, bytes({255, 254, 34}));

        run(f, bytes({255, 252, 0}));            // WONT BINARY
        if (f.binary()) { printf("  FAIL binary mode not cleared\n"); g_fail++; }
    }

    // Subnegotiation is swallowed: TTYPE and NAWS are the usual preamble.
    {
        TelnetFilter f; f.begin(false);
        expect("SB TTYPE", run(f, bytes({255, 250, 24, 0, 'x', 'y', 255, 240})).payload, {});
        expect("SB NAWS", run(f, bytes({255, 250, 31, 3, 0, 240, 200, 80, 255, 240})).payload, {});
        expect("after SB", run(f, "OK").payload, bytes({'O', 'K'}));
    }

    // A negotiation split across two reads must behave like one read.
    {
        TelnetFilter whole, split;
        whole.begin(false);
        split.begin(false);
        std::vector<uint8_t> stream = bytes({0x41, 255, 251, 0, 0x42, 255, 255, 0x43});
        Result a = run(whole, stream);

        Result b1 = run(split, std::vector<uint8_t>(stream.begin(), stream.begin() + 3));
        Result b2 = run(split, std::vector<uint8_t>(stream.begin() + 3, stream.end()));
        std::vector<uint8_t> payload = b1.payload;
        payload.insert(payload.end(), b2.payload.begin(), b2.payload.end());
        std::vector<uint8_t> reply = b1.reply;
        reply.insert(reply.end(), b2.reply.begin(), b2.reply.end());

        expect("split read payload", payload, a.payload);
        expect("split read reply", reply, a.reply);
    }

    // A 0xFF that is not doubled cannot be an escape - nothing follows it that
    // could be a command - so both bytes are passed through instead of the
    // stream being silently truncated.
    {
        TelnetFilter f; f.begin(false);
        expect("undoubled 0xFF", run(f, bytes({0x41, 255, 0x42})).payload,
               bytes({0x41, 255, 0x42}));
        expect("0xFF before a command", run(f, bytes({255, 0x41})).payload,
               bytes({255, 0x41}));
        expect("lone 0xFF stays pending", run(f, bytes({0x41, 255})).payload,
               bytes({0x41}));
        expect("...and completes on the next read", run(f, bytes({0x42})).payload,
               bytes({255, 0x42}));
    }

    // CRLF translation, for the many devices that need CR before LF.
    {
        TelnetFilter f; f.begin(true);
        expect("LF becomes CRLF", run(f, "a\nb").payload, bytes({'a', 13, 10, 'b'}));
        expect("CR is left alone", run(f, "a\r\nb").payload, bytes({'a', 13, 13, 10, 'b'}));

        f.setCrlfToCrlf(false);
        expect("translation off", run(f, "a\nb").payload, bytes({'a', 10, 'b'}));
    }

    // Binary payload with negotiation mixed in must survive untouched.
    {
        TelnetFilter f; f.begin(false);
        std::vector<uint8_t> payload;
        for (int i = 0; i < 256; i++) payload.push_back((uint8_t)i);
        std::vector<uint8_t> stream;
        stream.push_back(255); stream.push_back(251); stream.push_back(0);
        for (uint8_t b : payload) {
            if (b == 255) stream.push_back(255);
            stream.push_back(b);
        }
        Result r = run(f, stream);
        expect("all 256 byte values", r.payload, payload);
    }

    // The way out. A 0xFF sent bare is read by the client as the start of a
    // command, and the byte after it is swallowed - which is how a serial port
    // stops being a serial port. IAC has to be doubled, in binary mode too.
    {
        TelnetFilter f; f.begin(false);

        std::vector<uint8_t> in;
        for (int i = 0; i < 256; i++) in.push_back((uint8_t)i);

        std::vector<uint8_t> out(512);
        size_t n = f.encode(out.data(), out.size(), in.data(), in.size());

        std::vector<uint8_t> want;
        for (uint8_t b : in) { want.push_back(b); if (b == 255) want.push_back(255); }

        expect("every byte escapes to the wire", std::vector<uint8_t>(out.begin(), out.begin() + n), want);

        // Decoding what came out has to give the payload back unchanged.
        TelnetFilter back; back.begin(false);
        std::vector<uint8_t> stream(out.begin(), out.begin() + n);
        expect("round trip through the wire", run(back, stream).payload, in);

        // Binary mode does not relax this: BINARY is about the eighth bit.
        TelnetFilter bin; bin.begin(false);
        std::vector<uint8_t> bout(8);
        size_t bn = bin.encode(bout.data(), bout.size(),
                               (const uint8_t *)"\xff" "A", 2);
        expect("a lone 0xFF still doubles",
               std::vector<uint8_t>(bout.begin(), bout.begin() + bn),
               bytes({255, 255, 'A'}));

        // A short buffer must not leave half an escape behind.
        std::vector<uint8_t> tight(3);
        size_t tn = bin.encode(tight.data(), tight.size(),
                               (const uint8_t *)"\xff" "\xff", 2);
        expect("never splits an escaped pair",
               std::vector<uint8_t>(tight.begin(), tight.begin() + tn),
               bytes({255, 255}));

        // ECHO belongs to the server, so a client asking for it is answered
        // WILL and a client offering it is told not to.
        TelnetFilter e; e.begin(false);
        Result r = run(e, bytes({255, 253, 1}));
        expect("DO ECHO answered WILL", r.reply, bytes({255, 251, 1}));
        r = run(e, bytes({255, 251, 1}));
        expect("WILL ECHO answered DONT", r.reply, bytes({255, 254, 1}));
    }

    printf(g_fail ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
