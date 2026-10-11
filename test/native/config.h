// Shadows the firmware's config.h for the native test: the log only needs the
// heap sizing hook, and pulling in LittleFS/ArduinoJson/WiFi is pointless here.
#pragma once

#include "Arduino.h"

#define LOG_MIN_BYTES 4096u

uint32_t capLogBytes();
