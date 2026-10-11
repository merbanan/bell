#include "ota.h"
#include "config.h"
#include "log.h"
#include "bell.h"

#include <Updater.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#define OTA_CHUNK   1024
#define OTA_BUDGET  12288   // bytes downloaded per loop() pass

static OtaState   s_state     = OTA_IDLE;
static String     s_remoteVer;
static String     s_binUrl;
static String     s_notes;
static uint32_t   s_total     = 0;
static uint32_t   s_written   = 0;
static String     s_err;
static uint32_t   s_rebootAt  = 0;
static uint32_t   s_lastByte  = 0;   // stall detection while streaming
static bool       s_available = false;

#define OTA_STALL_MS 20000

static HTTPClient s_http;
static bool       s_httpOpen  = false;

// "1.2.10" > "1.2.9"; missing components count as zero.
int otaCompareVersion(const String &a, const String &b) {
    int va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    sscanf(a.c_str(), "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b.c_str(), "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

static void fail(const String &why) {
    SYSLOG("[ota] %s", why.c_str());
    s_err = why;
    s_state = OTA_FAILED;
    if (s_httpOpen) {
        s_http.end();
        s_httpOpen = false;
    }
}

void otaCheckNow() {
    if (otaBusy() || s_state == OTA_REBOOTING) return;
    if (!g_cfg.otaUrl[0]) {
        s_err = "no update URL configured";
        s_state = OTA_FAILED;
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        s_err = "no network connection";
        s_state = OTA_FAILED;
        return;
    }

    s_err = "";
    s_remoteVer = "";
    s_binUrl = "";
    s_notes = "";
    s_available = false;
    s_state = OTA_CHECKING;

    SYSLOG("[ota] checking %s", g_cfg.otaUrl);

    WiFiClient plain;
    WiFiClientSecure tls;
    s_http.setTimeout(12000);
    s_http.setReuse(false);
    if (String(g_cfg.otaUrl).startsWith("https://")) {
        // No bundled trust store on a device this size: the manifest is fetched
        // over TLS but the certificate is not pinned. Point it at a host you
        // control, and treat the release page as the real source of truth.
        tls.setInsecure();
        s_http.begin(tls, g_cfg.otaUrl);
    } else {
        s_http.begin(plain, g_cfg.otaUrl);
    }
    s_httpOpen = true;

    int code = s_http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "manifest request failed (HTTP %d)", code);
        fail(buf);
        return;
    }

    String body = s_http.getString();
    s_http.end();
    s_httpOpen = false;

    JSON_DOC(d, 768);
    if (deserializeJson(d, body) || d.overflowed()) {
        fail(d.overflowed() ? "manifest too large to parse"
                            : "manifest is not valid JSON");
        return;
    }

    JsonObjectConst root = d.as<JsonObjectConst>();
    s_remoteVer = root["version"] | root["latest"] | "";
    s_binUrl    = root["url"]    | root["bin"]   | root["file"] | "";
    s_notes     = root["notes"]  | "";

    if (s_remoteVer.isEmpty() || s_binUrl.isEmpty()) {
        fail("manifest needs a \"version\" and a \"url\" field");
        return;
    }

    int cmp = otaCompareVersion(s_remoteVer, FW_VERSION);
    s_available = cmp > 0;
    s_state = OTA_READY;

    SYSLOG("[ota] running %s, manifest offers %s -> %s", FW_VERSION, s_remoteVer.c_str(),
           cmp > 0 ? "update available" : (cmp == 0 ? "up to date" : "manifest is older"));
}

