#include "config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>

#include "log.h"

Config g_cfg;

static bool s_dirty = false;
static PooledDoc<CFG_POOL_BYTES> s_cfgPool;

String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
    for (uint32_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\')        { out += '\\'; out += c; }
        else if (c == '\n')               { out += "\\n"; }
        else if (c == '\r')               { out += "\\r"; }
        else if (c == '\t')               { out += "\\t"; }
        else if ((uint8_t)c < 0x20)        { out += '.'; }
        else                               { out += c; }
    }
    return out;
}

uint32_t capLogBytes() {
    uint32_t heap = ESP.getFreeHeap();
    if (heap <= LOG_HEAP_KEEP) return LOG_MIN_BYTES;
    uint32_t room = heap - LOG_HEAP_KEEP;
    uint32_t cap = LOG_MIN_BYTES;
    while ((cap << 1) <= room && cap < LOG_MAX_BYTES) cap <<= 1;
    if (cap > LOG_MAX_BYTES) cap = LOG_MAX_BYTES;
    return cap;
}

void cfgDefaults() {
    g_cfg.telnetPort = DEFAULT_TELNET_PORT;
    g_cfg.telnetPortRo = DEFAULT_TELNET_PORT_RO;

    strlcpy(g_cfg.staSsid, "", sizeof(g_cfg.staSsid));
    strlcpy(g_cfg.staPass, "", sizeof(g_cfg.staPass));
    strlcpy(g_cfg.hostname, "uart-server", sizeof(g_cfg.hostname));
    g_cfg.staTimeout = 20;

    snprintf(g_cfg.apSsid, sizeof(g_cfg.apSsid), "uart-server-%02X%02X",
             ESP.getChipId() >> 8, ESP.getChipId() & 0xff);
    strlcpy(g_cfg.apPass, "uartserver", sizeof(g_cfg.apPass));
    g_cfg.apChannel = 0;
    g_cfg.apHidden  = false;

    // No host configured means the uplink stays off: the module still runs its
    // own telnet server and behaves exactly as it did before this existed.
    strlcpy(g_cfg.hostHost, "", sizeof(g_cfg.hostHost));
    g_cfg.hostPort = DEFAULT_HOST_PORT;

    g_cfg.baud    = 115200;
    g_cfg.bits    = 8;
    g_cfg.parity  = 'N';
    g_cfg.stops   = 1;
    // GPIO1/GPIO3 are the pins the USB-serial chip is wired to on almost every
    // NodeMCU, so they double as the debug console and the ROM loader's output.
    // The bridge is more useful on GPIO15/GPIO13, which leaves the console and
    // the flashing path alone. GPIO15 has to be low at reset, which it is on
    // an unmodified dev board.
    g_cfg.swapPins = true;
    g_cfg.localEcho = false;
    g_cfg.crlfTx  = false;
    g_cfg.logTx   = false;
    g_cfg.clearOnBoot = false;
    g_cfg.statusLed = true;
    // Long enough to see it settle, short enough that it is not a night light.
    g_cfg.ledSeconds = 30;

    // The manifest lives with the source, and points at the newest release
    // asset, so publishing a release is all it takes to offer an update.
    strlcpy(g_cfg.otaUrl,
            "https://raw.githubusercontent.com/merbanan/bell/master/firmware.json",
            sizeof(g_cfg.otaUrl));
    g_cfg.otaAuto = false;
}

static void cp(JsonObjectConst src, const char *key, char *dst, size_t n) {
    const char *v = src[key];
    if (v) strlcpy(dst, v, n);
}

