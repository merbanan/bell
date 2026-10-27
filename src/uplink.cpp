#include "uplink.h"
#include "config.h"
#include "log.h"
#include "uart.h"
#include "netmgr.h"

#include <ESP8266WiFi.h>

#include "telnet_filter.h"

// Retry cadence for reaching the host: quick at first, slower once it is clear
// the host is not there, capped so a long outage does not turn into a flood.
#define UL_RETRY_MIN 2000
#define UL_RETRY_MAX 30000
#define UL_FIRST_DELAY 1500
#define UL_DNS_TIMEOUT_MS 2000
#define UL_STALL_MS 30000              // a host that cannot take bytes is closed
#define UL_DROP_LOG_MS 5000            // rate limit for "ring full" complaints

// One heap block: rx | out | reply. rx is what comes off the host, out has room
// for CRLF expansion, reply carries the negotiation answers.
#define UL_RX_BYTES    LOG_BATCH
#define UL_OUT_BYTES   (LOG_BATCH * 2)
#define UL_REPLY_BYTES LOG_BATCH
#define UL_SCRATCH_BYTES (UL_RX_BYTES + UL_OUT_BYTES + UL_REPLY_BYTES + 8)

// UART-to-host transmit ring. Allocated together with the block, so an idle
// uplink pays nothing for it.
#define UL_TX_BYTES 1024u
#define UL_SLACK_BYTES 256u

static WiFiClient   s_sock;
static TelnetFilter s_filter;

static uint8_t *s_buf   = nullptr;      // rx | out | reply
static uint8_t *s_rx    = nullptr;
static uint8_t *s_out   = nullptr;
static uint8_t *s_reply = nullptr;
static uint8_t *s_tx    = nullptr;      // UL_TX_BYTES

static uint16_t s_head  = 0;
static uint16_t s_tail  = 0;
static uint16_t s_queued = 0;

static bool     s_open    = false;      // TCP session established
static uint32_t s_nextTry = 0;          // earliest next attempt
static uint32_t s_backoff = UL_RETRY_MIN;
static uint32_t s_fails   = 0;          // attempts since the last success

static uint32_t s_stallAt = 0;          // last time anything moved on the wire
static uint32_t s_dropped = 0;          // UART bytes thrown away while offline
static uint32_t s_dropLoggedAt = 0;
static uint32_t s_rxTotal = 0;
static uint32_t s_txTotal = 0;

// Identity the module announces the moment the host answers: "AA:BB:CC:DD:EE:FF"
// as an option-200 subnegotiation. Built once per session and written out in
// pieces, so a socket that cannot take it whole does not lose any of it.
static uint8_t s_greet[24];
static uint8_t s_greetLen = 0;
static uint8_t s_greetPos = 0;
static char    s_mac[18];

static void buildGreeting() {
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    WiFi.macAddress(mac);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t i = 0;
    s_greet[i++] = telnet::IAC;
    s_greet[i++] = telnet::SB;
    s_greet[i++] = telnet::OPT_MAC;
    for (uint8_t j = 0; j < sizeof(s_mac) - 1; j++) s_greet[i++] = (uint8_t)s_mac[j];
    s_greet[i++] = telnet::IAC;
    s_greet[i++] = telnet::SE;
    s_greetLen = i;
    s_greetPos = 0;
}

static void closeSession(const char *why) {
    s_sock.stop();
    s_open = false;
    s_nextTry = millis() + s_backoff;
    s_backoff = s_backoff < UL_RETRY_MAX ? (uint32_t)(s_backoff * 2) : UL_RETRY_MAX;
    s_fails++;
    SYSLOG("[uplink] %s, retrying in %u s", why, (unsigned)(s_backoff / 1000));
}

static bool configure() {
    if (s_buf) return true;

    uint32_t need = UL_SCRATCH_BYTES + UL_TX_BYTES + UL_SLACK_BYTES;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < need + WEB_MIN_FREE_HEAP) {
        s_nextTry = millis() + s_backoff;
        s_backoff = s_backoff < UL_RETRY_MAX ? (uint32_t)(s_backoff * 2) : UL_RETRY_MAX;
        SYSLOG("[uplink] %u bytes of heap left, %u needed - holding off",
               (unsigned)freeHeap, (unsigned)need);
        return false;
    }

    s_buf = (uint8_t *)malloc(UL_SCRATCH_BYTES);
    s_tx  = (uint8_t *)malloc(UL_TX_BYTES);
    if (!s_buf || !s_tx) {
        free(s_buf); free(s_tx);
        s_buf = s_tx = nullptr;
        s_nextTry = millis() + s_backoff;
        s_backoff = s_backoff < UL_RETRY_MAX ? (uint32_t)(s_backoff * 2) : UL_RETRY_MAX;
        SYSLOG("[uplink] allocation failed, %u bytes of heap left",
               (unsigned)ESP.getFreeHeap());
        return false;
    }
    s_rx    = s_buf;
    s_out   = s_buf + UL_RX_BYTES;
    s_reply = s_buf + UL_RX_BYTES + UL_OUT_BYTES;
    SYSLOG("[uplink] buffers allocated");
    return true;
}

static void greetRemainder() {
    while (s_greetPos < s_greetLen && s_sock.availableForWrite() > 0) {
        size_t w = s_sock.write(s_greet + s_greetPos, s_greetLen - s_greetPos);
        if (!w) break;
        s_greetPos += (uint8_t)w;
        s_stallAt = millis();
    }
}

