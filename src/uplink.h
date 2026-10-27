#pragma once

#include <Arduino.h>

// The module-to-host link. Once the station is joined and a host address is
// stored, the module connects to the console-server host as a telnet client,
// sends its MAC address so the host can tell modules apart, and then shuttles
// the UART both ways through it.
//
// Its own telnet server keeps running throughout; the uplink is an extra path,
// not a replacement.

void uplinkBegin();                // note when the first attempt may go out
void uplinkLoop();                  // connect / pump in both directions
void uplinkReset();                 // drop the connection (WiFi or host changed)
bool uplinkSend(const uint8_t *d, uint32_t n);   // UART bytes toward the host
bool uplinkAccepting();             // the ring can take another burst right now
void uplinkAppendJson(String &out); // state, appended in place for the web UI
