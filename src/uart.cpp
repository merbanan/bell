#include "uart.h"
#include "config.h"
#include "log.h"
#include "telnet.h"

static uint32_t s_rxBytes  = 0;
static uint32_t s_txBytes  = 0;
static uint32_t s_rxOverruns    = 0;
static uint32_t s_lastShortWrite = 0;
static bool     s_open     = false;

static SerialConfig uartHwConfig() {
    switch (g_cfg.bits) {
        case 5:
            switch (g_cfg.parity) {
                case 'E': return g_cfg.stops == 2 ? SERIAL_5E2 : SERIAL_5E1;
                case 'O': return g_cfg.stops == 2 ? SERIAL_5O2 : SERIAL_5O1;
                default:   return g_cfg.stops == 2 ? SERIAL_5N2 : SERIAL_5N1;
            }
        case 6:
            switch (g_cfg.parity) {
                case 'E': return g_cfg.stops == 2 ? SERIAL_6E2 : SERIAL_6E1;
                case 'O': return g_cfg.stops == 2 ? SERIAL_6O2 : SERIAL_6O1;
                default:   return g_cfg.stops == 2 ? SERIAL_6N2 : SERIAL_6N1;
            }
        case 7:
            switch (g_cfg.parity) {
                case 'E': return g_cfg.stops == 2 ? SERIAL_7E2 : SERIAL_7E1;
                case 'O': return g_cfg.stops == 2 ? SERIAL_7O2 : SERIAL_7O1;
                default:   return g_cfg.stops == 2 ? SERIAL_7N2 : SERIAL_7N1;
            }
        default:
            switch (g_cfg.parity) {
                case 'E': return g_cfg.stops == 2 ? SERIAL_8E2 : SERIAL_8E1;
                case 'O': return g_cfg.stops == 2 ? SERIAL_8O2 : SERIAL_8O1;
                default:   return g_cfg.stops == 2 ? SERIAL_8N2 : SERIAL_8N1;
            }
    }
}

void uartSetup() {
    Serial.setRxBufferSize(1024);

    // Order matters. begin() nulls the UART first, so a swap() in front of it
    // quietly does nothing; and begin() only understands TX pins 1 and 2, so
    // GPIO15 can only be reached through swap() afterwards.
    Serial.begin(g_cfg.baud, uartHwConfig(), SERIAL_FULL);
    if (g_cfg.swapPins && !Serial.swap())
        SYSLOG("[uart] could not move to GPIO15/13, staying on GPIO1/3");
    s_open = true;

    SYSLOG("[uart] %s on %s (rx buffer 1024)",
           uartSummary().c_str(),
           g_cfg.swapPins ? "TX=GPIO15 RX=GPIO13" : "TX=GPIO1 RX=GPIO3");
}

void uartReconfigure() {
    Serial.end();
    s_open = false;
    delay(20);
    uartSetup();
}

void uartLoop() {
    // Static: loop() runs on a 4 kB stack on this chip.
    static uint8_t buf[LOG_BATCH];
    uint32_t budget = LOG_BATCH;

    // Do not pull more off the wire while a telnet peer is backed up; the
    // hardware FIFO is only 1KB and the bridge cannot afford to spin.
    if (!telnetAccepting()) return;

    while (budget && Serial.available() > 0) {
        uint32_t want = (uint32_t)Serial.available();
        if (want > budget) want = budget;

        size_t n = Serial.readBytes(buf, want);
        if (n == 0) break;

        s_rxBytes += n;

        if (Serial.hasOverrun()) {
            s_rxOverruns++;
            SYSLOG("[uart] %u overrun(s): the receiver could not keep up",
                   (unsigned)s_rxOverruns);
        }

        // The log takes the data first: whatever happens to the telnet peers
        // afterwards, the byte stream is still retrievable from the web UI.
        logWrite(buf, (uint32_t)n, LOG_DIR_IN);      // the device spoke

        telnetBroadcast(buf, (uint32_t)n);
        budget -= n;
    }
}

bool uartWrite(const uint8_t *d, uint32_t n) {
    if (!s_open || !n) return false;

    size_t w = Serial.write(d, n);
    s_txBytes += w;

    if (w < n) {
        // The hardware FIFO could not take everything; the tail is dropped.
        // Logged once per burst, because a client that keeps the line busy
        // would otherwise fill the log with this and evict the payload.
        if (n - w != s_lastShortWrite) {
            s_lastShortWrite = n - w;
            SYSLOG("[uart] transmitter is not keeping up, %u byte(s) dropped",
                   (unsigned)(n - w));
        }
        return false;
    }
    s_lastShortWrite = 0;
    if (g_cfg.logTx) logWrite(d, w, LOG_DIR_OUT);   // a client typed this
    return true;
}

uint32_t uartRxBytes()  { return s_rxBytes; }
uint32_t uartTxBytes()  { return s_txBytes; }
uint32_t uartRxErrors() { return s_rxOverruns; }


String uartSummary() {
    char p[3] = {g_cfg.parity, 0, 0};
    String s = String(g_cfg.baud) + " " + String(g_cfg.bits) + p + String(g_cfg.stops);
    return s;
}
