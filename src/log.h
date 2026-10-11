#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Ring log
//
// A single byte ring that holds everything received on the UART. Capacity is
// decided once at boot from the free heap (see capLogBytes()) so the buffer
// takes as much of the module's memory as can be spared, and it is never grown
// later on - a request for log space must not be able to fragment the heap out
// from under lwIP.
//
// Positions handed to and returned from the API are absolute counters that only
// ever increase; the physical slot is `absolute & mask`. The web UI stores the
// last counter it saw and asks for "everything after N", which makes the
// transport a pure delta stream even while the ring wraps underneath.
// ---------------------------------------------------------------------------

// Which way a logged byte travelled. The ring stays a plain byte ring - the
// downloadable log has to be exactly what crossed the wire, with nothing added
// - so direction is kept beside it as runs, and the page draws its own markers.
#define LOG_DIR_OUT 0                   // client or browser -> the device
#define LOG_DIR_IN  1                   // the device -> client
#define LOG_DIR_SYS 2                   // the firmware talking about itself

bool     logInit();                    // allocate + replay anything logged pre-boot
void     logWrite(const uint8_t *d, uint32_t n, uint8_t dir = LOG_DIR_SYS);

// Appends "dirs" for the bytes in [from, to) as [[pos, len, dir], ...] and
// returns how many runs were written.
uint32_t logDirRuns(uint64_t from, uint64_t to, String &out);
void     logClear();                   // hide everything recorded so far

uint32_t logCapacity();
uint32_t logFree();
uint32_t logHighWater();               // largest burst held back for back-pressure
uint64_t logWritten();                 // absolute bytes ever recorded
uint64_t logOldest();                  // absolute index of the oldest retained byte
uint64_t logVisibleFrom();             // oldest index a client may still read
uint32_t logCopy(uint64_t fromAbs, uint8_t *out, uint32_t max);

// Timestamped system messages. Lines logged before the ring exists are staged
// in a small static buffer and replayed once it does.
void     syslog(const char *fmt, ...);
#define SYSLOG(...) syslog(__VA_ARGS__)

String   logUptime(uint32_t ms);
