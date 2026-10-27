#include "netmgr.h"
#include "config.h"
#include "log.h"
#include "telnet.h"
#include "uplink.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>

// Join retry cadence, and how long to keep knocking on the door once the setup
// access point has taken over.
#define JOIN_RETRY_MS   2000
#define JOIN_RETRY_LATE 30000

// Two gestures on the flash button, told apart by how long it is held:
//   ~1.5 s  bring the setup access point up
//   5   s   factory reset
// The factory reset fires as soon as the longer hold is reached rather than on
// release, so it works whether or not the button is let go, and the shorter
// gesture waits for the release so the two can never both happen.
#define BUTTON_SETUP_MS    1500
#define BUTTON_FACTORY_MS  5000

static NetMode   s_mode      = NET_AP_ONLY;
static uint32_t  s_attemptAt = 0;      // when the last join attempt went out
static uint32_t  s_retryAt   = 0;      // next late retry while the AP is up
static uint32_t  s_tries     = 0;      // attempts made since the last success
static bool      s_haveIp    = false;
static bool      s_dnsActive = false;
static bool      s_mdnsDone  = false;
static bool      s_btnReady     = false;   // operator asked for the setup point
static bool      s_btnFactory   = false;   // operator asked for a factory reset
static bool      s_factoryFired = false;   // the longer hold has been reported
static uint32_t  s_btnDown      = 0;

static DNSServer s_dns;
static String    s_hostname;

static void serviceScan();

NetMode netMode()      { return s_mode; }
bool    netConnected() { return s_mode != NET_AP_ONLY && WiFi.status() == WL_CONNECTED; }

String netModeName() {
    switch (s_mode) {
        case NET_STA:    return "client";
        case NET_STA_AP: return "client+ap";
        default:         return "ap";
    }
}

static void startDns() {
    if (s_dnsActive) return;
    if (s_dns.start(53, "*", WiFi.softAPIP())) {
        s_dnsActive = true;
        SYSLOG("[wifi] captive portal DNS active");
    }
}

// The subnet mask is a mask. Handing softAPConfig() an address here gets it
// stored as the netmask the DHCP server offers, clients take a lease with a
// nonsense prefix and drop it - which looks exactly like a DHCP server that is
// not answering.
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

static bool applyApConfig() {
    return WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
}

static void startAp() {
    WiFi.mode(s_mode == NET_STA ? WIFI_AP_STA : WIFI_AP);
    WiFi.setSleep(false);

    bool cfgOk = applyApConfig();
    bool ok = g_cfg.apPass[0]
            ? WiFi.softAP(g_cfg.apSsid, g_cfg.apPass, g_cfg.apChannel,
                          g_cfg.apHidden ? 1 : 0, MAX_AP_CLIENTS)
            : WiFi.softAP(g_cfg.apSsid, NULL, g_cfg.apChannel,
                          g_cfg.apHidden ? 1 : 0, MAX_AP_CLIENTS);

    if (!ok) {
        SYSLOG("[wifi] could not start access point \"%s\"", g_cfg.apSsid);
        return;
    }
    if (!cfgOk) SYSLOG("[wifi] access point address could not be set to %s",
                       AP_IP.toString().c_str());
    startDns();
    SYSLOG("[wifi] access point \"%s\" up on 192.168.4.1 (channel %u)",
           g_cfg.apSsid,
           (unsigned)(g_cfg.apChannel ? g_cfg.apChannel : WiFi.channel()));
}

static void announceMdns() {
    if (s_mdnsDone) return;
    if (MDNS.begin(s_hostname, WiFi.localIP())) {
        MDNS.addService("http", "tcp", WEB_PORT);
        s_mdnsDone = true;
    }
}

static void startStation() {
    s_hostname = g_cfg.hostname[0] ? String(g_cfg.hostname)
                                    : String("uart-server-") + String(ESP.getChipId(), HEX);
    WiFi.hostname(s_hostname);
    WiFi.setSleep(false);

    SYSLOG("[wifi] joining \"%s\" (giving up after %us)",
           g_cfg.staSsid, (unsigned)g_cfg.staTimeout);
    WiFi.begin(g_cfg.staSsid, g_cfg.staPass);
    s_attemptAt = millis();
    s_tries = 0;
}

