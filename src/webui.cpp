#include "webui.h"
#include "bell.h"
#include "config.h"
#include "log.h"
#include "netmgr.h"
#include "ota.h"
#include "telnet.h"
#include "uart.h"
#include "uplink.h"
#include "gpio.h"

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <Updater.h>
#include <LittleFS.h>
#include <base64.h>
#include <ArduinoJson.h>

#include "web_assets.h"

static ESP8266WebServer s_server(WEB_PORT);

// A log page is base64'd into the reply, so one costs about 2.3x its own size
// in heap all at once. 2 kB is what a module running an access point can
// spare; the page catches up over several polls rather than asking for a
// buffer that is not there.
#define LOG_PAGE_MAX   2048
#define SEND_MAX       512
#define LOG_DUMP_CHUNK 512

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static String argStr(const char *key, const String &fallback = "") {
    return s_server.hasArg(key) ? s_server.arg(key) : fallback;
}

// Log positions are 64 bit; the core's String has no 64 bit parser.
static uint64_t argU64(const char *key, uint64_t fallback) {
    if (!s_server.hasArg(key)) return fallback;
    return strtoull(s_server.arg(key).c_str(), nullptr, 10);
}

// A form control that is cleared is still sent by URLSearchParams as "key=",
// and the server happily registers that as an argument - so the value has to
// be checked, not just the presence of the name.
static bool argOn(const char *key) {
    if (!s_server.hasArg(key)) return false;
    String v = s_server.arg(key);
    return v == "1" || v == "on" || v == "true";
}

