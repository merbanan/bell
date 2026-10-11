#pragma once

#include <Arduino.h>

enum OtaState { OTA_IDLE, OTA_CHECKING, OTA_READY, OTA_DOWNLOADING, OTA_REBOOTING, OTA_FAILED };

void   otaLoop();                       // drives the state machine, never blocks long

void   otaCheckNow();                   // fetch the manifest and compare versions
void   otaFlashNow();                   // download + write the pending image

OtaState otaState();
bool     otaBusy();
String   otaJson();                     // status + available version for the UI

int      otaCompareVersion(const String &a, const String &b);
