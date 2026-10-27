#include "telnet.h"
#include "config.h"
#include "log.h"
#include "uart.h"

#include <ESP8266WiFi.h>

#include "telnet_filter.h"

// Sent the moment a client connects, before it says anything. This is the
// sequence ser2net opens with, for the same reason it uses one:
//
//   WILL SGA    no go-ahead prompts
//   WILL ECHO   the server echoes, so the client stops echoing locally and the
//               session behaves like a serial line rather than a chat
//   DONT ECHO   ...and explicitly refuses to be echoed by the client
//   DO BINARY   take the eighth bit as it comes
static const uint8_t GREETING[] = {
    telnet::IAC, telnet::WILL, telnet::OPT_SGA,
    telnet::IAC, telnet::WILL, telnet::OPT_ECHO,
    telnet::IAC, telnet::DONT, telnet::OPT_ECHO,
    telnet::IAC, telnet::DO,   telnet::OPT_BINARY
};

// loop() runs on a 4 kB stack, so nothing on that path may declare a buffer
// big enough to matter. Each session therefore owns its scratch space, taken
// from the heap when a client connects and handed back when it goes away - an
// idle bridge pays nothing for the sessions it could serve.
#define TC_SCRATCH_BYTES (LOG_BATCH * 3 + 32)

struct TClient {
    WiFiClient sock;
    TelnetFilter filter;

    uint8_t   *buf;       // one block: rx | out | reply
    uint8_t   *rx;        // LOG_BATCH
    uint8_t   *out;       // 2 * LOG_BATCH, room for CRLF expansion
    uint8_t   *reply;     // option negotiations, 3 bytes each
    uint8_t   *tx;        // TC_TX_BYTES, the transmit ring for UART data

    uint16_t   head;      // ring write cursor
    uint16_t   tail;      // ring read cursor
    uint16_t   queued;
    bool       used;
    bool       readOnly;  // on the monitor port: watches, never types
    uint32_t   rxBytes;
    uint32_t   txBytes;
    uint32_t   ignored;   // bytes a read-only client offered and we dropped

    // Snapshotted at accept time: once the peer half-closes, lwIP releases the
    // pcb and sock.remoteIP() starts answering 0.0.0.0.
    IPAddress  ip;
};

static TClient       s_cl[MAX_TELNET_CLIENTS];
static WiFiServer   *s_srv = nullptr;      // read/write
static WiFiServer   *s_srvRo = nullptr;    // read only

// The monitor port follows the working one unless it has been set to its own
// number, so there is a setting for the unusual case and nothing to keep in
// step in the usual one.
uint16_t telnetReadOnlyPort() {
    uint32_t p = g_cfg.telnetPortRo ? (uint32_t)g_cfg.telnetPortRo
                                    : (uint32_t)g_cfg.telnetPort + 1;
    if (p > 65535 || p == WEB_PORT || p == g_cfg.telnetPort) return 0;
    return (uint16_t)p;
}

static TClient *firstFree() {
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++)
        if (!s_cl[i].used) return &s_cl[i];
    return nullptr;
}

static void releaseClient(TClient *c) {
    free(c->buf); c->buf = c->rx = c->out = c->reply = nullptr;
    free(c->tx);  c->tx = nullptr;
    c->queued = c->head = c->tail = 0;
}

static void dropClient(TClient *c, const char *why) {
    if (!c || !c->used) return;
    SYSLOG("[telnet] %s%s disconnected (%s), %u rx / %u tx%s",
           c->ip.toString().c_str(), c->readOnly ? " (read only)" : "", why,
           (unsigned)c->rxBytes, (unsigned)c->txBytes,
           c->ignored ? " / ignored" : "");
    c->sock.stop();
    c->used = false;
    releaseClient(c);
}

// Drain a monitor port client. Whatever it sends is thrown away: it is here to
// watch the line, and letting its bytes reach the UART would make "read only"
// a claim rather than a fact. They are still counted so an operator can see
// that a client tried to type.
static void drainRx(TClient *c) {
    uint8_t sink[64];
    while (c->sock.available() > 0) {
        size_t n = c->sock.read(sink, sizeof(sink));
        if (n == 0) break;
        c->ignored += n;
    }
}

// Pull payload bytes out of the socket, strip the telnet framing and hand the
// rest to the UART in batches.
static void pumpRx(TClient *c) {
    if (c->readOnly) { drainRx(c); return; }

    while (c->sock.available() > 0) {
        size_t n = c->sock.read(c->rx, LOG_BATCH);
        if (n == 0) break;

        size_t rlen = 0;
        size_t olen = c->filter.feed(c->rx, n, c->out, TC_SCRATCH_BYTES - LOG_BATCH,
                                     c->reply, TC_SCRATCH_BYTES - LOG_BATCH * 3, &rlen);
        if (rlen) c->sock.write(c->reply, rlen);
        if (!olen) continue;

        c->rxBytes += olen;
        if (g_cfg.localEcho) {
            // Echo is for the operator's benefit only; it never re-enters the
            // payload stream.
            for (size_t i = 0; i < olen; i++) c->sock.write(c->out[i]);
        }
        uartWrite(c->out, olen);
    }
}