static long argLong(const char *key, long fallback, long lo, long hi) {
    if (!s_server.hasArg(key)) return fallback;
    long v = s_server.arg(key).toInt();
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static void okJson(const String &extra = "") {
    String s = "{\"ok\":true" + extra + "}";
    s_server.send(200, "application/json", s);
}

static void failJson(const String &msg) {
    String s = "{\"ok\":false,\"error\":\"" + msg + "\"}";
    s_server.send(500, "application/json", s);
}

static void noStore() {
    s_server.sendHeader("Cache-Control", "no-store, max-age=0");
}

static void sendAsset(int code, const char *type, PGM_P body) {
    noStore();
    s_server.send_P(code, type, body);
}

// ---------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------

static void handleIndex() {
    sendAsset(200, "text/html", WEB_INDEX_HTML);
}

static void handleCss() {
    sendAsset(200, "text/css", WEB_APP_CSS);
}

static void handleJs() {
    sendAsset(200, "application/javascript", WEB_APP_JS);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

// Built as a plain string rather than through a JSON document: the reply lives
// for the length of one request, and every byte kept out of static RAM ends up
// in the log ring instead.
static void handleState() {
    bool joined = WiFi.status() == WL_CONNECTED;

    // Building and sending this reply takes about twice its own length in heap,
    // and a session takes 2.6 kB of what is left. Once the heap is down there,
    // the allocation does not fail politely: it resets the module. Answer with
    // something small instead, and the page will ask again in a second.
    if (ESP.getFreeHeap() < WEB_MIN_FREE_HEAP) {
        s_server.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"busy, try again\"}");
        return;
    }

    // One allocation for the whole reply, and send() copies it again, so the
    // buffer has to be right first time: with sessions connected the heap is
    // down to a couple of kilobytes and a String that doubles its way there
    // truncates the reply. 1500 covers every case below.
    String out;
    out.reserve(1500);
    out += "{\"fw\":\"" FW_VERSION "\",\"board\":\"" FW_BOARD "\"";
    out += ",\"chipId\":\"" + String(ESP.getChipId(), HEX) + "\"";
    out += ",\"mac\":\"" + jsonEscape(WiFi.macAddress()) + "\"";
    out += ",\"flash\":\"" + String((ESP.getFlashChipRealSize() >> 20)) + "MB\"";
    out += ",\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"";
    out += ",\"uptime\":\"" + logUptime(millis()) + "\"";
    out += ",\"heap\":" + String((uint32_t)ESP.getFreeHeap());
    out += ",\"mode\":\"" + netModeName() + "\"";
    out += ",\"ap\":\"" + (netMode() != NET_STA ? WiFi.softAPIP().toString() : "") + "\"";
    out += ",\"sta\":\"" + (joined ? WiFi.localIP().toString() : "") + "\"";
    out += ",\"ssid\":\"" + (joined ? jsonEscape(WiFi.SSID()) : String()) + "\"";
    out += ",\"rssi\":" + String(WiFi.RSSI());
    out += ",\"hostname\":\"" + jsonEscape(String(g_cfg.hostname)) + "\"";
    out += ",\"telnetPort\":" + String((uint32_t)g_cfg.telnetPort);
    out += ",\"telnetPortRo\":" + String((uint32_t)telnetReadOnlyPort());
    out += ",\"clients\":" + String(telnetClientCount(false));
    out += ",\"clientsRo\":" + String(telnetClientCount(true));
    out += ",\"maxClients\":" + String((uint32_t)MAX_TELNET_CLIENTS);

    out += ",\"net\":{\"staSsid\":\"" + jsonEscape(String(g_cfg.staSsid)) + "\"";
    out += ",\"telnetPort\":" + String((uint32_t)g_cfg.telnetPort);
    out += ",\"telnetPortRo\":" + String((uint32_t)g_cfg.telnetPortRo);
    out += ",\"staPassSet\":" + String(g_cfg.staPass[0] ? "true" : "false");
    out += ",\"hostname\":\"" + jsonEscape(String(g_cfg.hostname)) + "\"";
    out += ",\"staTimeout\":" + String((uint32_t)g_cfg.staTimeout);
    out += ",\"apSsid\":\"" + jsonEscape(String(g_cfg.apSsid)) + "\"";
    // Never the passphrase itself: this reply is unauthenticated and the page
    // polls it every few seconds.
    out += ",\"apPassSet\":" + String(g_cfg.apPass[0] ? "true" : "false");
    out += ",\"apChannel\":" + String((uint32_t)g_cfg.apChannel);
    out += ",\"apHidden\":" + String(g_cfg.apHidden ? "true" : "false");
    out += ",\"hostHost\":\"" + jsonEscape(String(g_cfg.hostHost)) + "\"";
    out += ",\"hostPort\":" + String((uint32_t)g_cfg.hostPort);
    out += ",\"otaUrl\":\"" + jsonEscape(String(g_cfg.otaUrl)) + "\"";
    out += ",\"otaAuto\":" + String(g_cfg.otaAuto ? "true" : "false") + "}";

    out += ",\"uart\":{\"baud\":" + String(g_cfg.baud);
    out += ",\"bits\":" + String((uint32_t)g_cfg.bits);
    out += ",\"parity\":\"" + String(g_cfg.parity) + "\"";
    out += ",\"stops\":" + String((uint32_t)g_cfg.stops);
    out += ",\"swapPins\":" + String(g_cfg.swapPins ? "true" : "false");
    out += ",\"localEcho\":" + String(g_cfg.localEcho ? "true" : "false");
    out += ",\"crlfTx\":" + String(g_cfg.crlfTx ? "true" : "false");
    out += ",\"logTx\":" + String(g_cfg.logTx ? "true" : "false");
    out += ",\"clearOnBoot\":" + String(g_cfg.clearOnBoot ? "true" : "false");
    out += ",\"statusLed\":" + String(g_cfg.statusLed ? "true" : "false");
    out += ",\"ledSeconds\":" + String((uint32_t)g_cfg.ledSeconds);
    out += ",\"rx\":" + String((uint64_t)uartRxBytes());
    out += ",\"tx\":" + String((uint64_t)uartTxBytes());
    out += ",\"errors\":" + String((uint32_t)uartRxErrors()) + "}";

    out += ",\"log\":{\"capacity\":" + String((uint32_t)logCapacity());
    out += ",\"written\":" + String((uint64_t)logWritten());
    out += ",\"oldest\":" + String((uint64_t)logOldest());
    out += ",\"highWater\":" + String((uint32_t)logHighWater()) + "}";

    // Every pin, present whether or not it can be driven, so a client sees the
    // whole picture without a second request.
    out += ",\"gpioAvailable\":[";
    for (uint8_t pin = 0; pin <= 16; pin++) {
        if (!gpioUsable(pin)) continue;
        if (out[out.length() - 1] != '[') out += ',';
        out += String(pin);
    }
    out += "]";
    out += ",\"gpios\":";
    gpioAppendState(out);

    out += ",\"sessions\":";
    telnetAppendJson(out);
    out += ",\"uplink\":";
    uplinkAppendJson(out);
    out += ",\"ota\":" + otaJson() + "}";

    noStore();
    s_server.send(200, "application/json", out);
}

static void handleScanStart() {
    bool started = netScanStart();
    noStore();
    String out = "{\"ok\":" + String(started ? "true" : "false");
    if (!started) out += ",\"error\":\"a scan is already running\"";
    out += ",\"scanning\":" + String(started ? "true" : "false") + "}";
    s_server.send(started ? 200 : 409, "application/json", out);
}

// Streamed so the device never has to hold a second copy of the list: the scan
// buffer is handed out as-is, in chunks, straight from flash-resident RAM.
static void handleScan() {
    int n = netScanStatus();
    bool running = (n < 0);

    size_t len = 0;
    bool truncated = false;
    const char *json = netScanJson(len, truncated);

    char head[128];
    int hl = snprintf(head, sizeof(head),
                      "{\"scanning\":%s,\"count\":%d,\"truncated\":%s%s,\"networks\":",
                      running ? "true" : "false", running ? 0 : n,
                      truncated ? "true" : "false",
                      n == -2 ? ",\"error\":\"the scan did not finish\"" : "");
    if (hl < 0) hl = 0;
    if (hl > (int)sizeof(head) - 1) hl = (int)sizeof(head) - 1;

    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "application/json", "");
    s_server.sendContent(head, (size_t)hl);
    if (!running && len) s_server.sendContent(json, len);
    s_server.sendContent("}");
    s_server.sendContent("");
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

// A field that was not sent at all means "leave this alone"; only a field that
// is present and empty clears it. Without that distinction, a partial post -
// say just a new telnet port - silently wipes every password it does not
// mention. "*" means the same as leaving it out, which is what the page sends
// for an untouched box.
static bool untouched(const char *key) {
    return !s_server.hasArg(key) || s_server.arg(key) == "*";
}

static void handleSaveNet() {
    // Collect and check the whole form first. Committing half of it and then
    // answering with an error would leave the running configuration describing
    // a mix of the old and new settings.
    String ssid = argStr("staSsid", g_cfg.staSsid);
    ssid.trim();

    String pass = argStr("staPass");
    bool keepPass = untouched("staPass");
    if (!keepPass && pass.length() > 0 && pass.length() < 8) {
        failJson("WPA2 needs at least 8 characters");
        return;
    }
    if (pass.length() >= MAX_STA_PASS) { failJson("password too long"); return; }
    if (ssid.length() >= MAX_SSID) { failJson("SSID too long"); return; }

    String host = argStr("hostname", g_cfg.hostname);
    host.trim();
    if (host.length() >= MAX_HOSTNAME) { failJson("device name too long"); return; }

    String apSsid = argStr("apSsid", g_cfg.apSsid);
    apSsid.trim();
    if (apSsid.length() >= MAX_SSID) { failJson("AP name too long"); return; }

    // Same convention as the station password.
    String apPass = argStr("apPass");
    bool keepApPass = untouched("apPass");
    if (!keepApPass && apPass.length() > 0 && apPass.length() < 8) {
        failJson("AP password needs 8+ characters");
        return;
    }
    if (apPass.length() >= MAX_AP_PASS) { failJson("AP password too long"); return; }

    String url = argStr("otaUrl", g_cfg.otaUrl);
    url.trim();
    if (url.length() >= MAX_URL) { failJson("update URL too long"); return; }

    long port = argLong("telnetPort", g_cfg.telnetPort, 1, 65535);
    if (port == WEB_PORT) { failJson("the web interface already uses port 80"); return; }

    // 0 means "one above the read/write port"; anything else is taken literally.
    long portRo = argLong("telnetPortRo", g_cfg.telnetPortRo, 0, 65535);
    if (portRo != 0) {
        if (portRo == WEB_PORT) { failJson("the web interface already uses port 80"); return; }
        if (portRo == port) { failJson("the read-only port must differ from the read/write one"); return; }
    }

    // An empty field clears the host: without it the uplink stays off even while
    // the station itself is joined.
    String hostAddr = argStr("hostHost", g_cfg.hostHost);
    hostAddr.trim();
    if (hostAddr.length() >= MAX_HOST_ADDR) { failJson("host address too long"); return; }
    long hostPort = argLong("hostPort", g_cfg.hostPort, 1, 65535);
    if (hostPort == WEB_PORT) { failJson("the web interface already uses port 80"); return; }

    uint8_t  timeout = (uint8_t)argLong("staTimeout", g_cfg.staTimeout, 3, 120);
    uint8_t  channel = (uint8_t)argLong("apChannel", g_cfg.apChannel, 0, 13);
    bool     hidden  = s_server.hasArg("apHidden")  ? argOn("apHidden")  : g_cfg.apHidden;
    bool     autoOta = s_server.hasArg("otaAuto")   ? argOn("otaAuto")   : g_cfg.otaAuto;

    uint16_t oldPort = g_cfg.telnetPort, oldPortRo = g_cfg.telnetPortRo;
    String oldHost   = String(g_cfg.hostHost);
    uint16_t oldHostPort = g_cfg.hostPort;

    strlcpy(g_cfg.staSsid, ssid.c_str(), sizeof(g_cfg.staSsid));
    if (!keepPass) strlcpy(g_cfg.staPass, pass.c_str(), sizeof(g_cfg.staPass));
    strlcpy(g_cfg.hostname, host.c_str(), sizeof(g_cfg.hostname));
    g_cfg.staTimeout = timeout;
    strlcpy(g_cfg.apSsid, apSsid.c_str(), sizeof(g_cfg.apSsid));
    if (!keepApPass) strlcpy(g_cfg.apPass, apPass.c_str(), sizeof(g_cfg.apPass));
    g_cfg.apChannel = channel;
    g_cfg.apHidden  = hidden;
    strlcpy(g_cfg.otaUrl, url.c_str(), sizeof(g_cfg.otaUrl));
    g_cfg.otaAuto   = autoOta;
    g_cfg.telnetPort = (uint16_t)port;
    g_cfg.telnetPortRo = (uint16_t)portRo;
    strlcpy(g_cfg.hostHost, hostAddr.c_str(), sizeof(g_cfg.hostHost));
    g_cfg.hostPort = (uint16_t)hostPort;

    if (!cfgSave()) { failJson("could not write config.json"); return; }

    if (g_cfg.telnetPort != oldPort || g_cfg.telnetPortRo != oldPortRo) {
        // Move the listener straight away rather than making the operator find
        // a reboot button: the sessions on the old port cannot survive it.
        uint16_t wasRo = telnetReadOnlyPort();
        telnetReset();
        telnetBegin();
        SYSLOG("[cfg] telnet ports moved to %u (read/write) and %u (read only)",
               (unsigned)g_cfg.telnetPort, (unsigned)wasRo);
    }

    if (String(g_cfg.hostHost) != oldHost || g_cfg.hostPort != oldHostPort) {
        // The live connection belongs to the old host; drop it so the next
        // attempt goes to the one that is configured now.
        uplinkReset();
        SYSLOG("[cfg] host is now %s:%u%s",
               g_cfg.hostHost, (unsigned)g_cfg.hostPort,
               g_cfg.hostHost[0] ? "" : " (uplink off)");
    }

    SYSLOG("[cfg] saved: join \"%s\", ap \"%s\"", g_cfg.staSsid, g_cfg.apSsid);
    okJson(",\"reboot\":true");
}

static void handleForgetNet() {
    netForget();
    okJson(",\"reboot\":true");
}

static void handleSaveUart() {
    // Validate everything before touching the running configuration, so a
    // rejected form does not leave the module half way between two settings.
    long baud = argLong("baud", (long)g_cfg.baud, 300, 921600);
    uint8_t bits  = (uint8_t)argLong("bits", g_cfg.bits, 5, 8);
    uint8_t stops = (uint8_t)(argLong("stops", g_cfg.stops, 1, 2) == 2 ? 2 : 1);

    String par = argStr("parity", String(g_cfg.parity));
    par.trim();
    if (par.length() != 1 || !strchr("NEO", par[0])) {
        failJson("parity must be N, E or O");
        return;
    }

    g_cfg.baud   = (uint32_t)baud;
    g_cfg.bits   = bits;
    g_cfg.stops  = stops;
    g_cfg.parity = par[0];

    // argOn() reports "false" for an absent key, so a partial post would clear
    // every box the caller did not mention. Keep what is already configured.
    g_cfg.swapPins    = s_server.hasArg("swapPins")    ? argOn("swapPins")    : g_cfg.swapPins;
    g_cfg.localEcho   = s_server.hasArg("localEcho")   ? argOn("localEcho")   : g_cfg.localEcho;
    g_cfg.crlfTx      = s_server.hasArg("crlfTx")      ? argOn("crlfTx")      : g_cfg.crlfTx;
    g_cfg.logTx       = s_server.hasArg("logTx")       ? argOn("logTx")       : g_cfg.logTx;
    g_cfg.clearOnBoot = s_server.hasArg("clearOnBoot") ? argOn("clearOnBoot") : g_cfg.clearOnBoot;
    g_cfg.statusLed   = s_server.hasArg("statusLed")   ? argOn("statusLed")   : g_cfg.statusLed;
    g_cfg.ledSeconds  = (uint16_t)argLong("ledSeconds", g_cfg.ledSeconds, 0, 3600);

    if (!cfgSave()) { failJson("could not write config.json"); return; }
    uartReconfigure();
    telnetApplyConfig();
    okJson();
}

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Appended straight into the reply rather than returned. The core's encoder
// hands back a String, which meant a second full-size buffer alive alongside
// the reply: when that allocation failed the page was served with "data":""
// and an n that said otherwise, which looks like a working answer and is not.
static bool b64Append(String &out, const uint8_t *d, uint32_t n) {
    if (!out.reserve(out.length() + (n + 2) / 3 * 4 + 4)) return false;

    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t left = n - i;
        uint32_t v    = (uint32_t)d[i] << 16;
        if (left > 1) v |= (uint32_t)d[i + 1] << 8;
        if (left > 2) v |= d[i + 2];

        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += left > 1 ? B64[(v >> 6) & 63] : '=';
        out += left > 2 ? B64[v & 63] : '=';
    }
    return true;
}

