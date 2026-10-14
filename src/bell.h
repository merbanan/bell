#pragma once

#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "1.1.0"
#endif

#ifndef FW_BOARD
#define FW_BOARD "nodemcuv2"
#endif

// Telnet server ports. Settable from the web interface; these are what a
// module uses until it has been told otherwise. Both overridable at build time.
#ifndef DEFAULT_TELNET_PORT
#define DEFAULT_TELNET_PORT 12345
#endif

// The monitor port. A stored 0 means "one above the read/write port" instead.
#ifndef DEFAULT_TELNET_PORT_RO
#define DEFAULT_TELNET_PORT_RO 12346
#endif

// On a NodeMCU the LED on D4/GPIO2 is wired between 3V3 and the pin, so it
// lights when the pin is driven LOW. Boards that wire it the other way flip
// this at build time; without it "on" would show up as "off".
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif

#ifndef WEB_PORT
#define WEB_PORT 80
#endif

#ifndef MAX_TELNET_CLIENTS
#define MAX_TELNET_CLIENTS 4
#endif

#define CFG_PATH        "/config.json"
#define CFG_VERSION     1
#define CFG_MAGIC       "uartsrv"

// Largest image the device will install over the air.
//
// The ESP8266 core has no second application slot: Update.begin() places the new
// image so that it ENDS at the start of LittleFS and tells the bootloader where
// to find it through an eBoot command. On the stock 4 MB layout that leaves
// roughly 2.5 MB of room between the running sketch and LittleFS, but an image
// written any larger could reach back over the sketch that is running it, so the
// cap is deliberately conservative. Raising it is safe on this layout; it is not
// safe on a different flash size without checking the geometry first.
#ifndef OTA_SLOT_BYTES
#define OTA_SLOT_BYTES 0x100000UL     // 1 MB
#endif

#define MAX_STA_PASS    65
#define MAX_SSID        33
#define MAX_HOSTNAME    32
#define MAX_AP_PASS     65
#define MAX_URL         160

// Ring log tunables. The buffer is allocated once, at boot, from whatever heap
// is left after WiFi has come up: capLogBytes() clamps the result. Both bounds
// must be powers of two - the ring maps a position onto a slot with a mask.
#define LOG_MIN_BYTES   (4u * 1024u)
#define LOG_MAX_BYTES   (32u * 1024u)
#define LOG_HEAP_KEEP   (14u * 1024u)   // heap left untouched for stack/lwIP/TLS
#define LOG_BATCH       512u            // UART bytes pulled per loop() pass

// Stations the provisioning access point will accept. Note that softAP() takes
// this as its fifth argument, after ssid_hidden - passing a boolean "hidden" in
// its place sets the limit to 0 and the access point then refuses every client
// while still advertising itself.
#define MAX_AP_CLIENTS 4

// General purpose outputs whose level is restored at boot.
#define MAX_GPIO_PINS  8
#define GPIO_NAME_MAX  16

// Per telnet client transmit queue. Allocated on connect, freed on disconnect.
#define TC_TX_BYTES     1024u
#define TC_STALL_MS     5000u           // back-pressure grace before dropping a slow peer

// Head-room floors. A read/write session costs about 2.6 kB and a state reply is
// copied twice on its way out, so on a module that is also running an access
// point there is a point where the next allocation fails - and a failed malloc
// on this platform takes the whole module down rather than returning null.
//
// So the web interface turns requests away below its own floor, and a session is
// only admitted when taking it would still leave the web interface that much.
// Session count therefore follows what the heap can actually carry, which on an
// access point is fewer than MAX_TELNET_CLIENTS.
#define WEB_MIN_FREE_HEAP 2560u
// Below this the log cannot be served at all. Above it the page size is worked
// out from what is actually free, because the reply costs about 2.3x the page:
// a fixed size either wastes memory when there is plenty, or fails to fit when
// there is not - which is exactly when somebody is watching the log.
#define LOG_MIN_FREE_HEAP 1400u
#define TC_SLACK_BYTES     256u   // bookkeeping around a session's buffers