static void pumpTx() {
    uint8_t out[64];

    while (s_queued && s_sock.availableForWrite() > 0) {
        uint16_t room = (uint16_t)(UL_TX_BYTES - s_tail);
        uint16_t n    = s_queued < room ? s_queued : room;

        size_t o = s_filter.encode(out, sizeof(out), s_tx + s_tail, n);
        if (!o) break;

        size_t w = s_sock.write(out, o);
        if (!w) break;

        // A short write can stop part way through an escaped pair, so count how
        // many queued bytes the bytes actually written account for; an IAC that
        // lost its second half is left in the ring for the next pass.
        uint16_t used = 0, acc = 0;
        while (used < n) {
            uint16_t at   = (uint16_t)((s_tail + used) % UL_TX_BYTES);
            uint16_t need = (s_tx[at] == telnet::IAC) ? 2 : 1;
            if (acc + need > w) break;
            acc += need;
            used++;
        }

        s_tail   = (uint16_t)((s_tail + used) % UL_TX_BYTES);
        s_queued = (uint16_t)(s_queued - used);
        s_txTotal += used;
        s_stallAt = millis();
    }
}

static void pumpRx() {
    while (s_sock.available() > 0) {
        size_t n = s_sock.read(s_rx, UL_RX_BYTES);
        if (!n) break;

        size_t rlen = 0;
        size_t olen = s_filter.feed(s_rx, n, s_out, UL_OUT_BYTES,
                                    s_reply, UL_REPLY_BYTES, &rlen);
        if (rlen) s_sock.write(s_reply, rlen);
        if (!olen) continue;

        // Whatever the host types reaches the UART the same way an operator on
        // the module's own telnet port would; the log records it as OUT if the
        // module is configured to log transmissions.
        uartWrite(s_out, (uint32_t)olen);
        s_rxTotal += (uint32_t)olen;
        s_stallAt = millis();
    }
}

void uplinkBegin() {
    buildGreeting();
    s_nextTry = millis() + UL_FIRST_DELAY;
    SYSLOG("[uplink] host %s:%u configured, id %s",
           g_cfg.hostHost, (unsigned)g_cfg.hostPort, s_mac);
}

void uplinkLoop() {
    if (!g_cfg.hostHost[0] || !netConnected()) {
        if (s_open) {
            s_sock.stop();
            s_open = false;
            s_nextTry = millis() + UL_RETRY_MIN;
            SYSLOG("[uplink] closed, the network is down");
        }
        return;
    }

    if (!s_open) {
        if (!configure()) return;
        if ((int32_t)(millis() - s_nextTry) < 0) return;

        IPAddress ip;
        if (!WiFi.hostByName(g_cfg.hostHost, ip, UL_DNS_TIMEOUT_MS)) {
            closeSession("cannot resolve the host");
            return;
        }
        if (!s_sock.connect(ip, g_cfg.hostPort)) {
            closeSession("cannot reach the host");
            return;
        }

        s_open    = true;
        s_greetPos = 0;
        s_backoff = UL_RETRY_MIN;
        s_nextTry = 0;
        s_stallAt = millis();
        s_sock.setNoDelay(true);
        SYSLOG("[uplink] connected to %s:%u, identifying as %s",
               ip.toString().c_str(), (unsigned)g_cfg.hostPort, s_mac);
    }

    greetRemainder();
    pumpTx();
    pumpRx();

    if (s_queued && (uint32_t)(millis() - s_stallAt) > UL_STALL_MS) {
        closeSession("the host stopped taking bytes");
        return;
    }
}

void uplinkReset() {
    if (!s_open) return;
    s_sock.stop();
    s_open = false;
    s_nextTry = millis() + UL_RETRY_MIN;
    SYSLOG("[uplink] closed, connection dropped by request");
}

bool uplinkSend(const uint8_t *d, uint32_t n) {
    if (!s_tx || !n) return true;         // nothing configured: nothing to do

    uint32_t take = n > UL_TX_BYTES ? UL_TX_BYTES : n;
    uint32_t room = (uint32_t)(UL_TX_BYTES - s_queued);
    if (take > room) {
        // The host is not keeping up. Rather than stall the UART reader, the
        // oldest bytes go and the newest are kept - a live stream matters more
        // than a complete one.
        uint32_t drop = take - room;
        s_tail    = (uint16_t)((s_tail + drop) % UL_TX_BYTES);
        s_queued -= (uint16_t)drop;
        s_dropped += drop;
        if (millis() - s_dropLoggedAt > UL_DROP_LOG_MS) {
            s_dropLoggedAt = millis();
            SYSLOG("[uplink] dropped %u byte(s) of UART output, %u total",
                   (unsigned)drop, (unsigned)s_dropped);
        }
    }

    for (uint32_t i = 0; i < take; i++) {
        s_tx[s_head] = d[i];
        s_head = (uint16_t)((s_head + 1) % UL_TX_BYTES);
        s_queued++;
    }
    return true;
}

bool uplinkAccepting() { return true; }

void uplinkAppendJson(String &out) {
    bool cfgd = g_cfg.hostHost[0] != '\0';
    out += "{\"configured\":" + String(cfgd ? 1 : 0) +
           ",\"host\":\"" + jsonEscape(String(g_cfg.hostHost)) + "\"" +
           ",\"port\":" + String(g_cfg.hostPort) +
           ",\"open\":" + String(s_open ? 1 : 0) +
           ",\"queued\":" + String(s_queued) +
           ",\"dropped\":" + String(s_dropped) +
           ",\"rx\":" + String(s_rxTotal) +
           ",\"tx\":" + String(s_txTotal) +
           ",\"retryIn\":" + String(s_open || !cfgd ? 0 :
                                    (int32_t)(s_nextTry - millis()) > 0
                                        ? (uint32_t)(s_nextTry - millis()) : 0) +
           ",\"mac\":\"" + String(s_mac) + "\"}";
}