void cfgLoad() {
    cfgDefaults();

    JSON_DOC_INTO(s_cfgPool, d);

    File f = LittleFS.open(CFG_PATH, "r");
    if (!f) {
        cfgDefaults();          // nothing stored yet: this is a fresh module
        return;
    }

    DeserializationError err = deserializeJson(d, f);
    f.close();

    if (err || d.overflowed()) {
        // Falling back to defaults here would silently discard settings that
        // are perfectly fine in the flash, so say so.
        SYSLOG("[cfg] cannot read %s (%s%s) - using defaults",
               CFG_PATH, err.c_str(), d.overflowed() ? ", pool too small" : "");
        cfgDefaults();
        return;
    }

    JsonObjectConst root = d.as<JsonObjectConst>();
    if (!root["magic"].as<const char *>() ||
        strcmp(root["magic"].as<const char *>(), CFG_MAGIC) != 0) {
        cfgDefaults();
        return;
    }

    int version = root["version"] | 0;
    if (version > CFG_VERSION) {
        // Written by a newer build: unknown keys may exist, but the ones that
        // are here still mean what they meant.
        SYSLOG("[cfg] configuration was written by a newer firmware (v%d)", version);
    }

    int port = root["telnetPort"] | 0;
    g_cfg.telnetPort = (port >= 1 && port <= 65535) ? (uint16_t)port : DEFAULT_TELNET_PORT;

    // Absent in a config written before the monitor port existed, so give it
    // the default rather than a port nobody chose. A stored 0 is a deliberate
    // "follow the read/write port" and is kept as it is.
    long portRo = root["telnetPortRo"].isNull()
                    ? (long)DEFAULT_TELNET_PORT_RO
                    : (long)(root["telnetPortRo"] | 0);
    g_cfg.telnetPortRo = (portRo >= 0 && portRo <= 65535) ? (uint16_t)portRo
                                                          : DEFAULT_TELNET_PORT_RO;

    JsonObjectConst w = root["wifi"];
    if (w) {
        cp(w, "ssid", g_cfg.staSsid, sizeof(g_cfg.staSsid));
        cp(w, "pass", g_cfg.staPass, sizeof(g_cfg.staPass));
        cp(w, "hostname", g_cfg.hostname, sizeof(g_cfg.hostname));
        if (w["timeout"].is<int>()) g_cfg.staTimeout = constrain(w["timeout"].as<int>(), 3, 120);
    }

    JsonObjectConst a = root["ap"];
    if (a) {
        cp(a, "ssid", g_cfg.apSsid, sizeof(g_cfg.apSsid));
        cp(a, "pass", g_cfg.apPass, sizeof(g_cfg.apPass));
        if (a["channel"].is<int>()) g_cfg.apChannel = constrain(a["channel"].as<int>(), 0, 13);
        g_cfg.apHidden = a["hidden"] | false;
    }

    // Written by firmware before the uplink existed: absent keys mean "not
    // configured", which cfgDefaults() already left in place.
    JsonObjectConst h = root["host"];
    if (h) {
        cp(h, "addr", g_cfg.hostHost, sizeof(g_cfg.hostHost));
        int hp = h["port"] | 0;
        if (hp >= 1 && hp <= 65535) g_cfg.hostPort = (uint16_t)hp;
    }

    JsonObjectConst u = root["uart"];
    if (u) {
        if (u["baud"].is<unsigned long>())
            g_cfg.baud = constrain(u["baud"].as<unsigned long>(), 300UL, 921600UL);
        if (u["bits"].is<int>())    g_cfg.bits   = constrain(u["bits"].as<int>(), 5, 8);
        const char *p = u["parity"];
        if (p && p[0] && !p[1] && strchr("NEO", p[0])) g_cfg.parity = p[0];
        if (u["stops"].is<int>())   g_cfg.stops  = u["stops"].as<int>() == 2 ? 2 : 1;
        g_cfg.swapPins   = u["swapPins"]   | false;
        g_cfg.localEcho  = u["localEcho"]  | false;
        g_cfg.crlfTx     = u["crlfTx"]     | false;
        g_cfg.clearOnBoot= u["clearOnBoot"]| false;
        g_cfg.statusLed  = u["statusLed"]  | true;
        if (u["ledSeconds"].is<unsigned long>())
            g_cfg.ledSeconds = constrain(u["ledSeconds"].as<unsigned long>(), 0UL, 3600UL);
        if (u["flowLed"].is<bool>()) g_cfg.statusLed = u["flowLed"] | true;
    }

    JsonObjectConst o = root["ota"];
    if (o) {
        cp(o, "manifest", g_cfg.otaUrl, sizeof(g_cfg.otaUrl));
        g_cfg.otaAuto = o["auto"] | false;
    }

    JsonVariantConst pins = root["gpio"];
    if (!pins.isNull()) {
        g_cfg.gpioCount = 0;
        JsonArrayConst arr = pins.as<JsonArrayConst>();
        for (JsonObjectConst p : arr) {
            if (g_cfg.gpioCount >= MAX_GPIO_PINS) break;
            int pin = p["pin"] | -1;
            if (pin < 0 || pin > 16) continue;
            GpioEntry &e = g_cfg.gpio[g_cfg.gpioCount++];
            e.pin   = (uint8_t)pin;
            e.value = p["value"] | 0;
            cp(p, "name", e.name, sizeof(e.name));
        }
    }
}

