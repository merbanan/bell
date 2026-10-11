#include "log.h"
#include "config.h"

#include <stdarg.h>

// Absolute head/tail counters; the physical slot is `(uint32_t)pos & s_mask`.
static uint8_t *s_buf      = nullptr;
static uint32_t s_cap      = 0;
static uint32_t s_mask     = 0;
static uint64_t s_head     = 0;   // absolute index of the next byte to store
static uint64_t s_tail     = 0;   // absolute index of the oldest retained byte
static uint64_t s_viewFrom = 0;   // first absolute index the UI may still read
static uint32_t s_hw       = 0;

static char     s_pre[768];
static uint32_t s_preLen = 0;

String logUptime(uint32_t ms) {
    uint32_t s = ms / 1000;
    char out[16];
    snprintf(out, sizeof(out), "%02u:%02u:%02u.%03u",
             (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
             (unsigned)(ms % 1000));
    return String(out);
}

bool logInit() {
    if (s_buf) {
        free(s_buf);
        s_buf = nullptr;
    }

    uint32_t cap = capLogBytes();
    if (cap < LOG_MIN_BYTES) cap = LOG_MIN_BYTES;

    s_buf = (uint8_t *)malloc(cap);
    if (!s_buf) {
        SYSLOG("[log] allocation of %u bytes failed, logging disabled", (unsigned)cap);
        return false;
    }

    s_cap      = cap;
    s_mask     = cap - 1;
    s_head     = 0;
    s_tail     = 0;
    s_viewFrom = 0;
    s_hw       = 0;

    SYSLOG("[log] ring buffer %u bytes, %u bytes of heap still free",
           (unsigned)cap, (unsigned)ESP.getFreeHeap());

    if (s_preLen) {
        logWrite((const uint8_t *)s_pre, s_preLen);
        s_preLen = 0;
    }
    return true;
}
// Direction runs, kept beside the ring rather than inside it. A byte per byte
// would cost a sixteenth of the ring again; in practice the direction only
// changes when someone speaks, so a run per change is nearly free. The table
// is fixed and static: when it wraps, the oldest directions are simply lost and
// the page draws no marker for those bytes rather than guessing.
#define LOG_DIR_RUNS 96
struct LogDirRun { uint64_t pos; uint32_t len; uint8_t dir; };
static LogDirRun s_runs[LOG_DIR_RUNS];
static uint8_t   s_runHead = 0;
static uint8_t   s_runCount = 0;

static void noteDir(uint64_t pos, uint32_t n, uint8_t dir) {
    if (!n) return;

    if (s_runCount) {
        LogDirRun &r = s_runs[(s_runHead + LOG_DIR_RUNS - 1) % LOG_DIR_RUNS];
        if (r.dir == dir && r.pos + r.len == pos) { r.len += n; return; }
    }
    LogDirRun &r = s_runs[s_runHead];
    r.pos = pos; r.len = n; r.dir = dir;
    s_runHead = (uint8_t)((s_runHead + 1) % LOG_DIR_RUNS);
    if (s_runCount < LOG_DIR_RUNS) s_runCount++;
}

uint32_t logDirRuns(uint64_t from, uint64_t to, String &out) {
    out += '[';
    uint8_t written = 0;
    uint8_t start = (uint8_t)((s_runHead + LOG_DIR_RUNS - s_runCount) % LOG_DIR_RUNS);

    for (uint8_t i = 0; i < s_runCount; i++) {
        const LogDirRun &r = s_runs[(start + i) % LOG_DIR_RUNS];
        uint64_t a = r.pos, b = r.pos + r.len;
        if (b <= from || a >= to) continue;

        if (a < from) a = from;                 // the page starts mid-run
        if (b > to)   b = to;
        if (a >= b) continue;

        if (written++) out += ',';
        out += '[' + String((uint32_t)a) + ',' + String((uint32_t)(b - a)) +
               ',' + String((uint32_t)r.dir) + ']';
    }
    out += ']';
    return written;
}

void logWrite(const uint8_t *d, uint32_t n, uint8_t dir) {
    if (!s_buf || !n) return;
    if (n > s_cap) n = s_cap;      // a single write never spans more than one lap

    uint32_t used  = (uint32_t)(s_head - s_tail);
    uint32_t space = s_cap - used;
    if (n > space) {
        // Recycle the oldest bytes; the newest data is what matters.
        uint64_t tail = s_tail + (n - space);
        if (s_viewFrom < tail) s_viewFrom = tail;
        s_tail = tail;
    }

    noteDir(s_head, n, dir);

    uint32_t off = (uint32_t)s_head & s_mask;
    uint32_t c1  = n < s_cap - off ? n : s_cap - off;
    memcpy(s_buf + off, d, c1);
    if (c1 < n) memcpy(s_buf, d + c1, n - c1);

    s_head += n;
    uint32_t size = (uint32_t)(s_head - s_tail);
    if (size > s_hw) s_hw = size;
}

// Absolute counters never go backwards: clearing only moves the point from
// which the log may be read. A client that asks for an older position is told
// there was a gap instead of being fed bytes that belong to a previous life.
void logClear() {
    if (s_viewFrom < s_head) s_viewFrom = s_head;
    s_hw = 0;
}

uint32_t logCapacity()  { return s_cap; }
uint32_t logFree()      { return s_buf ? s_cap - (uint32_t)(s_head - s_tail) : 0; }
uint32_t logHighWater() { return s_hw; }
uint64_t logWritten()   { return s_head; }
uint64_t logOldest()    { return s_tail; }
uint64_t logVisibleFrom() { return s_viewFrom > s_tail ? s_viewFrom : s_tail; }

uint32_t logCopy(uint64_t fromAbs, uint8_t *out, uint32_t max) {
    if (!s_buf || !max) return 0;

    if (fromAbs < s_tail) fromAbs = s_tail;
    if (fromAbs >= s_head) return 0;

    uint64_t avail = s_head - fromAbs;
    uint32_t n = (uint32_t)(avail < max ? avail : max);
    uint32_t off = (uint32_t)fromAbs & s_mask;
    uint32_t c1  = n < s_cap - off ? n : s_cap - off;

    memcpy(out, s_buf + off, c1);
    if (c1 < n) memcpy(out + c1, s_buf, n - c1);
    return n;
}

void syslog(const char *fmt, ...) {
    char line[200];
    int n = snprintf(line, sizeof(line), "[%s] ", logUptime(millis()).c_str());
    if (n < 0 || n >= (int)sizeof(line)) return;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (m < 0) return;

    n += m;
    if (n > (int)sizeof(line) - 4) n = (int)sizeof(line) - 4;
    line[n++] = '\r';
    line[n++] = '\n';
    line[n]   = '\0';

    if (s_buf) {
        logWrite((const uint8_t *)line, (uint32_t)n);
    } else if (s_preLen + (uint32_t)n + 1 <= sizeof(s_pre)) {
        memcpy(s_pre + s_preLen, line, n);
        s_preLen += n;
    }
}
