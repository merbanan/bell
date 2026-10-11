// Differential test for the firmware's UART log ring.
//
// The ring keeps absolute 64 bit head/tail counters and derives the physical
// slot with a power-of-two mask, which is the sort of arithmetic that looks
// right and is not. So: run the real logWrite/logCopy against a plain deque
// used as a reference model, over randomised sequences, and compare.
//
// Run it through test/native/run.sh, which sets up the Arduino shim and copies
// the current src/log.cpp next to it.

#include <deque>
#include <random>
#include <vector>

#include "log.h"

static uint32_t g_capBytes = 8192;
uint32_t capLogBytes() { return g_capBytes; }

static uint32_t g_now = 0;
uint32_t millis() { return g_now; }

EspClass ESP;
static bool g_quiet = false;

#define LOG_SIZE 8192

// Reference model. g_ref holds every byte the firmware has been fed; g_base is
// the absolute log index of g_ref[0]. logInit() writes a banner line through the
// real syslog(), so the model adopts the ring's state after that instead of
// assuming the log starts empty.
static std::deque<uint8_t> g_ref;
static uint64_t g_base = 0;
static uint64_t g_viewFrom = 0;

static void adoptRing() {
    uint8_t tmp[16384];
    uint64_t from = logOldest();
    uint32_t n = logCopy(from, tmp, sizeof(tmp));
    g_ref.assign(tmp, tmp + n);
    g_base = from;
    g_viewFrom = logVisibleFrom();
}

static int g_fail = 0;

static void check(bool ok, const char *what, uint64_t a = 0, uint64_t b = 0) {
    if (ok) return;
    printf("  FAIL %s (%llu vs %llu)\n", what,
           (unsigned long long)a, (unsigned long long)b);
    g_fail++;
}

// Expected answers derived from the reference model.
static uint64_t refWritten() { return g_base + g_ref.size(); }
static uint64_t refOldest() {
    uint64_t w = refWritten();
    return w - g_base > LOG_SIZE ? w - LOG_SIZE : g_base;
}
static uint64_t refVisible() { uint64_t o = refOldest(); return g_viewFrom > o ? g_viewFrom : o; }

static void write(const std::vector<uint8_t> &data) {
    if (data.empty()) return;
    logWrite(data.data(), (uint32_t)data.size());
    // logWrite() clamps a single write to the ring capacity and keeps the
    // newest bytes; the model has to do the same.
    size_t skip = data.size() > LOG_SIZE ? data.size() - LOG_SIZE : 0;
    for (size_t i = skip; i < data.size(); i++) g_ref.push_back(data[i]);
    g_now += 3;
}

static void modelClear() {
    g_viewFrom = refWritten();
    logClear();
}

// Reads `max` bytes at `from`, exactly like the /api/log endpoint.
static uint32_t read(uint64_t from, uint32_t max, std::vector<uint8_t> &out) {
    out.assign(max, 0);
    uint64_t vis = refVisible();
    if (from < vis) from = vis;
    if (from >= refWritten()) { out.assign(0, 0); return 0; }
    uint32_t n = (uint32_t)((refWritten() - from) < max ? (refWritten() - from) : max);
    out.assign(n, 0);
    for (uint32_t i = 0; i < n; i++) out[i] = g_ref[(size_t)(from + i)];
    return n;
}

static void compareAll(const char *tag) {
    check(logWritten() == refWritten(), tag, logWritten(), refWritten());
    check(logVisibleFrom() == refVisible(), tag, logVisibleFrom(), refVisible());
    check(logCapacity() == LOG_SIZE, tag, logCapacity(), LOG_SIZE);

    static const uint64_t probes[] = {0, 1, 7, 100, LOG_SIZE - 1, LOG_SIZE,
                                      LOG_SIZE + 1, 2 * LOG_SIZE, 2 * LOG_SIZE + 5};
    uint8_t got[16384];
    std::vector<uint8_t> want;
    for (uint64_t p : probes) {
        if (p > refWritten() + 10) continue;
        uint32_t max = (uint32_t)(p % 6000) + 1;
        uint32_t n = logCopy(p, got, max);
        uint32_t w = read(p, max, want);
        bool same = n == w && memcmp(got, want.data(), n) == 0;
        check(same, tag, p, n);
    }
}

int main() {
    g_quiet = true;

    printf("ring log differential test\n");
    g_capBytes = LOG_SIZE;
    check(logInit(), "logInit");
    adoptRing();

    // 1. nothing written yet beyond the boot banner
    compareAll("boot banner");
    {
        uint8_t buf[256];
        std::vector<uint8_t> want;
        uint32_t got = logCopy(0, buf, sizeof(buf));
        uint32_t exp = read(0, sizeof(buf), want);
        check(got == exp && memcmp(buf, want.data(), got) == 0, "boot banner readback");
    }

    // 2. straight writes
    std::vector<uint8_t> d;
    for (int i = 0; i < 100; i++) d.push_back((uint8_t)i);
    write(d);
    compareAll("first write");

    // 3. random traffic well past one lap
    std::mt19937 rng(12345);
    for (int round = 0; round < 4000; round++) {
        uint32_t n = rng() % 700 + 1;
        std::vector<uint8_t> chunk(n);
        for (uint32_t i = 0; i < n; i++) chunk[i] = (uint8_t)rng();
        write(chunk);

        if (round % 250 == 0) compareAll("random traffic");
        if (round % 900 == 0) modelClear();
        if (round % 1300 == 0) logClear(), g_viewFrom = refWritten();
    }
    compareAll("after random traffic");

    // 4. a single write larger than the whole ring
    {
        std::vector<uint8_t> huge(LOG_SIZE * 3);
        for (size_t i = 0; i < huge.size(); i++) huge[i] = (uint8_t)(i * 7);
        write(huge);
        compareAll("oversized write");
        uint8_t buf[64];
        uint32_t n = logCopy(refWritten() - 64, buf, 64);
        check(n == 64, "oversized write keeps the tail", n, 64);
        bool ok = true;
        for (uint32_t i = 0; i < 64; i++)
            if (buf[i] != (uint8_t)(((huge.size() - 64 + i) * 7) & 0xff)) ok = false;
        check(ok, "oversized write content");
    }

    // 5. 64 bit counter really is 64 bit: simulate 5 GB of traffic cheaply
    {
        logClear();
        g_viewFrom = refWritten();
        std::vector<uint8_t> lap(LOG_SIZE);
        for (uint32_t i = 0; i < LOG_SIZE; i++) lap[i] = (uint8_t)(i ^ 0x5a);
        uint64_t laps = (uint64_t)6 * 1024 * 1024 * 1024 / LOG_SIZE;   // 6 GB
        for (uint64_t i = 0; i < laps; i++) logWrite(lap.data(), LOG_SIZE);
        check(logWritten() > (uint64_t)5ull * 1024 * 1024 * 1024, "counter past 2^32",
              (unsigned long long)logWritten(), 0);
        uint8_t buf[32];
        uint64_t head = logWritten();
        uint32_t n = logCopy(head - 32, buf, 32);
        check(n == 32, "read at the far end", n, 32);
        bool ok = n == 32;
        if (ok) for (uint32_t i = 0; i < 32; i++) if (buf[i] != lap[LOG_SIZE - 32 + i]) ok = false;
        check(ok, "content correct past the 32 bit wrap");
    }

    printf(g_fail ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
