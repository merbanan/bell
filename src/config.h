#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "bell.h"

// JSON helpers that work out of a static pool: the documents here are small
// and short lived, and on a device whose scarcest resource is heap they must
// not take any. Allocator is bump-only, which is all JsonDocument asks for.
template <size_t N>
class PooledDoc : public ArduinoJson::Allocator {
public:
    PooledDoc() : doc(this), _base(reinterpret_cast<char *>(_pool)),
                  _cap(sizeof(_pool)), _used(0) {}

    // Reclaims the whole pool. Needed because the documents below are declared
    // as function statics: a bump allocator would otherwise hand out fresh
    // space on every call until the pool ran dry.
    void reset() {
        doc.clear();
        _used = 0;
    }

    void *allocate(size_t size) override {
        char *block = take(size);
        if (!block) return nullptr;
        *reinterpret_cast<uint32_t *>(block) = (uint32_t)size;
        return block + 8;
    }

    void deallocate(void *) override {}

    void *reallocate(void *ptr, size_t size) override {
        if (!ptr) return allocate(size);

        size_t old  = *reinterpret_cast<uint32_t *>(static_cast<char *>(ptr) - 8);
        char  *block = static_cast<char *>(ptr) - 8;

        // Serialisation builds strings by doubling, and every doubling would
        // otherwise leave the abandoned copy behind. Growing the newest block
        // in place is what keeps this pool small enough to live in static RAM.
        size_t have = slot(old);
        if (block + have == _base + _used) {
            size_t need = slot(size);
            if (_used - have + need <= _cap) {
                _used += need - have;
                *reinterpret_cast<uint32_t *>(block) = (uint32_t)size;
                return ptr;
            }
            return nullptr;   // the newest block can only grow, and it will not fit
        }

        void *fresh = allocate(size);
        if (!fresh) return nullptr;
        memcpy(fresh, ptr, old < size ? old : size);
        return fresh;
    }

    JsonDocument doc;

private:
    static size_t slot(size_t size) { return (size + 8 + 15u) & ~(size_t)15u; }

    char *take(size_t size) {
        size_t need = slot(size);
        if (_used + need > _cap) return nullptr;
        char *block = _base + _used;
        _used += need;
        return block;
    }

    uint64_t _pool[(N + 7) / 8];
    char    *_base;
    size_t   _cap;
    size_t   _used;
};

// A JSON document backed by static memory. The pool is a function static, so
// this costs no stack - which matters when it is used inside a web request
// handler running on the 4 kB loop() stack.
#define JSON_DOC(name, sz)                       \
    static PooledDoc<sz> name##_pool;            \
    name##_pool.reset();                         \
    JsonDocument &name = name##_pool.doc

// Uses a pool that already exists rather than declaring a new one. Loading and
// saving never overlap, so they share one - and they need the same generous
// size: parsing a 400 byte file costs well over 2 kB, because ArduinoJson
// allocates a node per string and a fresh variant pool whenever one fills.
#define JSON_DOC_INTO(pool, name)                \
    pool.reset();                                \
    JsonDocument &name = pool.doc

// How many bytes the log ring should get. Derived from the free heap so the
// buffer is as large as the module can carry without starving the rest of the
// firmware, rounded down to a power of two (cheap index masking).
uint32_t capLogBytes();

// Escapes a string for inclusion in a hand-built JSON document. Used for
// anything that came from a remote network or from a saved setting, because
// both the telnet client and the update manifest are outside our control.
String   jsonEscape(const String &in);

void   cfgLoad();
bool   cfgSave();
bool   cfgErase();                 // forget everything, back to defaults on next boot
void   cfgDefaults();
bool   cfgConfigured();     // true once a station SSID has been stored
bool   cfgDirty();
void   cfgMarkDirty();

// One pin the firmware drives as an output and restores on the next boot.
struct GpioEntry {
    uint8_t pin;
    uint8_t value;
    char    name[GPIO_NAME_MAX + 1];
};

struct Config {
    uint16_t telnetPort;
    uint16_t telnetPortRo;                 // 0 = follow, one above telnetPort

    // station
    char  staSsid[MAX_SSID];
    char  staPass[MAX_STA_PASS];
    char  hostname[MAX_HOSTNAME];
    uint8_t staTimeout;                   // seconds before falling back to AP

    // access point used for provisioning / fallback
    char  apSsid[MAX_SSID];
    char  apPass[MAX_AP_PASS];
    uint8_t apChannel;
    bool  apHidden;

    // uart
    uint32_t baud;
    uint8_t bits;                         // 5..8
    char    parity;                       // 'N', 'E', 'O', 'M', 'S'
    uint8_t stops;                        // 1 or 2
    bool    swapPins;                     // UART on GPIO13/15 instead of GPIO1/3
    bool    localEcho;                    // mirror client input back to that client
    bool    crlfTx;                       // translate LF->CRLF on the way to the UART
    bool    logTx;                        // also append transmitted bytes to the log
    bool    clearOnBoot;
    bool    statusLed;        // blink while booting, steady once ready
    uint16_t ledSeconds;      // how long it stays lit after boot; 0 = always

    // general purpose outputs
    uint8_t   gpioCount;
    GpioEntry gpio[MAX_GPIO_PINS];

    // firmware update
    char    otaUrl[MAX_URL];              // http(s) URL of a JSON manifest
    bool    otaAuto;
};

extern Config g_cfg;

// Sized from measurement rather than from the size of the document: see above.
#define CFG_POOL_BYTES 3072