static void logBusy() {
    s_server.send(503, "application/json",
                  "{\"ok\":false,\"error\":\"busy, try again\"}");
}

static void handleLog() {
    // This is the expensive endpoint: the page buffer and its base64 are both
    // alive at once. Ask for a page that fits rather than one that might not -
    // a session being open is precisely when the log is being watched.
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < LOG_MIN_FREE_HEAP) { logBusy(); return; }

    uint64_t oldest = logVisibleFrom();
    uint64_t from   = argU64("from", oldest);

    // Page bytes plus 4/3 for base64, less a little for the rest of the reply.
    uint32_t max = (freeHeap - 512) * 3 / 7;
    if (max > LOG_PAGE_MAX) max = LOG_PAGE_MAX;
    if (max < 512)           max = 512;

    if (s_server.hasArg("max")) {
        long v = s_server.arg("max").toInt();
        if (v > 0 && (uint32_t)v < max) max = (uint32_t)v;
    }

    uint8_t *tmp = (uint8_t *)malloc(max);
    if (!tmp) { logBusy(); return; }

    // Anything the ring already recycled, or that a clear hid, is gone for
    // good: tell the browser rather than splicing two unrelated streams.
    bool gap = from < oldest;
    if (gap) from = oldest;

    uint32_t n = logCopy(from, tmp, max);

    // One buffer for the whole reply, reserved for the worst case up front so
    // nothing has to grow - and so reallocate - halfway through.
    String out;
    if (!out.reserve(160 + (max + 2) / 3 * 4)) {
        free(tmp);
        logBusy();
        return;
    }
    out += "{\"head\":" + String((uint64_t)logWritten());
    out += ",\"from\":" + String((uint64_t)from);
    out += ",\"oldest\":" + String((uint64_t)oldest);
    out += ",\"cap\":" + String(logCapacity());
    out += ",\"n\":" + String(n);
    out += ",\"gap\":" + String(gap ? "true" : "false");
    // Direction travels beside the bytes, never inside them: the downloadable
    // log is exactly what crossed the line.
    // logDirRuns writes its own brackets.
    out += ",\"dirs\":";
    logDirRuns(from, from + n, out);

    out += ",\"data\":\"";
    if (!b64Append(out, tmp, n)) {
        free(tmp);
        logBusy();
        return;
    }
    free(tmp);
    out += "\"}";

    noStore();
    s_server.send(200, "application/json", out);
}