static void onConnected() {
    s_haveIp = true;
    s_tries  = 0;
    SYSLOG("[wifi] connected to \"%s\", address %s",
           WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    telnetBegin();
    announceMdns();
}

// GPIO0 low *while resetting* is what selects the serial bootloader, and then
// this firmware never runs at all - the hardware reset button cannot be
// observed by the firmware at all, for the same reason. The flash button is
// therefore what both gestures use.
static void pollButton() {
    if (digitalRead(0) != LOW) {
        if (!s_btnDown) return;
        uint32_t held = millis() - s_btnDown;
        s_btnDown = 0;

        if (s_factoryFired) return;          // already on its way to a reset
        if (held >= BUTTON_SETUP_MS && !s_btnReady) {
            s_btnReady = true;
            if (s_mode != NET_STA) return;   // the setup point is already up
            SYSLOG("[wifi] setup access point requested from the flash button");
            s_mode = NET_STA_AP;
            WiFi.mode(WIFI_AP_STA);
            startAp();
            s_retryAt = millis() + JOIN_RETRY_LATE;
        }
        return;
    }

    if (!s_btnDown) { s_btnDown = millis(); return; }

    if (millis() - s_btnDown >= BUTTON_FACTORY_MS && !s_factoryFired) {
        s_factoryFired = true;
        s_btnFactory = true;
        SYSLOG("[sys] factory reset requested from the flash button");
    }
}

bool netFactoryResetDue() {
    bool due = s_btnFactory;
    s_btnFactory = false;
    return due;
}

void netBegin() {
    pinMode(0, INPUT_PULLUP);       // watched by pollButton() for a setup request

    if (cfgConfigured()) {
        s_mode = NET_STA;
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        startStation();
    } else {
        s_mode = NET_AP_ONLY;
        SYSLOG("[wifi] no configuration stored, starting access point mode");
        startAp();
    }

    // The telnet port is served on whatever interface exists: a joined network,
    // the fallback access point, or both.
    telnetBegin();
    netLoop();
}

void netApply() {
    s_haveIp = false;
    telnetReset();
    uplinkReset();

    if (s_dnsActive) { s_dns.stop(); s_dnsActive = false; }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);

    netBegin();
}

void netForget() {
    g_cfg.staSsid[0] = '\0';
    g_cfg.staPass[0] = '\0';
    cfgMarkDirty();
    cfgSave();
    netApply();
}

void netLoop() {
    pollButton();
    serviceScan();

    // The mDNS responder has no background task: nobody refreshes its records
    // unless the sketch does, and without this the hostname stops resolving
    // half a minute after the announcement.
    MDNS.update();

    switch (s_mode) {
        case NET_STA:
            if (WiFi.status() == WL_CONNECTED) {
                if (!s_haveIp) onConnected();
                return;
            }

            // netLoop runs thousands of times a second, so both the timer and
            // the retry counter only move once every JOIN_RETRY_MS - otherwise
            // the timeout below would be reached on the first pass.
            if (millis() - s_attemptAt < JOIN_RETRY_MS) return;

            s_attemptAt = millis();
            s_tries++;
            if (s_tries * (JOIN_RETRY_MS / 1000) > (uint32_t)g_cfg.staTimeout) {
                SYSLOG("[wifi] could not join \"%s\" within %us, "
                       "falling back to the access point",
                       g_cfg.staSsid, (unsigned)g_cfg.staTimeout);
                s_mode = NET_STA_AP;
                startAp();
                s_retryAt = millis() + JOIN_RETRY_LATE;
                return;
            }
            WiFi.begin(g_cfg.staSsid, g_cfg.staPass);
            return;

        case NET_STA_AP:
            // The access point is the way back in, but the station keeps
            // trying: a network that comes back should not need a reboot.
            if (WiFi.status() == WL_CONNECTED) {
                if (!s_haveIp) onConnected();
            } else if ((int32_t)(millis() - s_retryAt) >= 0) {
                s_retryAt = millis() + JOIN_RETRY_LATE;
                WiFi.begin(g_cfg.staSsid, g_cfg.staPass);
            }
            if (s_dnsActive) s_dns.processNextRequest();
            return;

        default:
            if (s_dnsActive) s_dns.processNextRequest();
            return;
    }
}

static String macOf(const uint8_t *b) {
    char out[18];
    if (!b) return "";
    snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return String(out);
}

// ---------------------------------------------------------------------------
// scanning
//
// A scan needs the station radio, and switching the radio into station mode
// restarts the access point - which drops the client that asked for it. Holding
// the request open across that would only turn a slow scan into a failed one,
// so the work is deferred: the answer goes back first, the radio flips a moment
// later, and the page polls.
//
// The result itself is built into a fixed buffer. It used to be a String on the
// heap, and that is where the crashes came from: the SDK keeps roughly eighty
// bytes per network resident until scanDelete(), the log ring has already taken
// most of the heap, and building the list on top of that ran the device out of
// memory in a dense RF environment. Nothing here allocates any more.
// ---------------------------------------------------------------------------

#define SCAN_START_DELAY  300      // let the reply leave before the AP restarts
#define SCAN_TIMEOUT_MS  20000
#define SCAN_JSON_MAX    2048
#define SCAN_MAX_REPORT  20

static char     s_scanBuf[SCAN_JSON_MAX];
static size_t   s_scanLen   = 0;
static bool     s_scanShort = false;   // more were found than fitted
static bool     s_scanPending = false;
static bool     s_scanActive  = false;
static uint32_t s_scanAt      = 0;
static int      s_scanStatus  = 0;     // count once finished, -2 on failure

