#include "gpio.h"
#include "config.h"
#include "log.h"

#include <ESP8266WiFi.h>

bool gpioUsable(uint8_t pin) {
    if (pin > 16) return false;

    switch (pin) {
        case 0:                                 // flash button, sampled at reset
        case 1:                                 // UART0 TX, and the USB console
        case 3:                                 // UART0 RX, ditto
        case 6: case 7: case 8:                 // wired to the SPI flash
        case 9: case 10: case 11:               // ditto; the SDK reserves them
            return false;

        case 13: case 15:                       // the UART's alternative pins
            return !g_cfg.swapPins;

        case 2:
            // On a NodeMCU this is the on-board LED, which the firmware already
            // drives as its status indicator. Two owners for one pin means one
            // of them lies, so it is only free once the status LED is off.
            return !g_cfg.statusLed;

        default:                                // 4, 5, 12, 14, 16
            return true;
    }
}

// Reading or re-pointing these is not merely unwise: on the ESP8266 they are the
// data and command lines of the flash chip this firmware is running out of.
static bool pinSafeToTouch(uint8_t pin) {
    switch (pin) {
        case 6: case 7: case 8:
        case 9: case 10: case 11:
            return false;
        default:
            return pin <= 16;
    }
}

const char *gpioWhy(uint8_t pin) {
    switch (pin) {
        case 0:  return "boot";                  // sampled by the boot loader
        case 1:
        case 3:  return "console";               // UART0, and the USB serial
        case 6: case 7: case 8:
        case 9: case 10: case 11:                return "flash";
        case 13:
        case 15:  return g_cfg.swapPins ? "uart" : "";
        case 2:   return g_cfg.statusLed ? "led" : "";
        default:   return "";
    }
}

// One bit per pin the firmware has turned into an output. pinMode() cannot be
// read back, so this is the only way to tell "high because we drive it" from
// "high because the board next door does".
static uint32_t s_drive;

bool gpioDriven(uint8_t pin) {
    return pin <= 16 && ((s_drive >> pin) & 1u);
}


String gpioUsableList() {
    String out;
    for (uint8_t pin = 0; pin <= 16; pin++) {
        if (!gpioUsable(pin)) continue;
        if (out.length()) out += ',';
        out += String(pin);
    }
    return out;
}

static GpioEntry *find(uint8_t pin) {
    for (uint8_t i = 0; i < g_cfg.gpioCount; i++)
        if (g_cfg.gpio[i].pin == pin) return &g_cfg.gpio[i];
    return nullptr;
}

static bool drive(uint8_t pin, bool value) {
    digitalWrite(pin, value ? HIGH : LOW);
    s_drive |= 1UL << pin;
    return true;
}

void gpioInit() {
    for (uint8_t i = 0; i < g_cfg.gpioCount; i++) {
        const GpioEntry &e = g_cfg.gpio[i];
        if (!gpioUsable(e.pin)) {
            SYSLOG("[gpio] GPIO%u (%s) is not available any more, skipping",
                   (unsigned)e.pin, e.name);
            continue;
        }
        pinMode(e.pin, OUTPUT);
        digitalWrite(e.pin, e.value ? HIGH : LOW);
        s_drive |= 1UL << e.pin;
    }
    if (g_cfg.gpioCount)
        SYSLOG("[gpio] %u output(s) restored", (unsigned)g_cfg.gpioCount);
}

bool gpioSet(uint8_t pin, bool value) {
    if (!gpioUsable(pin)) return false;

    if (GpioEntry *e = find(pin)) {
        if (e->value == value) return true;      // nothing changed
        e->value = value;
        cfgSave();
    }
    pinMode(pin, OUTPUT);
    return drive(pin, value);
}

bool gpioToggle(uint8_t pin) {
    if (!gpioUsable(pin)) return false;

    // Remembered pins know their own state; the rest are simply read back.
    GpioEntry *e = find(pin);
    bool now = e ? (e->value != 0) : (digitalRead(pin) == LOW);
    return gpioSet(pin, now);
}

bool gpioInput(uint8_t pin) {
    if (pin > 16 || !pinSafeToTouch(pin)) return false;

    // "Restored at boot as an output" and "left for the board" cannot both be
    // true, so releasing a pin also drops it from the saved list.
    if (find(pin) && !gpioForget(pin)) return false;

    pinMode(pin, INPUT);
    s_drive &= ~(1UL << pin);
    SYSLOG("[gpio] GPIO%u released back to an input", (unsigned)pin);
    return true;
}