static void handleLogClear() {
    logClear();
    SYSLOG("[log] cleared from the web interface");
    okJson();
}

// Streamed dump: walk the ring once, from the oldest retained byte up to the
// position the request started at, so an active UART cannot keep the transfer
// running forever.
static void handleLogTxt() {
    uint64_t end = logWritten();

    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain; charset=utf-8", "");

    uint8_t buf[LOG_DUMP_CHUNK];
    char    line[LOG_DUMP_CHUNK + 8];
    uint64_t pos = logVisibleFrom();
    uint32_t off = 0;

    while (pos < end) {
        uint32_t n = logCopy(pos, buf, LOG_DUMP_CHUNK);
        if (!n) break;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n')      line[off++] = '\n';
            else if (c == '\r') line[off++] = '\r';
            else if (c == '\t') line[off++] = '\t';
            else if (c >= 32 && c < 127) line[off++] = (char)c;
            else line[off++] = '.';
            if (off == LOG_DUMP_CHUNK) { s_server.sendContent(line, off); off = 0; }
        }
        pos += n;
    }
    if (off) s_server.sendContent(line, off);
    s_server.sendContent("");
}

static void handleLogBin() {
    uint64_t end = logWritten();
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.sendHeader("Content-Disposition", "attachment; filename=\"uart-log.bin\"");
    s_server.send(200, "application/octet-stream", "");

    uint8_t buf[LOG_DUMP_CHUNK];
    uint64_t pos = logVisibleFrom();
    while (pos < end) {
        uint32_t n = logCopy(pos, buf, LOG_DUMP_CHUNK);
        if (!n) break;
        s_server.sendContent((const char *)buf, n);
        pos += n;
    }
    s_server.sendContent("");
}

