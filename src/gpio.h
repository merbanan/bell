#pragma once

#include <Arduino.h>
#include "bell.h"

// General purpose output pins, separate from the UART.
//
// The pins that could break the module if they were driven - the flash button,
// the pins the UART and the USB console live on, and the ones the SPI flash is
// wired to - are refused outright rather than silently ignored.

// False for pins that are not ours to touch.
bool    gpioUsable(uint8_t pin);
String  gpioUsableList();

// Why a pin is refused, as a short code that is cheap to put on the wire:
// "boot" (sampled at reset), "console" (UART0 / USB), "flash" (wired to the
// SPI chip), "uart" (the swapped UART owns it), "led" (the status light owns
// it). Empty when the pin can be driven.
const char *gpioWhy(uint8_t pin);

// True once this firmware has switched the pin to an output.
bool    gpioDriven(uint8_t pin);

// Applys every saved output at boot.
void    gpioInit();

bool    gpioSet(uint8_t pin, bool value);
bool    gpioToggle(uint8_t pin);
bool    gpioInput(uint8_t pin);            // hand the pin back to the board

// Pins that remember their state across a reboot.
bool    gpioRemember(uint8_t pin, const String &name);
bool    gpioForget(uint8_t pin);

// Every pin 0-16, compactly, for /api/state. Appended rather than returned: the
// state reply is built on a heap this small, and a returned String would mean a
// second full-size copy alive at the same time.
void    gpioAppendState(String &out);

String  gpioJson();