void otaFlashNow() {
    if (otaBusy() || s_state == OTA_REBOOTING) return;
    if (s_binUrl.isEmpty()) {
        fail("nothing to flash, run a check first");
        return;
    }

    s_err = "";
    s_written = 0;
    s_lastByte = millis();
    s_state = OTA_DOWNLOADING;
    SYSLOG("[ota] downloading %s", s_binUrl.c_str());

    WiFiClient plain;
    WiFiClientSecure tls;
    s_http.setTimeout(20000);
    s_http.setReuse(false);
    if (s_binUrl.startsWith("https://")) {
        tls.setInsecure();
        s_http.begin(tls, s_binUrl);
    } else {
        s_http.begin(plain, s_binUrl);
    }
    s_httpOpen = true;

    int code = s_http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "download failed (HTTP %d)", code);
        fail(buf);
        return;
    }

    // Without a Content-Length there is no way to know whether the transfer
    // finished early, and a truncated image would leave the next boot without
    // a valid application: refuse instead.
    int reported = s_http.getSize();
    if (reported <= 0) {
        fail("server did not report the image size");
        return;
    }
    s_total = (uint32_t)reported;
    if (s_total > OTA_SLOT_BYTES) {
        // Writing it anyway would land past the slot and take LittleFS - and
        // the saved configuration - with it.
        char buf[96];
        snprintf(buf, sizeof(buf), "image is %u bytes, the slot holds %u",
                 (unsigned)s_total, (unsigned)OTA_SLOT_BYTES);
        fail(buf);
        return;
    }

    if (!Update.begin(s_total)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "not enough flash for a %u byte image: %s",
                 (unsigned)s_total, Update.getErrorString().c_str());
        fail(buf);
        return;
    }
    SYSLOG("[ota] flashing %u bytes", (unsigned)s_total);
}

void otaLoop() {
    if (s_state == OTA_REBOOTING) {
        if ((int32_t)(millis() - s_rebootAt) >= 0) ESP.restart();
        return;
    }
    if (s_state != OTA_DOWNLOADING) return;

    WiFiClient *stream = s_http.getStreamPtr();
    if (!stream) {
        Update.end(false);
        fail("download stream vanished");
        return;
    }

    // A read that blocks for the client's full timeout would freeze the web
    // interface and every telnet session along with it.
    stream->setTimeout(3000);

    static uint8_t buf[OTA_CHUNK];
    uint32_t budget = OTA_BUDGET;

    while (budget && stream->available() > 0) {
        size_t n = stream->readBytes(buf, min((size_t)OTA_CHUNK, (size_t)budget));
        if (n == 0) break;
        if (Update.write(buf, n) != n) {
            Update.end(false);
            fail(Update.getErrorString());
            return;
        }
        s_written += n;
        s_lastByte = millis();
        budget -= n;
        yield();   // the telnet bridge and this page share the same loop
    }

    if (s_written < s_total) {
        // The socket went quiet before the image was complete. Sitting here
        // forever would leave the device in a downloading state with nothing
        // left to download.
        if (millis() - s_lastByte > OTA_STALL_MS) {
            s_http.end();
            s_httpOpen = false;
            Update.end(false);
            char buf2[96];
            snprintf(buf2, sizeof(buf2), "download stalled after %u of %u bytes",
                     (unsigned)s_written, (unsigned)s_total);
            fail(buf2);
        }
        return;
    }

    s_http.end();
    s_httpOpen = false;

    if (Update.end(true)) {
        SYSLOG("[ota] image written, rebooting");
        s_state = OTA_REBOOTING;
        s_rebootAt = millis() + 1500;
    } else {
        fail(Update.getErrorString());
    }
}

bool otaBusy() {
    return s_state == OTA_CHECKING || s_state == OTA_DOWNLOADING;
}

OtaState otaState() { return s_state; }

String otaJson() {
    static const char *names[] = {"idle", "checking", "ready", "downloading",
                                  "rebooting", "failed"};
    String s = "{";
    s += "\"state\":\"" + String(names[s_state]) + "\"";
    s += ",\"current\":\"" + String(FW_VERSION) + "\"";
    s += ",\"latest\":\"" + jsonEscape(s_remoteVer) + "\"";
    s += ",\"available\":" + String(s_available ? "true" : "false");
    s += ",\"notes\":\"" + jsonEscape(s_notes) + "\"";
    s += ",\"url\":\"" + jsonEscape(s_binUrl) + "\"";
    s += ",\"written\":" + String(s_written);
    s += ",\"total\":" + String(s_total);
    s += ",\"error\":\"" + jsonEscape(s_err) + "\"";
    s += "}";
    return s;
}