// ---------------------------------------------------------------------------
// system
// ---------------------------------------------------------------------------

static void handleReboot() {
    SYSLOG("[sys] reboot requested from the web interface");
    okJson();
    s_server.client().flush();
    delay(750);
    ESP.restart();
}

static void handleFactory() {
    cfgErase();
    SYSLOG("[sys] configuration erased, rebooting into access point mode");
    okJson();
    s_server.client().flush();
    delay(750);
    ESP.restart();
}

// Multipart upload, for a firmware image you already have on disk.
//
// The web server calls the upload handler for every chunk and once more when
// the last one arrives, then calls the request handler. UPLOAD_FILE_END is seen
// by both, so the answer is tracked here to make sure exactly one reply goes
// out and the image is finalised exactly once.
static bool s_uploadAnswered = false;
static bool s_uploadStarted  = false;

static void handleUpload() {
    HTTPUpload &up = s_server.upload();

    if (up.status == UPLOAD_FILE_START) {
        s_uploadAnswered = false;
        s_uploadStarted  = true;
        // The multipart body has no usable size field up front, so the updater
        // is opened with the largest image the free slot could hold. Passing 0
        // here is rejected outright by the core.
        // The updater places the image so that it ENDS where the running
        // sketch's filesystem begins, so anything larger than one application
        // slot would spill over the running application and into LittleFS.
        uint32_t room = OTA_SLOT_BYTES;
        uint32_t free_sketch = ESP.getFreeSketchSpace();
        if (free_sketch && free_sketch < room) room = free_sketch;
        SYSLOG("[sys] firmware upload started (%s, up to %u bytes)",
               up.name.c_str(), (unsigned)room);
        if (!Update.begin(room, U_FLASH)) {
            s_uploadAnswered = true;
            failJson(Update.getErrorString());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.end(false);
            s_uploadAnswered = true;
            failJson(Update.getErrorString());
        }
    } else if (up.status == UPLOAD_FILE_END && !s_uploadAnswered) {
        s_uploadAnswered = true;
        if (Update.end(true)) {
            SYSLOG("[sys] firmware uploaded (%u bytes), rebooting",
                   (unsigned)up.totalSize);
            okJson();
            s_server.client().flush();
            delay(750);
            ESP.restart();
        } else {
            failJson(Update.getErrorString());
        }
    }
}

