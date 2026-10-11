#pragma once

#include <Arduino.h>
#include "bell.h"

void     uartSetup();
void     uartReconfigure();                 // re-open the hardware UART from g_cfg
void     uartLoop();                        // UART -> log + every telnet client
bool     uartWrite(const uint8_t *d, uint32_t n);

uint32_t uartRxBytes();
uint32_t uartTxBytes();
uint32_t uartRxErrors();

String   uartSummary();                     // "115200 8N1"