static void pumpTx(TClient *c) {
    uint8_t out[64];

    while (c->queued && c->sock.availableForWrite() > 0) {
        uint16_t room = (uint16_t)(TC_TX_BYTES - c->tail);
        uint16_t n    = c->queued < room ? c->queued : room;

        uint16_t o = (uint16_t)c->filter.encode(out, sizeof(out),
                                                c->tx + c->tail, n);
        if (!o) break;

        size_t w = c->sock.write(out, o);
        if (w == 0) break;

        // A short write can stop part way through an escaped pair, so count how
        // many queued bytes the bytes actually written account for rather than
        // assuming one byte in, one byte out. An IAC that lost its second half
        // is left in the ring for the next pass, which re-encodes it complete.
        uint16_t used = 0, acc = 0;
        while (used < n) {
            uint16_t need = (c->tx[c->tail + used] == telnet::IAC) ? 2 : 1;
            if (acc + need > w) break;
            acc += need;
            used++;
        }

        c->tail = (uint16_t)((c->tail + used) % TC_TX_BYTES);
        c->queued = (uint16_t)(c->queued - used);
        c->txBytes += used;
    }
}

static void serviceClients() {
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++) {
        TClient *c = &s_cl[i];
        if (!c->used) continue;
        if (!c->sock.connected()) { dropClient(c, "closed"); continue; }
        pumpTx(c);
        pumpRx(c);
    }
}

// Index of the client with the least room left, or -1 when everything fits.
static int fullest() {
    int worst = -1;
    uint16_t min = TC_TX_BYTES;
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++) {
        if (!s_cl[i].used) continue;
        uint16_t room = (uint16_t)(TC_TX_BYTES - s_cl[i].queued);
        if (room < min) { min = room; worst = i; }
    }
    return worst;
}

static void queue(TClient *c, const uint8_t *d, uint32_t n) {
    if (n > TC_TX_BYTES - c->queued) n = TC_TX_BYTES - c->queued;   // never trust the caller
    for (uint32_t i = 0; i < n; i++) {
        c->tx[c->head] = d[i];
        c->head = (uint16_t)((c->head + 1) % TC_TX_BYTES);
        c->queued++;
    }
}

void telnetBegin() {
    if (!s_srv) {
        s_srv = new WiFiServer(g_cfg.telnetPort);
        s_srv->begin();
        s_srv->setNoDelay(true);
        SYSLOG("[telnet] listening on port %u, up to %d clients",
               (unsigned)g_cfg.telnetPort, (int)MAX_TELNET_CLIENTS);
    }

    uint16_t ro = telnetReadOnlyPort();
    if (!s_srvRo && ro) {
        s_srvRo = new WiFiServer(ro);
        s_srvRo->begin();
        s_srvRo->setNoDelay(true);
        SYSLOG("[telnet] monitor port %u, read only", (unsigned)ro);
    }
}

bool telnetAccepting() {
    int w = fullest();
    return w < 0 || (TC_TX_BYTES - s_cl[w].queued) >= LOG_BATCH;
}

void telnetBroadcast(const uint8_t *d, uint32_t n) {
    if (!n) return;

    // Back-pressure: a peer that cannot keep up holds the UART reading. Wait a
    // little while - pumping the socket in the meantime drains the backlog -
    // then hang up on it rather than stalling the bridge forever.
    uint32_t start = millis();
    for (;;) {
        serviceClients();
        int w = fullest();
        if (w < 0 || (TC_TX_BYTES - s_cl[w].queued) >= n) break;
        if (millis() - start > TC_STALL_MS) {
            dropClient(&s_cl[w], "too slow, queue full");
            continue;
        }
        delay(1);
    }

    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++) {
        TClient *c = &s_cl[i];
        if (!c->used || !c->sock.connected()) continue;
        uint32_t room = TC_TX_BYTES - c->queued;
        uint32_t take = n <= room ? n : room;
        queue(c, d, take);
    }
}