static void handleUploadDone() {
    if (s_uploadAnswered || !s_uploadStarted) {
        // Either the upload handler already replied, or there was no file part
        // at all and the upload callback never ran.
        if (!s_uploadStarted) {
            s_uploadAnswered = true;
            failJson("the request carried no firmware image");
        }
        return;
    }
    s_uploadAnswered = true;
    failJson("the firmware image ended without a closing marker");
}

// ---------------------------------------------------------------------------
// general purpose outputs
// ---------------------------------------------------------------------------

// Parses the pin argument without judging it, for the one operation that has
// to work on a pin which has since become unavailable.
static bool argPinRaw(int &pin) {
    if (!s_server.hasArg("pin")) return false;
    pin = s_server.arg("pin").toInt();
    return pin >= 0 && pin <= 16;
}

static bool argPin(int &pin) {
    return argPinRaw(pin) && gpioUsable((uint8_t)pin);
}

static void handleGpioGet() {
    noStore();
    s_server.send(200, "application/json", gpioJson());
}

static void handleGpioSet() {
    int pin;
    if (!argPinRaw(pin)) { failJson("pin must be between 0 and 16"); return; }
    if (!gpioUsable((uint8_t)pin)) {
        failJson("GPIO" + String(pin) + " is not available (" +
                 String(gpioWhy((uint8_t)pin)) + ")");
        return;
    }

    bool on = argOn("value") || s_server.arg("value") == "1";
    gpioSet((uint8_t)pin, on);
    okJson(",\"pin\":" + String(pin) + ",\"value\":" + String(on ? 1 : 0));
}

