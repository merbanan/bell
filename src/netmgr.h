#pragma once

#include <Arduino.h>

enum NetMode { NET_AP_ONLY, NET_STA, NET_STA_AP };

void   netBegin();                 // pick a mode and start it
void   netLoop();                  // connection supervision / AP fallback / flash button
void   netApply();                 // re-apply the stored configuration

NetMode netMode();
bool    netConnected();
String  netModeName();

// Scanning cannot be done inside the request that asks for it: the radio has
// to be put into station mode as well, and on this chip that restarts the
// access point - dropping the very client that asked. So a scan is started
// here, runs from loop(), and the page polls for the result.
//
// Returns false when a scan could not be started.
bool    netScanStart();

// -1 while one is still running, -2 on failure, otherwise the number found.
int     netScanStatus();

// The results live in a fixed buffer the device fills in; handing them out as a
// pointer keeps the reply from costing a second copy of the heap.
const char *netScanJson(size_t &len, bool &truncated);
void    netForget();               // wipe station credentials, go back to AP mode

// Consumes the "flash button held for 5 s" event. Read it once per loop().
bool    netFactoryResetDue();