// Take a connection from one of the two listeners. The monitor port skips the
// scratch block: it never filters, echoes or negotiates, so all it needs is a
// place to queue what the UART sends.
static void acceptFrom(WiFiServer *srv, bool readOnly) {
    if (!srv) return;

    WiFiClient sock = srv->accept();
    if (!sock) return;

    IPAddress ip = sock.remoteIP();
    TClient *c = firstFree();

    // Slots are not the only limit. Admitting a session that would leave the web
    // interface unable to answer means an operator who connected a fifth client
    // can no longer see how many are connected - so the page's floor is kept
    // back, and a client that does not fit is turned away instead.
    uint32_t need = readOnly ? (uint32_t)TC_TX_BYTES + TC_SLACK_BYTES
                             : (uint32_t)TC_SCRATCH_BYTES + TC_TX_BYTES + TC_SLACK_BYTES;
    uint32_t freeHeap = ESP.getFreeHeap();

    if (!c) {
        sock.print("!!! every session slot is in use\r\n");
        sock.stop();
        SYSLOG("[telnet] %s refused (%s), all %d slots busy",
               ip.toString().c_str(), readOnly ? "monitor" : "read/write",
               (int)MAX_TELNET_CLIENTS);
        return;
    }
    if (freeHeap < need + WEB_MIN_FREE_HEAP) {
        sock.print("!!! the bridge is out of memory\r\n");
        sock.stop();
        SYSLOG("[telnet] %s refused (%s), %u bytes of heap left, %u needed",
               ip.toString().c_str(), readOnly ? "monitor" : "read/write",
               (unsigned)freeHeap, (unsigned)(need + WEB_MIN_FREE_HEAP));
        return;
    }

    c->buf = readOnly ? nullptr : (uint8_t *)malloc(TC_SCRATCH_BYTES);
    c->tx  = (uint8_t *)malloc(TC_TX_BYTES);

    if (!c->tx || (!readOnly && !c->buf)) {
        releaseClient(c);
        sock.print("!!! the bridge is out of memory\r\n");
        sock.stop();
        SYSLOG("[telnet] %s refused, %u bytes of heap left",
               ip.toString().c_str(), (unsigned)ESP.getFreeHeap());
        return;
    }

    if (!readOnly) {
        c->rx    = c->buf;
        c->out   = c->buf + LOG_BATCH;
        c->reply = c->buf + LOG_BATCH * 3;
        c->filter.begin(g_cfg.crlfTx);
    }

    c->used     = true;
    c->readOnly = readOnly;
    c->sock     = sock;
    c->ip       = ip;
    // Stream::setTimeout stores a per-object member, so it has to be applied to
    // the copy the client slot keeps, not the local one.
    c->sock.setTimeout(2000);
    c->rxBytes = c->txBytes = c->ignored = 0;
    c->head = c->tail = c->queued = 0;
    sock.setNoDelay(true);

    // A monitor has no keyboard to configure, so there is nothing to negotiate
    // and no prompt to suppress.
    if (!readOnly) sock.write(GREETING, sizeof(GREETING));

    SYSLOG("[telnet] %s connected on port %u (%s, uart %s, %u bytes of heap left)",
           ip.toString().c_str(),
           (unsigned)(readOnly ? telnetReadOnlyPort() : g_cfg.telnetPort),
           readOnly ? "read only" : "read/write",
           uartSummary().c_str(), (unsigned)ESP.getFreeHeap());
}

void telnetLoop() {
    if (!s_srv && !s_srvRo) return;

    acceptFrom(s_srv, false);
    acceptFrom(s_srvRo, true);

    serviceClients();
}

// Settings changed in the web interface have to reach sessions that are already
// open, otherwise they keep the behaviour they were accepted with.
void telnetApplyConfig() {
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++)
        if (s_cl[i].used) s_cl[i].filter.setCrlfToCrlf(g_cfg.crlfTx);
}

void telnetCloseAll() {
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++) dropClient(&s_cl[i], "server restart");
}

// The listener does not survive a full WiFi shutdown, so it is thrown away and
// rebuilt whenever the interface is reconfigured.
void telnetReset() {
    telnetCloseAll();
    if (s_srv) {
        s_srv->close();
        delete s_srv;
        s_srv = nullptr;
    }
    if (s_srvRo) {
        s_srvRo->close();
        delete s_srvRo;
        s_srvRo = nullptr;
    }
}

uint8_t telnetClientCount(bool readOnly) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++)
        if (s_cl[i].used && s_cl[i].readOnly == readOnly) n++;
    return n;
}

void telnetAppendJson(String &out) {
    out += '[';
    bool first = true;
    for (uint8_t i = 0; i < MAX_TELNET_CLIENTS; i++) {
        TClient *c = &s_cl[i];
        if (!c->used) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"ip\":\"" + c->ip.toString() +
               "\",\"rx\":" + String(c->rxBytes) +
               ",\"tx\":" + String(c->txBytes) +
               ",\"queued\":" + String(c->queued) +
               ",\"ro\":" + String(c->readOnly ? 1 : 0) +
               ",\"ig\":" + String(c->ignored) +
               ",\"binary\":" + String(c->filter.binary() ? 1 : 0) + "}";
    }
    out += ']';
}