// Type into the device from the browser. Goes out exactly as a telnet client's
// keystrokes would, so it shares the same path, the same queue and the same
// back-pressure.
static bool hexNibble(char c) {
    if (c >= '0' && c <= '9') return true;
    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void handleUartSend() {
    String text = s_server.arg("text");
    String raw  = s_server.arg("raw");
    raw.trim();
    raw.toLowerCase();
    bool asHex = raw.length() > 0;

    String eol = s_server.arg("eol");
    eol.trim();
    eol.toLowerCase();

    uint8_t term[2];
    uint8_t nterm = 0;
    if      (eol == "cr")   term[nterm++] = '\r';
    else if (eol == "lf")   term[nterm++] = '\n';
    else if (eol == "crlf") { term[nterm++] = '\r'; term[nterm++] = '\n'; }
    else if (eol.length() && eol != "none")
                            { failJson("end of line must be cr, lf, crlf or none"); return; }

    // Control characters - Ctrl-C and the rest - cannot travel in the text
    // field: it is trimmed, and everything at or below a space goes with it.
    // `raw` is hex, so any byte can be sent exactly as it is.
    if (asHex && (raw.length() & 1)) {
        failJson("raw must be whole bytes of hex, e.g. 03");
        return;
    }
    for (uint32_t i = 0; asHex && i < raw.length(); i++)
        if (!hexNibble(raw[i])) { failJson("raw must be hex digits only"); return; }

    uint32_t payload = asHex ? raw.length() / 2 : text.length();
    if (!payload && !nterm) { failJson("nothing to send"); return; }

    // The telnet path is bounded by its transmit queue; this one is not, so the
    // length is capped here rather than by whatever the caller felt like.
    if (payload + nterm > SEND_MAX) {
        failJson("that is longer than " + String((uint32_t)SEND_MAX) + " bytes");
        return;
    }

    uint8_t buf[SEND_MAX];
    uint32_t n = 0;
    if (asHex) {
        for (uint32_t i = 0; i < payload; i++) {
            char hi = raw[i * 2], lo = raw[i * 2 + 1];
            buf[n++] = (uint8_t)((hi <= '9' ? hi - '0' : hi - 'a' + 10) * 16 +
                                 (lo <= '9' ? lo - '0' : lo - 'a' + 10));
        }
    } else {
        text.trim();
        memcpy(buf, text.c_str(), text.length());
        n = text.length();
    }
    for (uint8_t i = 0; i < nterm; i++) buf[n++] = term[i];

    if (!uartWrite(buf, n)) {
        failJson("the UART would not take it; try a shorter burst");
        return;
    }

    // uartWrite already records transmitted bytes when that option is on. When
    // it is off, telnet keystrokes stay out of the log - but this text was typed
    // into a box on screen, so it is not a secret and is worth keeping.
    if (!g_cfg.logTx) logWrite(buf, n, LOG_DIR_OUT);

    okJson(",\"sent\":" + String(n));
}

// Hand a pin back to the board: no longer an output, and no longer restored at
// boot, since the two cannot both be true.
static void handleGpioInput() {
    int pin;
    if (!argPinRaw(pin)) { failJson("pin must be between 0 and 16"); return; }
    if (!gpioInput((uint8_t)pin)) {
        failJson("GPIO" + String(pin) + " cannot be released");
        return;
    }
    okJson(",\"pin\":" + String(pin));
}

static void handleGpioToggle() {
    int pin;
    if (!argPin(pin)) { failJson("that pin cannot be driven"); return; }

    gpioToggle((uint8_t)pin);
    okJson(",\"pin\":" + String(pin) + ",\"value\":" +
           String(digitalRead(pin) ? 1 : 0));
}

static void handleGpioRemember() {
    int pin;
    if (!argPin(pin)) { failJson("that pin cannot be driven"); return; }

    String name = argStr("name");
    name.trim();
    if (name.length() >= GPIO_NAME_MAX + 1) { failJson("name too long"); return; }

    if (!gpioRemember((uint8_t)pin, name)) { failJson("no room for another pin"); return; }
    SYSLOG("[gpio] GPIO%u (%s) will be restored at boot",
           (unsigned)pin, g_cfg.gpio[g_cfg.gpioCount - 1].name);
    okJson();
}

static void handleGpioForget() {
    // Deliberately not argPin(): a pin remembered while it was still free has to
    // be forgettable after it stops being usable, or it is stuck there.
    int pin;
    if (!argPinRaw(pin)) { failJson("no such pin"); return; }

    if (!gpioForget((uint8_t)pin)) { failJson("that pin was not remembered"); return; }
    okJson();
}

static void handleUpdateCheck() {
    otaCheckNow();
    okJson(",\"ota\":" + otaJson());
}

static void handleUpdateStatus() {
    okJson(",\"ota\":" + otaJson());
}

static void handleUpdateFlash() {
    otaFlashNow();
    okJson(",\"ota\":" + otaJson());
}

// The web server core only decodes application/x-www-form-urlencoded bodies
// into arguments. A JSON body is dropped before the handler runs, so it used to
// answer {"ok":true} for a request that changed nothing at all. Refuse it.
typedef void (*Handler)();
static void postArgs(Handler next) {
    if (s_server.header("Content-Type").indexOf("application/json") >= 0) {
        s_server.send(415, "application/json",
                      "{\"ok\":false,\"error\":\"send fields as "
                      "application/x-www-form-urlencoded, not JSON\"}");
        return;
    }
    next();
}

// Phones and laptops probe these while looking for a login portal; answering
// with a redirect is what turns the unconfigured AP into a working setup page.
static void handleCaptive() {
    s_server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    s_server.send(302, "text/plain", "");
}

static void handleNotFound() {
    if (s_server.uri().startsWith("/api/")) { failJson("no such endpoint"); return; }
    s_server.sendHeader("Location", "/", true);
    s_server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------

void webBegin() {
    // Only headers named here are kept; without this the guard below cannot
    // see what the client said it was sending.
    s_server.collectHeaders("Content-Type");

    s_server.on("/", HTTP_GET, handleIndex);
    s_server.on("/app.css", HTTP_GET, handleCss);
    s_server.on("/app.js", HTTP_GET, handleJs);
    s_server.on("/favicon.ico", HTTP_GET, []() { s_server.send(204, "image/x-icon", ""); });

    s_server.on("/api/state", HTTP_GET, handleState);
    s_server.on("/api/scan", HTTP_GET, handleScan);
    s_server.on("/api/scan/start", HTTP_POST, []{ postArgs(handleScanStart); });

    s_server.on("/api/net", HTTP_POST, []{ postArgs(handleSaveNet); });
    s_server.on("/api/net/forget", HTTP_POST, []{ postArgs(handleForgetNet); });
    s_server.on("/api/uart", HTTP_POST, []{ postArgs(handleSaveUart); });
    s_server.on("/api/uart/send", HTTP_POST, []{ postArgs(handleUartSend); });

    s_server.on("/api/log", HTTP_GET, handleLog);
    s_server.on("/api/log/txt", HTTP_GET, handleLogTxt);
    s_server.on("/api/log/bin", HTTP_GET, handleLogBin);
    s_server.on("/api/log/clear", HTTP_POST, []{ postArgs(handleLogClear); });

    s_server.on("/api/gpio", HTTP_GET, handleGpioGet);
    s_server.on("/api/gpio/set", HTTP_POST, []{ postArgs(handleGpioSet); });
    s_server.on("/api/gpio/input", HTTP_POST, []{ postArgs(handleGpioInput); });
    s_server.on("/api/gpio/toggle", HTTP_POST, []{ postArgs(handleGpioToggle); });
    s_server.on("/api/gpio/remember", HTTP_POST, []{ postArgs(handleGpioRemember); });
    s_server.on("/api/gpio/forget", HTTP_POST, []{ postArgs(handleGpioForget); });

    s_server.on("/api/system/reboot", HTTP_POST, []{ postArgs(handleReboot); });
    s_server.on("/api/system/factory", HTTP_POST, []{ postArgs(handleFactory); });
    s_server.on("/update", HTTP_POST, handleUploadDone, handleUpload);

    s_server.on("/api/update/check", HTTP_POST, []{ postArgs(handleUpdateCheck); });
    s_server.on("/api/update/status", HTTP_GET, handleUpdateStatus);
    s_server.on("/api/update/flash", HTTP_POST, []{ postArgs(handleUpdateFlash); });

    s_server.on("/generate_204", HTTP_ANY, handleCaptive);
    s_server.on("/gen_204", HTTP_ANY, handleCaptive);
    s_server.on("/hotspot-detect.html", HTTP_ANY, handleCaptive);
    s_server.on("/fwlink", HTTP_ANY, handleCaptive);
    s_server.on("/connecttest.txt", HTTP_ANY, handleCaptive);
    s_server.onNotFound(handleNotFound);

    s_server.begin();
    SYSLOG("[web] configuration interface on port %d", (int)WEB_PORT);
}

void webLoop() {
    s_server.handleClient();
}