bool gpioRemember(uint8_t pin, const String &name) {
    if (!gpioUsable(pin)) return false;

    if (GpioEntry *e = find(pin)) {
        strlcpy(e->name, name.c_str(), sizeof(e->name));
        return cfgSave();
    }
    if (g_cfg.gpioCount >= MAX_GPIO_PINS) return false;

    GpioEntry &e = g_cfg.gpio[g_cfg.gpioCount++];
    e.pin   = pin;
    e.value = (digitalRead(pin) == HIGH);
    strlcpy(e.name, name.c_str(), sizeof(e.name));

    pinMode(pin, OUTPUT);
    s_drive |= 1UL << pin;
    return cfgSave();
}

bool gpioForget(uint8_t pin) {
    GpioEntry *e = find(pin);
    if (!e) return false;

    uint8_t i = (uint8_t)(e - g_cfg.gpio);
    for (uint8_t k = (uint8_t)(i + 1); k < g_cfg.gpioCount; k++)
        g_cfg.gpio[k - 1] = g_cfg.gpio[k];
    g_cfg.gpioCount--;

    SYSLOG("[gpio] GPIO%u is no longer restored at boot", (unsigned)pin);
    return cfgSave();
}

// All 17 pins, in pin order, as [usable, output, level, restored]. Positional
// on purpose: this sits in /api/state, which the page polls every few seconds,
// and a heap that is down to a couple of kilobytes once sessions are connected
// cannot spare an object per pin. Names ride alongside in gpioNames, and only
// when somebody has actually given one.
void gpioAppendState(String &out) {
    out += '[';
    for (uint8_t pin = 0; pin <= 16; pin++) {
        if (pin) out += ',';
        out += '[';
        out += String(gpioUsable(pin) ? 1 : 0);
        out += ',';
        out += String(gpioDriven(pin) ? 1 : 0);
        out += ',';
        out += String(pinSafeToTouch(pin) && digitalRead(pin) ? 1 : 0);
        out += ',';
        out += String(find(pin) ? 1 : 0);
        out += ']';
    }
    out += "]";

    out += ",\"gpioNames\":{";
    bool first = true;
    for (uint8_t pin = 0; pin <= 16; pin++) {
        GpioEntry *e = find(pin);
        if (!e || !e->name[0]) continue;
        if (!first) out += ',';
        first = false;
        out += "\"" + String((uint32_t)pin) + "\":\"" +
               jsonEscape(String(e->name)) + "\"";
    }
    out += "}";
}

String gpioJson() {
    String out = "{\"pins\":[";
    for (uint8_t i = 0; i < g_cfg.gpioCount; i++) {
        const GpioEntry &e = g_cfg.gpio[i];
        if (i) out += ',';
        out += "{\"pin\":" + String((uint32_t)e.pin);
        out += ",\"name\":\"" + jsonEscape(String(e.name)) + "\"";
        out += ",\"value\":" + String(e.value ? 1 : 0);
        out += ",\"driving\":" + String(gpioUsable(e.pin) ? "true" : "false");
        out += ",\"level\":" + String(digitalRead(e.pin) ? 1 : 0) + "}";
    }
    out += "],\"all\":[";
    for (uint8_t pin = 0; pin <= 16; pin++) {
        if (pin) out += ',';
        GpioEntry *e = find(pin);

        out += "{\"p\":" + String((uint32_t)pin);
        out += ",\"u\":" + String(gpioUsable(pin) ? 1 : 0);
        out += ",\"w\":\"" + String(gpioWhy(pin)) + "\"";
        out += ",\"d\":" + String(gpioDriven(pin) ? 1 : 0);
        out += ",\"l\":" + String(pinSafeToTouch(pin) && digitalRead(pin) ? 1 : 0);
        out += ",\"r\":" + String(e ? 1 : 0);
        if (e && e->name[0]) out += ",\"n\":\"" + jsonEscape(String(e->name)) + "\"";
        out += "}";
    }
    out += "],\"available\":[";
    for (uint8_t pin = 0; pin <= 16; pin++) {
        if (!gpioUsable(pin)) continue;
        if (out[out.length() - 1] != '[') out += ',';
        out += String(pin);
    }
    out += "],\"max\":" + String((uint32_t)MAX_GPIO_PINS) + "}";
    return out;
}