static bool app(const char *str, size_t n) {
    if (s_scanLen + n >= SCAN_JSON_MAX) return false;
    memcpy(s_scanBuf + s_scanLen, str, n);
    s_scanLen += n;
    s_scanBuf[s_scanLen] = '\0';
    return true;
}

static bool app(const char *str)        { return app(str, strlen(str)); }
static bool app(const String &s)        { return app(s.c_str(), s.length()); }
static bool app(long v) {
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%ld", v);
    return n > 0 && app(tmp, (size_t)n);
}

// An SSID is an arbitrary byte string and may hold quotes, backslashes or
// control characters; all of them would otherwise break the reply.
static bool appEscaped(const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        if (c == '"' || c == '\\') {
            if (!app("\\", 1) || !app((const char *)&src[i], 1)) return false;
        } else if (c == '\n') {
            if (!app("\\n", 2)) return false;
        } else if (c == '\r') {
            if (!app("\\r", 2)) return false;
        } else if (c == '\t') {
            if (!app("\\t", 2)) return false;
        } else if (c < 0x20) {
            if (!app(".", 1)) return false;
        } else if (!app((const char *)&src[i], 1)) {
            return false;
        }
    }
    return true;
}

static void collectScanResults() {
    int n = WiFi.scanComplete();
    if (n < 0) n = 0;

    s_scanLen = 0;
    s_scanShort = false;
    s_scanBuf[0] = '\0';
    app("[");

    int reported = 0;
    for (int i = 0; i < n; i++) {
        if (reported >= SCAN_MAX_REPORT) { s_scanShort = true; break; }

        const bss_info *bi = WiFi.getScanInfoByIndex(i);
        if (!bi) continue;

        // Built in full or not at all: a half-written entry would leave the
        // reply as invalid JSON, which reads on the page as a dead scan.
        size_t mark = s_scanLen;
        bool ok = true;
        if (reported) ok = app(",");
        if (ok) ok = app("{\"ssid\":\"");
        if (ok) ok = appEscaped(bi->ssid, bi->ssid_len);
        if (ok) ok = app("\",\"rssi\":");
        if (ok) ok = app((long)bi->rssi);
        if (ok) ok = app(",\"ch\":");
        if (ok) ok = app((long)bi->channel);
        if (ok) ok = app(",\"enc\":");
        if (ok) ok = app((long)(bi->authmode != AUTH_OPEN));
        if (ok) ok = app(",\"mac\":\"");
        if (ok) ok = app(macOf(bi->bssid));
        if (ok) ok = app("\"}");

        if (!ok) {
            s_scanLen = mark;
            s_scanBuf[mark] = '\0';
            s_scanShort = true;
            break;
        }
        reported++;

        // Leave room for one more entry plus the closing bracket.
        if (s_scanLen + 160 > SCAN_JSON_MAX) { s_scanShort = true; break; }
    }
    app("]");

    WiFi.scanDelete();          // results are only valid until this call
    s_scanStatus = n;
    s_scanActive = false;
    SYSLOG("[wifi] scan finished, %d network(s), %d listed%s",
           n, reported, s_scanShort ? " (truncated)" : "");
}

static void serviceScan() {
    if (s_scanPending) {
        if ((int32_t)(millis() - s_scanAt) < 0) return;
        s_scanPending = false;

        // Radio into station mode as well. This is what restarts the access
        // point, so it happens here and not inside the request handler.
        WiFi.mode(WIFI_AP_STA);
        applyApConfig();
        if (g_cfg.apPass[0])
            WiFi.softAP(g_cfg.apSsid, g_cfg.apPass, 0, g_cfg.apHidden ? 1 : 0, MAX_AP_CLIENTS);
        else
            WiFi.softAP(g_cfg.apSsid, NULL, 0, g_cfg.apHidden ? 1 : 0, MAX_AP_CLIENTS);
        startDns();

        // async = true: a blocking scan would hold the whole bridge still for
        // seconds, including every telnet session.
        int rc = WiFi.scanNetworks(true, true);
        if (rc == WIFI_SCAN_FAILED) {
            SYSLOG("[wifi] scan could not be started");
            s_scanLen   = 0;
            app("[]");
            s_scanStatus = -2;
            return;
        }
        s_scanActive = true;
        s_scanAt     = millis();
        SYSLOG("[wifi] scan running");
        return;
    }

    if (!s_scanActive) return;

    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        collectScanResults();
        return;
    }
    if (millis() - s_scanAt > SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        s_scanActive = false;
        s_scanStatus = -2;
        s_scanLen    = 0;
        app("[]");
        SYSLOG("[wifi] scan timed out");
    }
}

bool netScanStart() {
    if (s_scanPending || s_scanActive) return false;
    s_scanPending = true;
    s_scanAt      = millis() + SCAN_START_DELAY;
    return true;
}

int netScanStatus() {
    if (s_scanPending || s_scanActive) return -1;
    return s_scanStatus;
}

const char *netScanJson(size_t &len, bool &truncated) {
    len = s_scanLen;
    truncated = s_scanShort;
    return s_scanBuf;
}
