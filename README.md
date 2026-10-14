# bell

### UART over Telnet for the ESP8266

[![platform](https://img.shields.io/badge/platform-ESP8266-8A2BE2?style=flat-square&logo=espressif&logoColor=white)](https://espressif.com)
[![firmware](https://img.shields.io/badge/firmware-1.0.0-2DA44E?style=flat-square)](https://github.com/merbanan/bell/releases)
[![license](https://img.shields.io/badge/license-see%20repo-8A2BE2?style=flat-square)](LICENSE)

Puts a serial port on the network. A device plugged into the ESP8266's UART shows
up as a telnet port you can `nc` to from anywhere, and a web interface on the
module itself handles the configuration, shows you what has crossed the line,
and updates the firmware.

Everything runs on the module. No cloud, no daemon, nothing to install on the
host.

```
   ┌──────────────┐   UART, GPIO15 TX / GPIO13 RX   ┌─────────┐
   │ your device  │◄───────────────────────────────►│         │◄──► telnet  :12345  read/write
   └──────────────┘                                 │  bell   │◄──► telnet  :12346  read only
                                                    │ ESP8266 │◄──► web    :80
                                                    └─────────┘
```

Two ports, because watching and driving are different jobs: anything a client
sends to the read-only port is read off the socket and thrown away — it never
reaches the UART.

## Why it is built this way

| | |
|---|---|
| **A serial line, not a chat** | The telnet negotiation is [ser2net's](https://github.com/cminyard/ser2net), including `WILL ECHO` so the client stops echoing locally, and IAC doubling on output so a `0xFF` survives the trip. |
| **Transparent** | A byte a client sends reaches the device, and a byte the device sends reaches every client. Nothing is dropped, translated or second-guessed. |
| **Survives being flashed wrong** | The ESP8266 has one application slot, so updates are capped conservatively and refuse anything that would not fit. |
| **Honest when it cannot cope** | Memory is tight once sessions are open. Rather than let a failed allocation restart the module, the paths that can be asked for at the worst moment turn requests away and say so. |
| **No diagnostics on the payload** | The firmware never prints to `Serial` — that pin pair is the data. Everything it has to say goes in the log ring. |

## What it does

* **Telnet server**, port 12345 by default and changeable from the Network page
  without a reboot, with a second read-only port above it and up to four
  simultaneous sessions.
* **Web interface** at port 80: status, UART log, UART setup, network, GPIO and
  system pages.
* **Log ring** sized from the heap that is actually free at boot, up to 32 kB,
  with a separate console view for the firmware's own messages.
* **Send box** under the UART log, so you can talk to the device from a browser
  — including a bare line feed on its own, and a Ctrl-C.
* **GPIO control** for every pin the hardware will let you have, with a plain
  reason for each one it refuses.
* **Access point mode** whenever there is nothing to connect to, so a freshly
  flashed module is configurable without any other equipment.
* **Network scan** from the browser, so you do not have to type an SSID you
  cannot see.
* **Firmware updates over the air**, from a version manifest or by uploading a
  `.bin` you already have.

## Hardware

The UART is on **GPIO15 (TX) and GPIO13 (RX)** by default.

| Signal | GPIO | Direction     |
| ------ | ---- | ------------- |
| UART TX | 15   | module → device |
| UART RX | 13   | module ← device |

Those are deliberate. GPIO1/GPIO3 are what the USB-serial chip is wired to on
almost every NodeMCU, so using them would put the payload on the same wires as
the debug console and the ROM loader's boot messages — you could not read a log
or flash the board while the bridge was running. GPIO13/GPIO15 leave the console
alone. The USB-serial pins stay available from **UART setup** if you would
rather bridge something through the USB port itself.

Two things to know about GPIO15: it is sampled at reset, and it must be **low**
then or the module will not boot normally (an unmodified dev board pulls it
down). It is free once running, but a device that holds it high during power-up
will stop the module from starting.

GPIO0 (the FLASH button) is watched after boot: hold it for a second and a half
and the setup access point comes up, whatever is stored in the configuration.
Holding it down *while resetting* will not do this — that combination is what
puts the chip into its serial bootloader, and then this firmware never runs.

Note that opening the USB serial port resets the module: the DTR/RTS
auto-reset circuit on these boards pulls EN low. That is hardware behaviour, and
it is another reason the payload is not on those pins.

## Build and flash

```sh
pio run -e nodemcuv2              # or: pio run
pio run -e nodemcuv2 -t upload
pio device monitor               # the UART is the payload, so expect garbage
```

Target any ESP8266 with 4 MB of flash; the default board is `nodemcuv2`, which
uses the core's stock layout:

| Offset | Contents |
| --- | --- |
| `0x00000` | system: rf calibration, wifi parameters, nvs |
| `0x01000` | sketch — about 1019 kB, this firmware uses 520 kB of it |
| `0x30000` | LittleFS — 1000 kB, holds `config.json` |

There is no second application slot, and none is needed: the ESP8266 core
writes an over-the-air image so that it *ends* where LittleFS begins and tells
the bootloader where to find it through an eBoot command. `OTA_SLOT_BYTES` caps
how large such an image may be at 1 MB — well under the ~2.5 MB actually
available — so an update can never reach back over the sketch running it.

## First run

An unconfigured module comes up as an access point:

| | |
| --- | --- |
| SSID | `uart-server-XXXX` (the last four hex digits of the chip id) |
| Password | `uartserver` |
| Address | `192.168.4.1` |

Most phones and laptops notice this is a captive portal and open the page on
their own; if not, browse to `http://192.168.4.1/`.

Go to **Network**, press **Scan for networks**, click yours, fill in the
password and save, then reboot from the **System** tab.

Scanning takes a few seconds and, on an unconfigured module, restarts the
access point on the way — the radio has to become a station to listen. That
drops the page's own connection, so the button stays disabled and polls until
the module answers again; nothing needs to be retyped. If the join fails the
module falls back to the access point after the timeout you configured, so you
can always get back in.

To get back to the setup page later, reset the module and then hold FLASH for a
second and a half; the access point comes up and the module stays reachable on
both interfaces.

Once it is joined, reach it at `http://<ip>/` or at `http://<hostname>.local/`
over mDNS.

## The log

Everything received on the UART lands in a ring buffer. The browser holds the
last position it has seen and asks for everything after it, so what crosses the
wire is only what is new — the same bytes are never sent twice.

By default the log records one direction: bytes arriving **on the UART**. The
telnet client's own keystrokes are not stored, because a password typed into a
telnet session would otherwise sit in the log. Tick *also record transmitted
bytes* on the **UART setup** tab to capture what the bridge writes to the UART
as well — which is what you want when there is nothing wired to the pins and
you are checking that what you type actually arrives:

```sh
curl -X POST -d 'logTx=true' http://192.168.4.1/api/uart
```

### The two panes

One ring holds both streams, and the page splits them:

* **Status** — the firmware's own messages (`[wifi]`, `[telnet]`, `[gpio]`, …)
  and the device status table.
* **UART log** — the bytes that crossed the line, with the firmware's messages
  left out, plus a send box and the counters for the line and its sessions.

Each line in the UART log is coloured by the way it went: blue for text sent to
the device, green for text received back from it, and plain for anything the
device can no longer vouch for. The colour belongs to the page alone — it is
drawn from a table of direction runs the device sends alongside the bytes, so
the `.txt` and `.bin` downloads are exactly what crossed the wire, with nothing
added.

The split is done in the browser against the same fetched bytes, so nothing is
duplicated on the device. Console lines are recognised by their
`[hh:mm:ss.mmm] [tag]` prefix; a UART payload that happens to look like one
would be shown on the wrong page.

* **Ascii** and **hex** views; hex shows the absolute byte offset.
* **Download** gives you the window as `.txt` (non-printables shown as `.`) or
  as the raw `.bin`.
* **Clear** hides everything recorded so far without resetting the counters; the
  page marks the gap instead of splicing two unrelated streams together.

The buffer is allocated once, after WiFi is up, from whatever heap is left over
after a 14 kB reserve — rounded down to a power of two, clamped to 4…32 kB. The
page header shows the size it actually got. A module with more free RAM gets a
larger log; taking more of it leaves less headroom for HTTPS, which is why the
reserve exists.

When the ring wraps, the oldest bytes are dropped and the page says how many.

## Telnet

There are **two** ports. The one you configure is read/write; the number above
it is a monitor that can only listen.

```sh
telnet <ip> 12345       # or: nc <ip> 12345   -- read/write
telnet <ip> 12346       # read only
```

Both ports live in the configuration (**Network → Telnet server**). The
read/write port defaults to 12345 and the read-only port to 12346; changing
either restarts both listeners on the spot and drops open sessions. Setting the
read-only port to **0** makes it follow the read/write port, one above it. If
the number would fall outside the usable range, clash with the read/write port,
or land on the web interface's port, the monitor does not start and says so in
the log.

Anything a monitor sends is read off the socket and discarded — it never reaches
the UART, is never echoed, and is never put in the log. The **UART log** page
counts those discarded bytes so you can see that a client tried to type. A
monitor gets no option negotiation on connect either, since it has no keyboard
to configure.

Both ports share one pool of session slots, and a session is only admitted if it
still leaves the web interface enough heap to answer. On a module running an
access point that is fewer than the four slots the header advertises; a client
that does not fit is told so and turned away rather than allowed to take the
module down with it. The **UART log** page shows what is actually connected.

The negotiation follows **ser2net**, the reference for this job. On connect the
bridge sends the same sequence it does:

```
IAC WILL SUPPRESS-GO-AHEAD
IAC WILL ECHO
IAC DONT ECHO
IAC DO   BINARY
```

`WILL ECHO` is the part that makes a session feel like a serial line rather than
a chat. A telnet client echoes what you type locally by default, so every
character appears twice; taking echo over tells the terminal to stop, and the
only thing that comes back is what the device actually sent. `DO ECHO` is
therefore answered `WILL` rather than `WONT`.

One more thing ser2net does that is easy to miss, and without which the path is
not byte-transparent at all: **a 0xFF in the payload is doubled on the way
out**. BINARY is about the eighth bit, not about IAC, so this is required in
both modes. Send a bare 0xFF and the client reads it as the start of a command
and swallows the byte after it. Every one of the 256 byte values survives the
round trip; `test/native` checks that, along with the negotiation. Two optional conveniences live on the
UART page for the many devices that want them:

* **Echo input back to the client** — for poking a REPL that does not echo.
* **Turn a typed LF into CR+LF** — for devices that only accept CR.

## LED

Blink while the module is starting, then a steady **one-tenth brightness** once
`setup()` has finished, then **off** thirty seconds later. Thirty seconds is long
enough to notice it settled and short enough that it is not a night light.

* The **seconds it stays lit** is a number box next to the checkbox. Default 30,
  and `0` leaves it on indefinitely for anyone who prefers that.
* Changing the number **restarts the countdown**, so raising it at 29 seconds
  remaining does not immediately switch the light off.
* Unticking **status LED** turns it off entirely — no boot blink, nothing.

The factory-reset blink still runs at full brightness for five seconds before the
restart even when the LED is switched off. That one is not a status light, it is
confirmation that a destructive gesture was understood, and withholding it would
leave you guessing whether the settings were erased.

Two details worth knowing:

* On a NodeMCU the LED on D4/GPIO2 is wired between 3V3 and the pin, so it lights
  when the pin is driven **low**. The code accounts for that (`LED_ACTIVE_LOW`
  in `bell.h`), otherwise "on" would show up as "off".
* **The ESP8266 has no PWM peripheral.** `analogWrite()` exists but is software,
  built on the SDK's shared timer — the same one the system uses for its own
  scheduling — and the core defaults it to 1 kHz. Taking that timer hostage to
  dim an indicator on a device that is bridging a 115200-baud serial line is a bad
  trade, so the square wave is generated in `loop()` instead: ten steps of half a
  millisecond — so a tenth of full is exactly one step — at 200 Hz, costing one
  pin write every 500 us and no timer or interrupt at all.

GPIO2 is that same LED, which is why the GPIO page will not hand it out while the
status LED is on. Timing out is **not** the same as switching it off: the pin stays
driven high rather than released, so it shows up as still owned until the checkbox
is actually unticked.

## GPIO

The **GPIO** tab lists all seventeen pins whether or not they can be driven, and
says why for the ones that cannot. A pin that is free can be pushed high or low,
and released back to an input when the board should take it over. The same
things are available from the API:

```sh
curl -X POST -d 'pin=5&value=1' http://192.168.4.1/api/gpio/set
curl -X POST -d 'pin=5'          http://192.168.4.1/api/gpio/toggle
curl -X POST -d 'pin=5'          http://192.168.4.1/api/gpio/input
curl -X POST -d 'pin=5&name=relay' http://192.168.4.1/api/gpio/remember
curl                            http://192.168.4.1/api/gpio
```

`GET /api/gpio` is the full answer: `all` has one record per pin with a `w` code
saying why it is refused, and `available` is the short list of pins that are
free right now. `POST /api/gpio/set` switches the pin to an output and sets its
level in one step.

The refused pins are refused for good reasons, and the API says which applies:

| Code      | Pins      | Why                                             |
| --------- | --------- | ----------------------------------------------- |
| `flash`   | 6…11      | wired to the SPI flash; driving them can corrupt the firmware |
| `boot`    | 0         | sampled at reset; pulling it low starts the bootloader |
| `console` | 1, 3      | UART0, and the USB serial console                |
| `uart`    | 13, 15    | only while the UART is swapped onto that pair    |
| `led`     | 2         | only while the status LED is switched on         |

Pins 13 and 15 free up as soon as the UART is moved back to GPIO1/3, and GPIO2
as soon as the status LED is switched off — both show up without a reboot.

That is done at the very top of `setup()`, before the radio is touched, so the
pins are live **~240 ms** from power-stable — 90 ms after the SDK takes over, plus
the ~150 ms of ROM before it — instead of waiting the best part of a second for
WiFi to come up first. Until that moment the pins are floating inputs, so anything
downstream that latches on its own reset wants a pull resistor on them.

Remembering is opt-in per pin, and releasing a pin with `/api/gpio/input` drops it
from the list too. One level is all a pin can hold, so those two cannot both apply.

A brief dip in the power rail is a different case again: if a capacitor holds it
above brownout the chip never resets, so the outputs simply keep their level and
nothing needs restoring.

Pins marked *remembered* are put back to their last level every time the module
boots; the rest are driven until something resets them. Releasing a pin with
`/api/gpio/input` also drops it from that list, since the two cannot both hold.

**Refused, deliberately:** GPIO0 (the flash button, sampled at reset), GPIO1 and
GPIO3 (the UART and, on a NodeMCU, the USB console), GPIO6–GPIO11 (wired to the
SPI flash), GPIO13/GPIO15 while the UART is using them, and **GPIO2** while the
status LED is on — that is the NodeMCU's own LED, and two owners for one pin
means one of them lies. That normally leaves **4, 5, 12, 14 and 16**; untick
**status LED** in the UART setup page to get GPIO2 as well.

Two cautions the firmware cannot check for you, because they depend on how your
board is wired: GPIO16 resets the chip on boards that tie it to EN/RST, and
GPIO2 must be low at reset to select a 4 MB flash. Check with a meter before
relying on either for something you cannot reach.

Levels are active-high. A relay or an opto-isolator usually wants the opposite
of whatever you would guess, so check it with a meter before trusting it.

**UART log** is where the live numbers live: bytes and throughput in each
direction, an activity lamp, what every telnet session is moving, receiver
overruns, and how much of the log ring is in use. Rates are worked out in the
browser from the byte counters the device keeps, so they read sensibly whatever
the polling interval.

If a client cannot drain fast enough, the bridge stops reading the UART rather
than throwing data away, and hangs up on that client if it stays stuck for five
seconds. The log still receives the bytes in the meantime.

## Firmware updates

Two ways in, both on the **System & firmware** page:

1. **Ask a manifest for a newer build.** Out of the box this points at

   ```
   https://raw.githubusercontent.com/merbanan/bell/master/firmware.json
   ```

   which is the `firmware.json` in this repository. Publish a release and the
   device sees it — a tag, a `.bin` uploaded to it, and the manifest updated is
   the whole job:

   ```sh
   pio run -e nodemcuv2
   gh release create v1.1.0 .pio/build/nodemcuv2/firmware.bin --latest
   # then bump "version" and "url" in firmware.json and push
   ```

   The manifest is JSON like this:

   ```json
   {
     "version": "1.0.0",
     "url": "https://github.com/merbanan/bell/releases/download/v1.0.0/bell-1.0.0.bin",
     "notes": "two telnet ports, ser2net-style negotiation"
   }
   ```

   The version is compared field by field, so `1.1.0` is newer than `1.0.9`. The
   device downloads the image in 4 kB chunks between `loop()` passes, so telnet
   sessions and this page stay alive during the install, and reboots when it is
   done. The server must send a `Content-Length`; a chunked reply is refused
   rather than risking a truncated image. Set your own manifest under
   **Network → Update manifest** to run your own firmware.

   The image has to fit `OTA_SLOT_BYTES`. An oversized download would be placed
   so that it reaches back over the running firmware, so the device checks the
   size against that limit and refuses instead of writing it.

2. **Upload a `.bin`** straight from the browser, with a progress bar.

TLS is available but the certificate is not validated — an ESP8266 has no room
for a bundled trust store, and the handshake costs a chunk of the heap this
firmware is careful about. Treat the release page as the source of truth, and
check what you are about to flash.

## HTTP API

| Method | Path                     | Purpose                                      |
| ------ | ------------------------ | -------------------------------------------- |
| GET    | `/api/state`             | everything the dashboard shows               |
| GET    | `/api/log?from=&max=`    | log bytes after position `from` (base64)     |
| GET    | `/api/log/txt`           | the log window as text                       |
| GET    | `/api/log/bin`           | the log window as raw bytes                  |
| POST   | `/api/log/clear`         | hide the log window                          |
| POST   | `/api/scan/start`        | start a scan; the access point restarts      |
| GET    | `/api/scan`              | scan state and results                       |
| POST   | `/api/net`               | save station / access point / manifest       |
| POST   | `/api/net/forget`        | forget the station and return to AP mode     |
| POST   | `/api/uart`              | save the UART and LED settings               |
| GET    | `/api/update/status`     | current update state                         |
| POST   | `/api/update/check`      | fetch the manifest and compare               |
| POST   | `/api/update/flash`      | download and install                         |
| POST   | `/update`                | multipart firmware upload                    |
| GET    | `/api/gpio`              | configured outputs and which pins are free    |
| POST   | `/api/gpio/set`          | drive a pin: `pin=5&value=1`                  |
| POST   | `/api/gpio/toggle`       | flip a pin: `pin=5`                           |
| POST   | `/api/gpio/input`        | release a pin back to an input: `pin=5`       |
| POST   | `/api/uart/send`         | type into the device: `text=AT+GMR&eol=crlf`  |
| POST   | `/api/gpio/remember`     | restore a pin at boot: `pin=5&name=relay`     |
| POST   | `/api/uart/send` raw     | exact bytes: `raw=030d0a` (this is Ctrl-C)   |
| POST   | `/api/gpio/forget`       | stop restoring it                             |
| POST   | `/api/system/reboot`     | reboot                                       |
| POST   | `/api/system/factory`    | erase the configuration and reboot           |

Request bodies are form-encoded fields, the way `curl -d` and the page's own
buttons send them. A JSON body is answered with `415` rather than being
silently dropped, because the web server core never turns one into arguments.
Settings posts may carry a subset of the fields; whatever you leave out keeps
its current value.

/api/uart/send types into the device from the browser. It goes out exactly
as a telnet client's keystrokes do, sharing the same queue and back-pressure.
`text` is the payload and `eol` is `crlf` (the default), `cr`, `lf`, `none` or
the word `none`; the payload plus its terminator is capped at 512 bytes.

```sh
curl -X POST --data-urlencode 'text=AT+GMR' -d 'eol=crlf' \
     http://192.168.4.1/api/uart/send
```

`text` may be left empty, which sends the terminator on its own — the **Empty
line** button in the UART log page, for answering a prompt or getting past a
"press enter". Empty payload *and* no terminator is refused as having nothing to
send.

`raw` sends bytes as hex instead, and is the only way to get a control character
out: the text field is trimmed, and Arduino's `trim()` removes everything at or
below a space, so a literal Ctrl-C would arrive as nothing at all.

```sh
curl -X POST -d 'raw=03&eol=none' http://192.168.4.1/api/uart/send   # Ctrl-C
```

The **Ctrl-C** button next to Send does exactly that. Any byte will do —
`raw=04` (Ctrl-D), `raw=1a` (Ctrl-Z), `raw=1b` (ESC) — and a terminator can be
appended to any of them with `eol`.

`/api/log` carries `dirs` — the direction runs overlapping the returned page, as
`[position, length, direction]` where direction is 0 out, 1 in, 2 the firmware's
own messages. The table holds the last 96 direction changes; bytes older than
that are drawn without a marker rather than guessed at.

`/api/state` also carries `gpios`, one entry per pin in pin order, each
`[usable, output, level, restored]`, plus `gpioNames` for the pins that have been
given a name. `telnetPortRo`, `clients` and `clientsRo` report the two ports
separately. If the heap is too thin to build the reply the device answers
`503` with `{"ok":false}` rather than restarting; the page simply asks again.

`/api/log` returns `{"head", "from", "oldest", "cap", "n", "gap", "data"}` and
caps a page at 4 kB regardless of what `max` asks for.
`head` is the position after the newest byte, `from` is the position of the
first returned byte and `gap` says whether something was skipped on the way.

Neither password is ever handed out by `/api/state` — it reports only whether
one is stored. In the network form a password field left empty means "keep the
stored one; ticking **the network is open** or **the access point is open**
clears the stored password on purpose.

None of this is authenticated. Anyone who can reach the port can change the
configuration, upload firmware or erase it. Keep the device on a network you
trust, or on its own access point.

## Memory

The firmware never prints to `Serial`: that pin pair is the payload, and a
single diagnostic byte would end up in the log of whatever you are bridging.
Diagnostics go into the log ring, where the web interface and the telnet log
show them.

```
RAM:   58.0% (used 47488 bytes from 81920 bytes)
Flash: 54.7% (used 571251 bytes from 1044464 bytes)
```

With a log ring taking its share, a few kilobytes of heap are all that is left,
and on this platform a failed allocation restarts the module rather than
returning null. So the paths that can be asked for at the worst moment say no
instead:

* a **telnet session** is only admitted if taking it would still leave the web
  interface enough heap to answer — so the page can always tell you how many
  clients are connected, which is the whole point of watching;
* a **log page** is sized from what is actually free, since the reply costs
  roughly 2.3x the page once the base64 is in it;
* the **state and log endpoints** answer `503` when even that will not fit, and
  the page retries rather than showing a half-built reply.

Static RAM buys the log ring twice over, so:

* the JSON documents that run on a schedule live in static pools sized from
  measurements, and refuse to save anything that did not fit rather than write
  a truncated file;
* the status document — the biggest of them — is built straight into a response
  string and lives only for one request;
* the buffers used by `loop()` are static or per-session, because `loop()` runs
  on a 4 kB stack on this chip.

Build flags in `platformio.ini` worth knowing about: `LOG_MAX_BYTES`,
`LOG_MIN_BYTES`, `LOG_HEAP_KEEP`, `MAX_TELNET_CLIENTS`,
`DEFAULT_TELNET_PORT`, `DEFAULT_TELNET_PORT_RO`,
`WEB_PORT`. `OTA_SLOT_BYTES` in `bell.h` is the ceiling on an over-the-air image
size; check the flash geometry before raising it.

## Checking a board over the serial port

The firmware prints nothing to UART0 — that pin pair is the payload — so the
only thing on the wire at power-up is the ROM loader, which runs at 74880 baud:

```sh
python3 tools/capture_boot.py /dev/ttyUSB1
```

The SDK banner that used to follow it is gone in ESP8266 core 3.x, so silence
after the loader chatter is the expected result, not a sign of a problem. To
confirm the sketch itself is running, add a `Serial.begin(115200)` line to
`setup()` temporarily, or watch for the access point instead.

## Tests

```sh
sh test/native/run.sh
```

The two pieces of this firmware that are pure logic — the telnet negotiation in
`telnet_filter.h` and the log ring in `log.cpp` — are tested on a workstation.
The ring test builds the real `src/log.cpp` against a small Arduino shim and
compares it against a reference model over randomised traffic, including
enough bytes to run the 64-bit counters past their 32-bit limit.