bool cfgSave() {
    JSON_DOC_INTO(s_cfgPool, d);
    JsonObject root = d.to<JsonObject>();
    root["magic"]     = CFG_MAGIC;
    root["version"]   = CFG_VERSION;

    root["telnetPort"] = g_cfg.telnetPort;
    root["telnetPortRo"] = g_cfg.telnetPortRo;

    JsonObject w = root["wifi"].to<JsonObject>();
    w["ssid"]     = g_cfg.staSsid;
    w["pass"]     = g_cfg.staPass;
    w["hostname"] = g_cfg.hostname;
    w["timeout"]  = g_cfg.staTimeout;

    JsonObject a = root["ap"].to<JsonObject>();
    a["ssid"]    = g_cfg.apSsid;
    a["pass"]    = g_cfg.apPass;
    a["channel"] = g_cfg.apChannel;
    a["hidden"]  = g_cfg.apHidden;

    JsonObject h = root["host"].to<JsonObject>();
    h["addr"] = g_cfg.hostHost;
    h["port"] = g_cfg.hostPort;

    JsonObject u = root["uart"].to<JsonObject>();
    u["baud"]        = g_cfg.baud;
    u["bits"]        = g_cfg.bits;
    u["parity"]      = String(g_cfg.parity);
    u["stops"]       = g_cfg.stops;
    u["swapPins"]    = g_cfg.swapPins;
    u["localEcho"]   = g_cfg.localEcho;
    u["crlfTx"]      = g_cfg.crlfTx;
    u["logTx"]       = g_cfg.logTx;
    u["clearOnBoot"] = g_cfg.clearOnBoot;
    u["statusLed"]   = g_cfg.statusLed;
    u["ledSeconds"]  = g_cfg.ledSeconds;

    JsonArray pins = root["gpio"].to<JsonArray>();
    for (uint8_t i = 0; i < g_cfg.gpioCount; i++) {
        JsonObject p = pins.add<JsonObject>();
        p["pin"]   = g_cfg.gpio[i].pin;
        p["value"] = g_cfg.gpio[i].value;
        p["name"]  = g_cfg.gpio[i].name;
    }

    JsonObject o = root["ota"].to<JsonObject>();
    o["manifest"] = g_cfg.otaUrl;
    o["auto"]     = g_cfg.otaAuto;

    // Refuse to write a document that did not fit: a truncated config.json
    // would look valid and then lose most of itself on the next boot.
    if (d.overflowed()) return false;

    File f = LittleFS.open(CFG_PATH, "w");
    if (!f) return false;
    bool ok = serializeJson(d, f) > 0;
    f.close();
    if (ok) s_dirty = false;
    return ok;
}

bool cfgErase() {
    cfgDefaults();
    s_dirty = false;
    return LittleFS.remove(CFG_PATH);
}

bool cfgConfigured() {
    return g_cfg.staSsid[0] != '\0';
}

void cfgMarkDirty() { s_dirty = true; }

bool cfgDirty() {
    return s_dirty;
}
