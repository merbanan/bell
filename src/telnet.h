#pragma once

#include <Arduino.h>
#include "bell.h"

void     telnetBegin();
void     telnetLoop();
void     telnetBroadcast(const uint8_t *d, uint32_t n);
void     telnetCloseAll();
void     telnetReset();                  // close the listener entirely
void     telnetApplyConfig();             // push changed stream settings to live clients

// The monitor port always sits one above the working one; 0 means it could not
// be derived and is therefore not listening.
uint16_t telnetReadOnlyPort();

uint8_t  telnetClientCount(bool readOnly);
bool     telnetAccepting();          // false while every peer is backed up
void     telnetAppendJson(String &out);   // client list, appended in place
