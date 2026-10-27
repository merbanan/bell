/*
 * bell - ESP8266 UART-over-Telnet server
 *
 * Bridges the hardware UART to TCP port 23 using the telnet protocol, and
 * exposes a web interface for configuration, for reading the UART log and for
 * updating the firmware.
 *
 * UART0 carries the payload, so this firmware never prints anything to Serial:
 * diagnostics go to the log ring, which the web interface shows.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include "bell.h"
#include "config.h"
#include "log.h"
#include "netmgr.h"
#include "ota.h"
#include "telnet.h"
#include "uart.h"
#include "uplink.h"
#include "gpio.h"
#include "webui.h"

#ifdef LED_BUILTIN
static uint8_t s_led = LED_BUILTIN;
#endif

// Brightness, 0 = dark, 255 = full.
#define LED_OFF 0
#define LED_ON  255

// The ESP8266 has no PWM peripheral - analogWrite() is software, driving the
// SDK's shared timer, and that timer also carries the system's own timing.
// Borrowing it to dim a cosmetic indicator on a device that is bridging a
// serial line is not a trade worth making, so the square wave is produced in
// loop() instead: ten steps of half a millisecond, so a tenth of full is exactly
// one step on - 200 Hz, fast enough not to read as a flicker, and it costs a
// pin write every 500 us and nothing else.
#define LED_PWM_STEPS 10
#define LED_PWM_STEP_US 500

// One step out of ten. The steady "ready" indicator sits here: bright enough to
// find across a room, dim enough to sit on a desk all day.
#define LED_DIM 26

static uint8_t  s_bright  = LED_OFF;   // what has been asked for
static bool     s_drive   = false;     // what is actually on the pin
static uint8_t  s_pwmStep = 0;
static uint32_t s_pwmNext = 0;
static uint32_t s_ledNext = 0;
static bool     s_ready   = false;     // setup() finished
static uint32_t s_readyAt = 0;         // when it became ready
// Last setting seen by the LED task, so a change starts the countdown again
// rather than judging the new timeout against the time already served.
static uint16_t s_ledSecSeen = 0xFFFF;
static bool     s_ledOnSeen  = true;

static void ledDrive(bool on) {
#ifdef LED_BUILTIN
    if (on == s_drive) return;
    s_drive = on;
    digitalWrite(s_led, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
#endif
}

static void led(uint8_t brightness) { s_bright = brightness; }

static void serviceLed() {
    if (s_bright <= LED_OFF || s_bright >= LED_ON) {
        ledDrive(s_bright != LED_OFF);
        return;                              // nothing to modulate
    }

    uint32_t now = micros();
    if ((int32_t)(now - s_pwmNext) < 0) return;
    s_pwmNext = now + LED_PWM_STEP_US;
    s_pwmStep = (uint8_t)((s_pwmStep + 1) % LED_PWM_STEPS);

    unsigned onSteps = ((unsigned)s_bright * LED_PWM_STEPS + 127u) / 255u;
    ledDrive(s_pwmStep < onSteps);
}

// Factory reset announcement: two blinks a second for five seconds, so it is
// obvious the gesture was understood before the settings disappear.
#define RESET_BLINK_HALF_MS  250
#define RESET_BLINK_CYCLES   10
static uint8_t  s_blinkHalves = 0;
static uint32_t s_blinkAt     = 0;

static void finishFactoryReset() {
    if (cfgErase())
        SYSLOG("[sys] settings erased, restarting into access point mode");
    else
        SYSLOG("[sys] no settings to erase, restarting anyway");
    delay(100);
    ESP.restart();
}

// Half-period ticks: the LED is lit while an even number of halves is left, so
// the first one is already on and the last one to go dark is the last blink.
static void serviceFactoryReset(uint32_t now) {
    if ((int32_t)(now - s_blinkAt) < 0) return;
    s_blinkAt = now + RESET_BLINK_HALF_MS;
    if (--s_blinkHalves == 0) {
        led(false);
        finishFactoryReset();
        return;
    }
    led((s_blinkHalves % 2) == 0);
}

// The whole life of the indicator: blink while the module is starting, steady
// once setup() has finished, and the factory-reset blink in between when that
// gesture is being honoured.
static void ledTask() {
    uint32_t now = millis();

    if (netFactoryResetDue()) {
        s_blinkHalves = RESET_BLINK_CYCLES * 2;
        s_blinkAt = now;
        led(LED_ON);
    }
    if (s_blinkHalves) {          // the reset owns the LED until it is done
        serviceFactoryReset(now);
        return;
    }

    if (s_ledSecSeen != g_cfg.ledSeconds || s_ledOnSeen != g_cfg.statusLed) {
        s_ledSecSeen = g_cfg.ledSeconds;
        s_ledOnSeen  = g_cfg.statusLed;
        s_readyAt    = now;
    }

    // Off means off: no boot blink, no steady light. The reset announcement
    // above still runs, because that is confirmation of a destructive gesture
    // and is not a status light.
    if (!g_cfg.statusLed) {
        led(LED_OFF);
        serviceLed();
        return;
    }

    if (s_ready) {
        // Dim while it is worth showing, then get out of the way.
        bool stillWanted = g_cfg.ledSeconds == 0 ||
                           (now - s_readyAt) < (uint32_t)g_cfg.ledSeconds * 1000u;
        led(stillWanted ? LED_DIM : LED_OFF);
        serviceLed();
        return;
    }

    // Booting: five blinks a second.
    if ((int32_t)(now - s_ledNext) >= 0) {
        s_ledNext = now + 100;
        led(s_bright ? LED_OFF : LED_ON);
    }
    serviceLed();
}

void setup() {
#ifdef LED_BUILTIN
    pinMode(s_led, OUTPUT);
    ledDrive(false);
#endif

    // Calibration data is kept in RAM: the radio does not need to write it back
    // to flash on every join, which would slowly wear the module out.
    WiFi.persistent(false);

    LittleFS.begin();
    cfgLoad();

    // Pins before radio. Setting the outputs is the one thing that can usefully
    // happen before the network, and the radio is nearly all of the boot: the
    // access point alone is about 900 ms of it. Anything this logs goes into
    // the staged pre-buffer and is replayed once the ring exists, so moving it
    // here costs nothing.
    gpioInit();

    // Radio first, log second: sizing the ring from the heap that is left once
    // lwIP has taken its share is the only way to be sure that what remains is
    // still enough for a working connection.
    netBegin();

    logInit();
    if (g_cfg.clearOnBoot) {
        logClear();
        SYSLOG("[log] cleared at boot");
    }

    uartSetup();
    webBegin();
    uplinkBegin();

    SYSLOG("[sys] %s ready - telnet on port %d, web on port %d",
           FW_VERSION, (int)g_cfg.telnetPort, (int)WEB_PORT);

    if (g_cfg.otaAuto) otaCheckNow();

    // Everything is up: from here the LED just says so, for as long as it is
    // configured to be worth saying.
    s_readyAt = millis();
    s_ready = true;
}

void loop() {
    netLoop();
    uplinkLoop();
    uartLoop();
    telnetLoop();
    webLoop();
    otaLoop();
    ledTask();
}
