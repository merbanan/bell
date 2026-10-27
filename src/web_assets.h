#pragma once

#include <Arduino.h>

// Served straight out of flash with send_P() so the interface costs no heap.

static const char WEB_INDEX_HTML[] PROGMEM = R"UI(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>UART Telnet Server</title>
<link rel="stylesheet" href="/app.css">
</head>
<body>
<header>
  <h1>UART <span>Telnet Server</span></h1>
  <div id="status" class="status"></div>
</header>

<nav class="tabs" id="tabs">
  <button data-tab="log" class="active">Status</button>
  <button data-tab="comms">UART log</button>
  <button data-tab="uart">UART setup</button>
  <button data-tab="net">Network</button>
  <button data-tab="gpio">GPIO</button>
  <button data-tab="system">System &amp; firmware</button>
</nav>

<main>
  <!-- ----------------------------------------------------------- status -->
  <section id="tab-log" class="tab active">
    <div class="card">
      <h2>Device</h2>
      <table class="kv" id="devstat"></table>
    </div>

    <div class="card">
      <h2>Console</h2>
      <p class="hint">What the firmware itself has to say: start-up, network,
        telnet and GPIO events. The UART's own byte stream is on the
        <b>UART log</b> page.</p>
      <div class="bar">
        <button id="btn-follow" class="on" title="Keep the newest line in view">Follow</button>
        <button id="btn-clear" class="danger">Clear log</button>
      </div>
      <div class="logwrap"><pre id="console"></pre></div>
    </div>
  </section>

  <!-- ----------------------------------------------------------- comms -->
  <section id="tab-comms" class="tab">
    <div class="card lead">
      <h2>UART comms history</h2>
      <p class="hint">Every byte the bridge has carried between a client and the
        device, oldest first. Anything that is not printable shows as a middot.
        The firmware's own messages are on the <b>Status</b> page and are left
        out of this one.</p>
      <div class="bar">
        <button id="btn-follow-ro" class="on" title="Keep the newest line in view">Follow</button>
        <button id="btn-hex" title="Switch between text and a hex dump">Hex view</button>
        <button id="btn-clear-ro" class="danger">Clear log</button>
        <span class="spacer"></span>
        <a class="btn" href="/api/log/txt" download="uart-log.txt"
           title="The whole log window, console lines and UART traffic together">Download .txt</a>
        <a class="btn" href="/api/log/bin" download="uart-log.bin"
           title="The whole log window as raw bytes">Download .bin</a>
      </div>
      <div class="logwrap"><pre id="uartlog"></pre></div>
      <div class="bar sendbar">
        <input id="send-text" type="text" placeholder="type here to send to the device"
               autocomplete="off" spellcheck="false" maxlength="512">
        <button id="btn-send-eol" class="ghost"
                title="Send just the end of line chosen here, with no text">Empty line</button>
        <select id="send-eol" title="What to put after the text">
          <option value="crlf" selected>CRLF</option>
          <option value="cr">CR</option>
          <option value="lf">LF</option>
          <option value="">none</option>
        </select>
        <button id="btn-send">Send</button>
        <button id="btn-ctrlc" class="ghost" title="Send a single 0x03 byte">Ctrl-C</button>
        <span id="send-msg" class="msg"></span>
      </div>
      <div class="bar logstat" id="logstat"></div>
    </div>
    <div class="card tight">
      <h2>Line</h2>
      <div class="flow">
        <div class="side">
          <span class="lbl">device &rarr; UART</span>
          <span class="big" id="c-rx">0</span>
          <span class="rate" id="c-rxr">&mdash;</span>
        </div>
        <div class="arrow" id="c-activity"></div>
        <div class="side">
          <span class="lbl">UART &rarr; device</span>
          <span class="big" id="c-tx">0</span>
          <span class="rate" id="c-txr">&mdash;</span>
        </div>
        <span class="spacer"></span>
        <span class="dot" id="c-dot"></span>
        <span id="c-note" class="note"></span>
        <span id="c-config" class="msg"></span>
      </div>
      <table class="kv" id="c-sessions"></table>
      <p class="hint" id="c-slots"></p>
      <p class="hint" id="c-health"></p>
    </div>

  </section>

  <!-- ------------------------------------------------------------ uart -->
  <section id="tab-uart" class="tab">
    <div class="card">
      <h2>Serial line</h2>
      <div class="grid">
        <label>Speed
          <select id="baud">
            <option value="1200">1200</option>
            <option value="2400">2400</option>
            <option value="4800">4800</option>
            <option value="9600">9600</option>
            <option value="19200">19200</option>
            <option value="38400">38400</option>
            <option value="57600">57600</option>
            <option value="115200" selected>115200</option>
            <option value="230400">230400</option>
            <option value="460800">460800</option>
            <option value="500000">500000</option>
            <option value="921600">921600</option>
            <option value="custom">custom&hellip;</option>
          </select>
        </label>
        <label>Custom baud
          <input id="baudCustom" type="number" min="300" max="921600" step="1" value="115200">
        </label>
        <label>Data bits
          <select id="bits"><option>5</option><option>6</option><option>7</option>
            <option selected>8</option></select>
        </label>
        <label>Parity
          <select id="parity"><option value="N" selected>none</option>
            <option value="E">even</option><option value="O">odd</option></select>
        </label>
        <label>Stop bits
          <select id="stops"><option value="1" selected>1</option>
            <option value="2">2</option></select>
        </label>
        <label>Pins
          <select id="swapPins"><option value="1" selected>TX 15 / RX 13 &mdash; off debug</option>
            <option value="0">TX 1 / RX 3 &mdash; USB serial</option></select>
        </label>
      </div>

      <h2>Stream handling</h2>
      <div class="checks">
        <label><input type="checkbox" id="localEcho"> Echo what a client types back to that client</label>
        <label><input type="checkbox" id="crlfTx"> Turn a typed LF into CR+LF towards the UART</label>
        <label><input type="checkbox" id="logTx"> Also record transmitted bytes in the log</label>
        <label><input type="checkbox" id="clearOnBoot"> Start each boot with an empty log</label>
        <label><input type="checkbox" id="statusLed"> Status LED: blink while booting,
          then steady for
          <input id="ledSeconds" type="number" min="0" max="3600" step="5" class="tiny"
                 title="Seconds it stays lit once the module is ready. 0 leaves it on.">
          s, then off</label>
      </div>

      <div class="bar">
        <button id="btn-uart" class="primary">Apply and save</button>
        <span id="uart-msg" class="msg"></span>
      </div>
    </div>
  </section>

  <!-- ------------------------------------------------------------- net -->
  <section id="tab-net" class="tab">
    <div class="card">
      <h2>Join a network</h2>
      <div class="grid">
        <label class="wide">Network (SSID)
          <input id="staSsid" list="ssidlist" placeholder="not configured" autocomplete="off">
          <datalist id="ssidlist"></datalist>
        </label>
        <label>Password
          <input id="staPass" type="password" placeholder="unchanged" autocomplete="new-password">
        </label>
        <label class="check"><input type="checkbox" id="staOpen"> The network is open (no password)</label>
        <label>Device name
          <input id="hostname" value="uart-server" autocomplete="off">
        </label>
        <label>Give up after (s)
          <input id="staTimeout" type="number" min="3" max="120" value="20">
        </label>
      </div>
      <div class="bar">
        <button id="btn-scan">Scan for networks</button>
        <span id="scan-msg" class="msg"></span>
      </div>
      <div id="nets" class="nets"></div>
    </div>

    <div class="card">
      <h2>Telnet server</h2>
      <div class="grid">
        <label>Read/write port
          <input id="telnetPort" type="number" min="1" max="65535" step="1">
        </label>
        <label>Read-only port
          <input id="telnetPortRo" type="number" min="0" max="65535" step="1">
        </label>
      </div>
      <p class="hint">The read-only port only listens: anything a client sends
        there is thrown away instead of going to the UART. Leave it at
        <b>0</b> and it follows the read/write port, one above it. Changing
        either drops every open session and moves the listener straight away —
        no reboot needed.</p>
    </div>

    <div class="card">
      <h2>Console-server host</h2>
      <div class="grid">
        <label class="wide">Host address
          <input id="hostHost" placeholder="e.g. bellhost.local or 192.168.1.5" autocomplete="off">
        </label>
        <label>Port
          <input id="hostPort" type="number" min="1" max="65535" step="1">
        </label>
      </div>
      <p class="hint">Once the network is joined and a host is set here, the module
        connects to it and sends the UART both ways; leave the address empty and
        the module keeps running its own telnet server only. Clearing the field
        turns the uplink off.</p>
    </div>

    <div class="card">
      <h2>Access point</h2>
      <p class="hint">Used while unconfigured, when a join attempt fails, and on
        demand while the boot button is held. The device is always reachable
        here at 192.168.4.1.</p>
      <div class="grid">
        <label>AP name<input id="apSsid" autocomplete="off"></label>
        <label>AP password<input id="apPass" type="text" autocomplete="new-password"></label>
        <label class="check"><input type="checkbox" id="apOpen"> The access point is open (no password)</label>
        <label>Channel<input id="apChannel" type="number" min="0" max="13" value="0"></label>
        <label class="check"><input type="checkbox" id="apHidden"> Hidden</label>
      </div>
    </div>

    <div class="card">
      <div class="bar">
        <button id="btn-net" class="primary">Save and reconnect</button>
        <button id="btn-forget" class="danger">Forget this network</button>
        <span id="net-msg" class="msg"></span>
      </div>
    </div>
  </section>

  <!-- ------------------------------------------------------------ gpio -->
  <section id="tab-gpio" class="tab">
    <div class="card">
      <h2>All pins</h2>
      <p class="hint">Every pin the module has, whether or not it can be driven.
        A pin that is refused says why; the ones that are free can be pushed
        high or low, and released again when the board should take them back.</p>
      <table class="pinlist" id="gpio-all"></table>
    </div>
    <div class="card">
      <h2>Restored at boot</h2>
      <p class="hint">Pins in this list are put back to the level shown every time
        the module boots.</p>
      <div id="gpio-list" class="pins"></div>
      <div class="bar">
        <span id="gpio-msg" class="msg"></span>
        <span class="spacer"></span>
        <select id="gpio-pin"></select>
        <input id="gpio-name" type="text" placeholder="name, e.g. relay" maxlength="16">
        <button id="gpio-add">Add output</button>
      </div>
    </div>
  </section>

  <!-- ---------------------------------------------------------- system -->
  <section id="tab-system" class="tab">
    <div class="card">
      <h2>Device</h2>
      <div class="bar">
        <button id="btn-reboot">Reboot</button>
        <button id="btn-factory" class="danger">Erase settings and reboot</button>
        <span id="sys-msg" class="msg"></span>
      </div>
    </div>

    <div class="card">
      <h2>Firmware updates</h2>
      <p class="hint">The server asks the manifest below whether a newer build
        exists, then downloads and flashes it over the air. It needs around
        500&nbsp;kB of free flash on this partition layout.</p>
      <div class="grid">
        <label class="wide">Update manifest (JSON)
          <input id="otaUrl" placeholder="https://example.org/uart-server/firmware.json">
        </label>
        <label class="check"><input type="checkbox" id="otaAuto"> Check automatically at boot</label>
      </div>
      <div class="bar">
        <button id="btn-check">Look for updates</button>
        <button id="btn-flash" class="primary" disabled>Install update</button>
        <span id="ota-msg" class="msg"></span>
      </div>
      <div id="ota-bar" class="progress"><i></i></div>
      <div id="ota-info" class="hint"></div>
    </div>

    <div class="card">
      <h2>Upload a firmware image</h2>
      <p class="hint">For a .bin you already have. The device reboots when the
        transfer is done.</p>
      <div class="bar">
        <input type="file" id="file" accept=".bin">
        <button id="btn-upload" disabled>Flash uploaded image</button>
        <span id="up-msg" class="msg"></span>
      </div>
      <div id="up-bar" class="progress"><i></i></div>
    </div>
  </section>
</main>

<script src="/app.js"></script>
</body>
</html>
)UI";

static const char WEB_APP_CSS[] PROGMEM = R"UI(
:root{
  --bg:#0e1116; --panel:#161b22; --line:#232b36; --fg:#d7dee8; --dim:#8b97a8;
  --accent:#4aa3ff; --ok:#3fb950; --warn:#d29922; --bad:#f85149;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Ubuntu,sans-serif}
header{display:flex;flex-wrap:wrap;gap:10px;align-items:center;justify-content:space-between;
  padding:12px 18px;border-bottom:1px solid var(--line);background:var(--panel)}
h1{margin:0;font-size:16px;letter-spacing:.14em;font-weight:600}
h1 span{color:var(--dim);font-weight:400}
h2{margin:0 0 10px;font-size:13px;letter-spacing:.1em;color:var(--dim);text-transform:uppercase}
.status{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.pill{border:1px solid var(--line);border-radius:999px;padding:2px 10px;font-size:12px;
  color:var(--dim);white-space:nowrap}
.pill b{color:var(--fg);font-weight:600;font-variant-numeric:tabular-nums}
.pill.ok{border-color:#1c4b2a;color:var(--ok)}
.pill.ok b{color:var(--ok)}
.pill.warn{border-color:#4d3c12;color:var(--warn)}
.pill.warn b{color:var(--warn)}
.pill.bad{border-color:#5c1f1c;color:var(--bad)}
.pill.bad b{color:var(--bad)}
.pill.live{border-color:var(--accent)}
.pill.live b{color:var(--accent)}
.flow{display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.side{display:flex;flex-direction:column;gap:2px;min-width:150px}
.side .lbl{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}
.side .big{font-size:26px;font-weight:600;font-variant-numeric:tabular-nums;line-height:1.1}
.side .unit{font-size:11px;color:var(--dim);margin-top:-2px}
.side .rate{font-size:12px;color:var(--accent);min-height:1.2em}
.arrow{font-size:22px;color:var(--line);transition:color .1s}
.arrow.live{color:var(--accent)}
/* The history is the reason to open this page, so it takes the top and most of
   the height; the counters below it are reference, and can be smaller. */
.card.lead{margin-bottom:10px}
/* A terminal wants room. Past the normal reading column the comms history runs
   full bleed, so long lines are not wrapped for the sake of the page width. */
@media (min-width:1160px){
  #tab-comms > .card.lead{
    margin-left:calc(50% - 50vw);
    margin-right:calc(50% - 50vw);
    border-left:none;
    border-right:none;
    border-radius:0;
  }
}
.card.lead .logwrap{height:52vh}
.card.tight{padding:10px 12px;margin-bottom:10px}
.card.tight h2{margin:0 0 6px}
.card.tight .flow{align-items:center;gap:12px;margin-bottom:6px}
.card.tight .side{min-width:96px}
.card.tight .side .big{font-size:17px}
.card.tight .side .rate{min-height:0;font-size:11px}
.card.tight .arrow{font-size:15px}
.card.tight .hint{margin:3px 0 0}
.card.tight table.kv td{padding:2px 0}
.card.tight .note{font-size:12px;color:var(--dim)}
.sendbar{gap:8px;margin-top:8px}
.sendbar #btn-send-eol,.sendbar #btn-ctrlc{white-space:nowrap}
/* Secondary action: a quiet button next to Send, not another heavy one. */
button.ghost{background:transparent;border-color:var(--line);color:var(--dim)}
button.ghost:hover{border-color:var(--accent);color:var(--accent)}
.sendbar #send-text{flex:1;min-width:160px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.sendbar #send-eol{width:78px}
/* A seconds field sitting inside a checkbox label. */
input.tiny{width:62px;display:inline-block;margin:0 4px;padding:3px 6px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--line);display:inline-block}
.dot.live{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.pins{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:8px}
.pin{display:flex;align-items:center;gap:10px;border:1px solid var(--line);
  border-radius:6px;padding:8px 10px}
.pin .n{font-weight:600;min-width:52px}
.pin .nm{color:var(--dim);font-size:12px;flex:1;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap}
.pin button{min-width:64px}
.pin.on{border-color:#1c4b2a}
.pin.on button{background:var(--ok);border-color:var(--ok);color:#06121f;font-weight:600}
.pin.stale{opacity:.55}
nav.tabs{display:flex;gap:2px;padding:0 18px;border-bottom:1px solid var(--line);background:var(--panel)}
nav.tabs button{background:none;border:0;border-bottom:2px solid transparent;color:var(--dim);
  padding:10px 14px;cursor:pointer;font:inherit}
nav.tabs button.active{color:var(--fg);border-bottom-color:var(--accent)}
main{padding:18px;max-width:1080px;margin:0 auto}
.tab{display:none}.tab.active{display:block}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;margin-bottom:14px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:10px 0 0}
.bar.logstat{margin:0;padding:8px 2px;color:var(--dim);font-size:12px}
.spacer{flex:1}
button,.btn{background:#1d2530;border:1px solid var(--line);color:var(--fg);border-radius:6px;
  padding:6px 12px;cursor:pointer;font:inherit;text-decoration:none;display:inline-block}
button:hover,.btn:hover{border-color:var(--accent)}
button:disabled{opacity:.45;cursor:not-allowed}
button.primary{background:var(--accent);border-color:var(--accent);color:#06121f;font-weight:600}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
button.on{border-color:var(--accent);color:var(--accent)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}
.grid label,.wide{display:flex;flex-direction:column;gap:4px;font-size:12px;color:var(--dim)}
.wide{grid-column:1/-1}
/* A checkbox is a row, not a stacked field: in a column the text ran past the
   cell and over whatever sat beside it. */
.grid label.check{flex-direction:row;align-items:center;align-self:end;gap:8px;
  font-size:13px;color:var(--fg);line-height:1.35}
.grid label.check input{width:auto;flex:0 0 auto;margin:0}
.checks label.check,.bar label.check{align-self:auto}
input,select{background:#0c1016;border:1px solid var(--line);color:var(--fg);border-radius:6px;
  padding:7px 9px;font:inherit}
/* Without this a field that is deliberately inactive looks like an editable
   one, which is exactly the wrong impression for "custom baud" or
   "install update". */
input:disabled,select:disabled{opacity:.45;cursor:not-allowed}
input[type=file]{padding:5px}
.checks{display:flex;flex-direction:column;gap:8px;font-size:13px;color:var(--fg)}
.checks label{display:flex;gap:8px;align-items:center}
.checks input,.check input{width:auto}
.hint{color:var(--dim);font-size:12px;margin:0 0 10px}
.msg{font-size:12px;color:var(--dim)}
.msg.good{color:var(--ok)}.msg.bad{color:var(--bad)}
table.kv{width:100%;border-collapse:collapse}
table.kv td{padding:3px 0;font-size:13px}
table.kv td:first-child{color:var(--dim);width:38%}
table.kv td:last-child{text-align:right;font-variant-numeric:tabular-nums}
table.pinlist{width:100%;border-collapse:collapse}
table.pinlist th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.06em;
  color:var(--dim);font-weight:600;padding:0 8px 6px 0;border-bottom:1px solid var(--line)}
table.pinlist td{padding:5px 8px 5px 0;font-size:13px;vertical-align:middle;
  border-bottom:1px solid var(--line)}
table.pinlist td.pk{white-space:nowrap;font-variant-numeric:tabular-nums}
table.pinlist td.pk small{color:var(--dim);font-weight:400}
table.pinlist td.why{color:var(--dim);font-size:12px;line-height:1.35}
table.pinlist td.acts{text-align:right;white-space:nowrap}
table.pinlist td.acts button{min-width:52px;margin-left:5px;font-size:12px;padding:3px 8px}
table.pinlist tr.off td{opacity:.5}
table.pinlist tr.off td.why{opacity:1}
.tag{font-size:10px;text-transform:uppercase;letter-spacing:.05em;padding:1px 5px;
  border:1px solid var(--line);border-radius:4px;color:var(--dim);margin-left:6px}
.tag.ro{border-color:var(--ok);color:var(--ok)}
.tag.rw{border-color:var(--accent);color:var(--accent)}
.none{color:var(--dim)}
.logwrap{background:#080b0f;border:1px solid var(--line);border-radius:8px;height:44vh;
  overflow:auto;margin-top:10px}
.logwrap pre{margin:0;padding:10px 12px;font:12px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  white-space:pre;color:#c9d3e0;user-select:text}
.logwrap .tx{color:var(--accent)}
.logwrap .rx{color:var(--ok)}
.nets{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:8px;margin-top:10px}
.net{border:1px solid var(--line);border-radius:6px;padding:8px 10px;cursor:pointer;display:flex;
  justify-content:space-between;gap:8px;align-items:center}
.net:hover{border-color:var(--accent)}
.net .ssid{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net .rssi{color:var(--dim);font-size:12px;white-space:nowrap}
.net.open{border-color:#1c4b2a}
.progress{height:6px;background:#0c1016;border:1px solid var(--line);border-radius:999px;
  overflow:hidden;margin-top:10px;display:none}
.progress.on{display:block}
.progress i{display:block;height:100%;width:0;background:var(--accent);transition:width .2s}
.busy{opacity:.6;pointer-events:none}
@media (prefers-color-scheme:light){
  :root{--bg:#f5f7fa;--panel:#fff;--line:#dde3ea;--fg:#1c2430;--dim:#5d6b7c}
  .logwrap{background:#0d1117}input,select{background:#fff}
}
)UI";

static const char WEB_APP_JS[] PROGMEM = R"UI(
'use strict';
const $  = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));

function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

// URLSearchParams renders an unchecked box as "key=", and the firmware's
// argument parser registers that as a present key - so unticked boxes are
// dropped entirely instead.
function flags(obj){
  const out = {};
  for (const [k, v] of Object.entries(obj)) if (v) out[k] = '1';
  return out;
}

async function post(path, params){
  try {
    const r = await fetch(path, {method:'POST', body:new URLSearchParams(params||{})});
    return await r.json();
  } catch (e) {
    return {ok:false, error:'no answer - the device may be rebooting'};
  }
}
function say(sel, text, kind){
  const el = $(sel);
  el.textContent = text || '';
  el.className = 'msg' + (kind ? ' ' + kind : '');
}

/* ------------------------------------------------------------------ tabs */
function selectTab(name){
  const btn = $$('#tabs button').find(b => b.dataset.tab === name);
  if (!btn) return false;
  btn.click();
  return true;
}
$$('#tabs button').forEach(b => b.addEventListener('click', () => {
  $$('#tabs button').forEach(x => x.classList.toggle('active', x === b));
  $$('.tab').forEach(t => t.classList.toggle('active', t.id === 'tab-' + b.dataset.tab));
  // Both panes read the one ring, so either of them needs it kept up to date.
  if (b.dataset.tab === 'log' || b.dataset.tab === 'comms') {
    logFollow = true; syncFollow(); pump();
  }
  if (b.dataset.tab === 'gpio') refreshGpio();
  // Bookmarkable, and a reload lands back where you were.
  if (location.hash.slice(1) !== b.dataset.tab)
    history.replaceState(null, '', '#' + b.dataset.tab);
}));
function syncFollow(){
  ['#btn-follow', '#btn-follow-ro'].forEach(sel => {
    const b = $(sel);
    b.classList.toggle('on', logFollow);
    b.textContent = logFollow ? 'Following' : 'Paused';
  });
}

/* ------------------------------------------------------------- log model */
const MAXKEEP = 512 * 1024;          // browser-side copy of the ring
const logModel = {buf:new Uint8Array(0), base:0, hex:false, gap:0, gapShown:false,
                  runs:[]};
let logFollow = true, pumping = false, dirty = false;

function b64bytes(s){
  if (!s) return new Uint8Array(0);
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function logAppend(bytes, absFrom){
  if (logModel.buf.length + bytes.length > MAXKEEP){
    const drop = Math.ceil((logModel.buf.length + bytes.length - MAXKEEP) / 65536) * 65536;
    logModel.buf = logModel.buf.slice(drop);
    logModel.base += drop;
  }
  const merged = new Uint8Array(logModel.buf.length + bytes.length);
  merged.set(logModel.buf);
  merged.set(bytes, logModel.buf.length);
  logModel.buf = merged;
}

async function pump(){
  if (pumping) return;
  pumping = true;
  try {
    const from = logModel.base + logModel.buf.length;
    const r = await fetch('/api/log?from=' + from + '&max=2048');
    const j = await r.json();
    // 503 while the module is short of memory: nothing to draw, try again.
    if (j.ok === false){ pumping = false; setTimeout(pump, 400); return; }
    // Counters restart at zero when the device reboots; start a fresh window
    // instead of waiting for a head that is far in the past.
    if (j.head < from) {
      // The device rebooted and its counters start again at zero.
      logModel.buf = new Uint8Array(0); logModel.base = j.from;
      logModel.gap = 0; logModel.gapShown = false; logModel.runs = [];
    } else if (j.gap) {
      // The ring has moved past what we asked for - a wrap, or a clear. Appending
      // anyway would splice two unrelated streams together and leave every
      // position out by the difference, so start again where the device has
      // data and say how much went missing.
      logModel.buf = new Uint8Array(0); logModel.base = j.from;
      logModel.runs = [];
      logModel.gap = j.from - from;
      logModel.gapShown = false;
    }
    // The first page starts at the oldest byte the ring still holds, which is
    // not position 0 unless the device has just booted. Base has to follow
    // that, or every position - and every direction lookup - is out by it.
    if (!logModel.buf.length) logModel.base = j.from;
    logAppend(b64bytes(j.data), j.from);
    // Direction runs, absolute positions, so pages compose as the ring wraps.
    if (j.dirs && j.dirs.length){
      logModel.runs = logModel.runs.concat(j.dirs);
      if (logModel.runs.length > 400) logModel.runs = logModel.runs.slice(-400);
    }
    dirty = true;
    $('#logstat').textContent =
      'window ' + j.oldest + '\u2013' + j.head + '   ring ' + (j.head - j.oldest) +
      ' / ' + j.cap + ' bytes' + (stateCache.heap ?
      '   free heap ' + (stateCache.heap / 1024).toFixed(1) + ' kB' : '');

    // A busy line fills the ring faster than one page per poll, so go straight
    // back for the next one instead of waiting out the interval.
    if (j.head > from + j.n) setTimeout(pump, 30);
  } catch (e) { /* the device is rebooting or out of reach */ }
  pumping = false;
}

// The firmware writes its own messages as "[hh:mm:ss.mmm] [tag] ..." and drops
// the UART's bytes into the same ring. Both panes read that one ring; this is
// the bit that tells the two apart.
const CONSOLE_RE = /\[\d\d:\d\d:\d\d\.\d\d\d\] \[[a-z]+\][^\r\n]*\r?\n?/g;

// The same stream with the firmware's own messages cut out, so the UART pane
// shows the traffic rather than a second copy of the console. Byte ranges are
// located in the decoded text and then removed from the original bytes, so what
// survives is exactly what crossed the line.
function uartOnly(){
  const buf = logModel.buf;
  const text = new TextDecoder('latin1').decode(buf);
  const re = new RegExp(CONSOLE_RE.source, 'g');
  const spans = [];
  let m;
  while ((m = re.exec(text)) !== null) spans.push([m.index, m.index + m[0].length]);
  if (!spans.length) return buf;

  let cut = 0;
  for (const s of spans) cut += s[1] - s[0];
  const out = new Uint8Array(buf.length - cut);
  let src = 0, dst = 0;
  for (const s of spans) {
    out.set(buf.subarray(src, s[0]), dst);
    dst += s[0] - src;
    src = s[1];
  }
  out.set(buf.subarray(src), dst);
  return out;
}

function tail(){
  const buf = logModel.buf;
  return buf.length > 256 * 1024 ? buf.subarray(buf.length - 256 * 1024) : buf;
}

// What the device says about itself, without the serial traffic in the way.
function renderConsole(){
  const text = new TextDecoder('latin1').decode(tail()).replace(/\r/g, '');
  // CONSOLE_RE swallows the break that ends each line - the UART view needs it
  // cut out along with the line - so drop it here before joining, or every
  // message comes out followed by a blank one.
  let out = (text.match(CONSOLE_RE) || [])
    .map(line => line.replace(/\n+$/, ''))
    .join('\n');
  if (logModel.gap && !logModel.gapShown)
    out = '... ' + logModel.gap + ' bytes lost ...\n' + out;

  const el = $('#console');
  scrollToEnd(el, out || '(the console has not said anything yet)');
}

// The <pre> is as tall as its content; the box around it is what scrolls, so
// that is the element that has to be told where to go.
function scrollToEnd(el, text, isHtml){
  const box = el.closest('.logwrap') || el;
  const wasAtEnd = box.scrollTop + box.clientHeight >= box.scrollHeight - 30;
  if (isHtml) el.innerHTML = text; else el.textContent = text;
  if (logFollow || wasAtEnd) box.scrollTop = box.scrollHeight;
}

// The direction a byte travelled, or -1 where the device no longer remembers.
function dirAt(abs){
  const runs = logModel.runs;
  for (let i = runs.length - 1; i >= 0; i--){
    const r = runs[i];
    if (abs >= r[0] && abs < r[0] + r[1]) return r[2];
  }
  return -1;
}

// One entry per line of UART traffic: where it started in the ring, and its
// text. The console's own lines are left out, but the positions are kept so
// each line can still be matched against the direction runs.
function uartLines(){
  const text = new TextDecoder('latin1').decode(logModel.buf);
  const re = new RegExp(CONSOLE_RE.source, 'g');
  const spans = [];
  let m;
  while ((m = re.exec(text)) !== null) spans.push([m.index, m.index + m[0].length]);

  // A console line lands wherever it lands, which is often in the middle of a
  // line the device was already sending, so the spans are cut out of each line
  // rather than the whole line being judged by where it began.
  const trim = (a, b) => {
    let out = '', at = a, first = -1;
    for (const s of spans) {
      if (s[1] <= at || s[0] >= b) continue;
      if (s[0] > at) { if (first < 0) first = at; out += text.slice(at, s[0]); }
      at = Math.max(at, s[1]);
    }
    if (at < b) { if (first < 0) first = at; out += text.slice(at, b); }
    return [out, first];
  };

  const lines = [];
  let start = 0;
  for (let i = 0; i <= text.length; i++){
    if (i !== text.length && text[i] !== '\n') continue;
    if (i > start) {
      const [body, at] = trim(start, i);
      if (body.replace(/\r/g, '').length) lines.push([at < 0 ? start : at, body]);
    }
    start = i + 1;
  }
  return lines;
}

function renderLog(){
  const buf = uartOnly();
  const show = buf.length > 256 * 1024 ? buf.subarray(buf.length - 256 * 1024) : buf;
  const gap = (logModel.gap && !logModel.gapShown)
    ? '... ' + logModel.gap + ' bytes lost ...\n' : '';
  if (logModel.gap) logModel.gapShown = true;   // once, not pinned to the top forever

  let body;
  if (!logModel.hex){
    // latin1 keeps every byte one character, so nothing is mangled on the way.
    // Colour says which way it went: blue for what was sent to the device,
    // green for what came back. Lines the device can no longer vouch for are
    // left plain rather than guessed at.
    body = uartLines().map(([pos, t]) => {
      const text = esc(t.replace(/\r/g, '')).replace(/[^\x09\x20-\x7e]/g, '\u00b7');
      const d = dirAt(logModel.base + pos);
      return d === 1 ? '<span class="rx">' + text + '</span>'
           : d === 0 ? '<span class="tx">' + text + '</span>'
           : text;
    }).join('\n');
  } else {
    const base = logModel.base + (buf.length - show.length);
    const rows = [];
    for (let i = 0; i < show.length; i += 16){
      const row = show.subarray(i, i + 16);
      let hx = '', asc = '';
      for (let k = 0; k < 16; k++){
        hx += (k ? ' ' : '') + (k < row.length ? row[k].toString(16).padStart(2, '0') : '  ');
        const c = row[k];
        asc += (k < row.length && c >= 32 && c < 127) ? String.fromCharCode(c) : '\u00b7';
      }
      rows.push((base + i).toString(16).padStart(8, '0') + '  ' + hx + '  |' + asc + '|');
    }
    body = rows.join('\n');
  }

  scrollToEnd($('#uartlog'), gap + (body || '(nothing has crossed the line yet)'), true);
  dirty = false;
}

$('#btn-follow').addEventListener('click', () => { logFollow = !logFollow; syncFollow(); });
$('#btn-follow-ro').addEventListener('click', () => { logFollow = !logFollow; syncFollow(); });
$('#btn-hex').addEventListener('click', e => {
  logModel.hex = !logModel.hex;
  e.target.classList.toggle('on', logModel.hex);
  renderLog();
});
async function clearLog(){
  const j = await post('/api/log/clear');
  if (j.ok){
    logModel.buf = new Uint8Array(0); logModel.base = 0;
    logModel.gap = 0; logModel.gapShown = false; logModel.runs = [];
    renderLog(); renderConsole();
  }
}
$('#btn-clear').addEventListener('click', clearLog);
$('#btn-clear-ro').addEventListener('click', clearLog);

/* ----------------------------------------------------------------- state */
let stateCache = {};

async function refreshState(){
  try {
    stateCache = await (await fetch('/api/state')).json();
  } catch (e){ return; }
  const s = stateCache;
  const pill = (label, value, cls) =>
    '<span class="pill ' + (cls || '') + '">' + label + ' <b>' + esc(value) + '</b></span>';

  // The status pane: everything you need to know without opening anything.
  $('#status').innerHTML = [
    pill('mode', s.mode, s.mode === 'client' ? 'ok' : 'warn'),
    s.sta ? pill('ip', s.sta, 'ok') : pill('ip', '–', 'bad'),
    pill('up', s.uptime),
    // Both ports draw on one pool of slots, so this is the total, not just the
    // read/write clients. The breakdown lives on the UART log page.
    pill('telnet', (s.clients + s.clientsRo) + '/' + s.maxClients,
         (s.clients + s.clientsRo) ? 'live' : ''),
    pill('heap', (s.heap / 1024).toFixed(1) + ' kB'),
    pill('log', (s.log.capacity / 1024).toFixed(0) + ' kB'),
    pill('fw', s.fw)
  ].join('');

  renderComms(s);
  if ($('#tab-gpio').classList.contains('active')) refreshGpio();

  $('#devstat').innerHTML = rows([
    ['Firmware', s.fw], ['Board', s.board], ['Chip id', s.chipId], ['MAC', s.mac],
    ['Flash', s.flash], ['SDK', s.sdk], ['Uptime', s.uptime],
    ['Free heap', (s.heap / 1024).toFixed(1) + ' kB'],
    ['Log ring', (s.log.capacity / 1024).toFixed(0) + ' kB'],
    ['Mode', s.mode + (s.ssid ? ' (' + esc(s.ssid) + ')' : '')],
    ['Address', s.sta || ('access point ' + s.ap)],
    ['Signal', s.rssi + ' dBm']
  ]);

  fillForms(s);
  if ($('#tab-uart').classList.contains('active')) syncBaud(s.uart.baud);
}

const rows = pairs => pairs.map(p => '<tr><td>' + p[0] + '</td><td>' + p[1] + '</td></tr>').join('');

function human(n){
  n = Number(n) || 0;
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' kB';
  return (n / 1048576).toFixed(2) + ' MB';
}
function rate(bps){
  if (bps <= 0) return 'idle';
  return bps < 1024 ? bps.toFixed(0) + ' B/s'
       : bps < 1048576 ? (bps / 1024).toFixed(1) + ' kB/s'
       : (bps / 1048576).toFixed(2) + ' MB/s';
}

// Rates and the activity lamp are derived here rather than in the firmware:
// the device already counts bytes, and working it out on this side keeps the
// numbers meaningful no matter how often the page happens to poll.
let comms = { rx:0, tx:0, rrx:0, rtx:0, at:0 };
function renderComms(s){
  const now = Date.now();
  const dt = (now - comms.at) / 1000;
  if (dt > 0.4){                       // too soon to mean anything
    comms.rrx = Math.max(0, (s.uart.rx - comms.rx) / dt);
    comms.rtx = Math.max(0, (s.uart.tx - comms.tx) / dt);
    comms.rx = s.uart.rx;
    comms.tx = s.uart.tx;
    comms.at = now;
  }

  const moving = comms.rrx > 0 || comms.rtx > 0;
  $('#c-rx').textContent = human(s.uart.rx);
  $('#c-tx').textContent = human(s.uart.tx);
  $('#c-rxr').textContent = rate(comms.rrx);
  $('#c-txr').textContent = rate(comms.rtx);
  $('#c-activity').classList.toggle('live', moving);
  $('#c-dot').classList.toggle('live', moving);
  $('#c-note').textContent = moving ? 'traffic on the line'
      : s.uart.rx + s.uart.tx ? 'line is quiet'
      : 'waiting for traffic';
  $('#c-config').textContent = s.uart.baud + ' ' + s.uart.bits + s.uart.parity +
    s.uart.stops + (s.uart.swapPins ? ' · GPIO15/13' : ' · GPIO1/3');

  $('#c-slots').textContent = (s.clients + s.clientsRo) + ' of ' +
    s.maxClients + ' session slots in use';

  // Ports and clients in one table: the two port rows are always there, the
  // client rows only when there is somebody to list.
  const lines = [
    ['Read/write &middot; port ' + s.telnetPort,
     s.clients ? s.clients + ' connected' : 'none connected'],
    ['Read only &middot; port ' + (s.telnetPortRo || 'off'),
     s.clientsRo ? s.clientsRo + ' connected' : 'none connected']
  ];
  if (s.uplink) lines.push([
    'Console-server host &middot; ' +
      (s.uplink.configured ? esc(s.uplink.host) + ':' + s.uplink.port : 'off'),
    s.uplink.open
      ? 'connected &middot; &uarr; ' + human(s.uplink.tx) + ' sent, &darr; ' +
        human(s.uplink.rx) + ' received' +
        (s.uplink.queued ? ' &nbsp; <small>' + s.uplink.queued + ' B queued</small>' : '') +
        (s.uplink.dropped ? ' &nbsp; <small>' + human(s.uplink.dropped) + ' dropped</small>' : '')
      : s.uplink.configured
        ? 'retrying every ' + Math.max(1, Math.round(s.uplink.retryIn / 1000)) + ' s'
        : 'not configured'
  ]);
  (s.sessions || []).forEach(c => lines.push([
    esc(c.ip) + (c.ro ? ' <span class="tag ro">read only</span>'
                      : ' <span class="tag rw">read/write</span>') +
      (c.binary ? ' <small>(binary)</small>' : ''),
    c.ro
      ? '&darr; ' + human(c.tx) +
        (c.ig ? ' &nbsp; <small>' + human(c.ig) + ' ignored</small>' : '')
      : '&darr; ' + human(c.tx) + ' &nbsp; &uarr; ' + human(c.rx) +
        (c.queued ? ' &nbsp; <small>' + c.queued + ' B queued</small>' : '')
  ]));
  $('#c-sessions').innerHTML = rows(lines);

  // textContent, so separators have to be the character itself: an entity
  // would show up here spelled out.
  $('#c-health').textContent =
    (s.uart.errors ? s.uart.errors + ' receiver overrun(s)' : 'no receiver overruns') +
    ' \u00b7 ' + s.log.written + ' bytes recorded' +
    ' \u00b7 log window ' + human(s.log.written - s.log.oldest) +
    ' of ' + human(s.log.capacity);
}

function fillForms(s){
  const a = document.activeElement;
  if (a && ((a.tagName === 'INPUT' && a.type !== 'checkbox') || a.tagName === 'SELECT' ||
            a.tagName === 'TEXTAREA')) return;   // never fight the operator

  const n = s.net, u = s.uart;
  $('#staSsid').value = n.staSsid;
  $('#staPass').placeholder = n.staPassSet ? 'stored — type * to keep' : 'not set';
  $('#hostname').value = n.hostname;
  $('#staTimeout').value = n.staTimeout;
  $('#telnetPort').value = n.telnetPort;
  $('#telnetPortRo').value = n.telnetPortRo || 0;
  $('#hostHost').value = n.hostHost;
  $('#hostPort').value = n.hostPort;
  $('#apSsid').value = n.apSsid;
  $('#apPass').value = '';
  $('#apPass').placeholder = n.apPassSet ? 'stored — type * to keep' : 'none set';
  $('#apOpen').checked = !n.apPassSet;
  $('#apChannel').value = n.apChannel;
  $('#apHidden').checked = n.apHidden;
  $('#otaUrl').value = n.otaUrl;
  $('#otaAuto').checked = n.otaAuto;

  $('#bits').value = u.bits;
  $('#parity').value = u.parity;
  $('#stops').value = u.stops;
  $('#swapPins').value = u.swapPins ? '1' : '0';
  for (const id of ['localEcho','crlfTx','logTx','clearOnBoot','statusLed']) $('#' + id).checked = u[id];
  $('#ledSeconds').value = u.ledSeconds;
  syncBaud(u.baud);
}

function syncBaud(baud){
  const sel = $('#baud');
  const known = Array.from(sel.options).some(o => o.value == baud);
  sel.value = known ? String(baud) : 'custom';
  // Only editable while "custom" is the selected speed - the value typed there
  // is not sent otherwise, so an enabled box would just invite confusion.
  $('#baudCustom').disabled = known;
  if (!known) $('#baudCustom').value = baud;
}
$('#baud').addEventListener('change', () => {
  $('#baudCustom').disabled = $('#baud').value !== 'custom';
});

$('#btn-uart').addEventListener('click', async () => {
  const baud = $('#baud').value === 'custom'
    ? $('#baudCustom').value : $('#baud').value;
  const j = await post('/api/uart', Object.assign({
    baud, bits:$('#bits').value, parity:$('#parity').value, stops:$('#stops').value
  }, flags({
    swapPins:$('#swapPins').value === '1',
    localEcho:$('#localEcho').checked, crlfTx:$('#crlfTx').checked,
    logTx:$('#logTx').checked, clearOnBoot:$('#clearOnBoot').checked,
    ledSeconds:$('#ledSeconds').value,
    statusLed:$('#statusLed').checked
  })));
  say('#uart-msg', j.ok ? 'UART set to ' + baud + ' ' + $('#bits').value +
      $('#parity').value + $('#stops').value : j.error, j.ok ? 'good' : 'bad');
  refreshState();
});

/* --------------------------------------------------------------- network */
function renderNetworks(nets){
  const list = $('#ssidlist');
  list.innerHTML = '';
  $('#nets').innerHTML = '';
  nets.forEach(n => {
    const opt = document.createElement('option');
    opt.value = n.ssid;
    list.appendChild(opt);
    const bar = n.rssi >= -60 ? '\u2588\u2588\u2588' : n.rssi >= -75 ? '\u2588\u2588\u2591'
              : n.rssi >= -88 ? '\u2588\u2591\u2591' : '\u2591\u2591\u2591';
    const el = document.createElement('div');
    el.className = 'net' + (n.enc ? '' : ' open');
    el.dataset.ssid = n.ssid;
    el.innerHTML = '<span class="ssid"></span><span class="rssi">' + bar + ' ' + n.rssi + '</span>';
    el.firstElementChild.textContent = n.ssid + (n.enc ? '' : ' (open)');
    el.addEventListener('click', () => { $('#staSsid').value = n.ssid; $('#staPass').focus(); });
    $('#nets').appendChild(el);
  });
}

$('#btn-scan').addEventListener('click', async () => {
  const btn = $('#btn-scan');
  btn.disabled = true;
  say('#scan-msg', 'scanning…');
  $('#nets').innerHTML = '';

  const started = await post('/api/scan/start');
  if (!started.ok){
    btn.disabled = false;
    say('#scan-msg', started.error || 'could not start the scan', 'bad');
    return;
  }

  // The device restarts its access point to get a station radio, so this page
  // loses its connection and has to come back; poll until it answers.
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline){
    await new Promise(r => setTimeout(r, 900));
    let j;
    try {
      j = await (await fetch('/api/scan')).json();
    } catch (e){
      continue;                       // access point was restarting, try again
    }
    if (j.scanning) continue;

    btn.disabled = false;
    if (j.error){ say('#scan-msg', j.error, 'bad'); return; }
    say('#scan-msg', j.count + ' network(s) found' +
        (j.truncated ? ', showing the first ' + j.networks.length : ''));
    renderNetworks(j.networks);
    return;
  }
  btn.disabled = false;
  say('#scan-msg', 'the device did not answer - is it still on the network?', 'bad');
});

$('#btn-net').addEventListener('click', async () => {
  const j = await post('/api/net', Object.assign({
    staSsid:$('#staSsid').value,
    staPass:$('#staOpen').checked ? '' : ($('#staPass').value || '*'),
    hostname:$('#hostname').value, staTimeout:$('#staTimeout').value,
    telnetPort:$('#telnetPort').value,
    telnetPortRo:$('#telnetPortRo').value,
    hostHost:$('#hostHost').value,
    hostPort:$('#hostPort').value,
    apSsid:$('#apSsid').value,
    apPass:$('#apOpen').checked ? '' : ($('#apPass').value || '*'),
    apChannel:$('#apChannel').value,
    otaUrl:$('#otaUrl').value
  }, flags({apHidden:$('#apHidden').checked, otaAuto:$('#otaAuto').checked})));
  say('#net-msg', j.ok ? 'saved — press Reboot on the System tab' : j.error,
      j.ok ? 'good' : 'bad');
});

$('#btn-forget').addEventListener('click', async () => {
  if (!confirm('Forget the stored network and fall back to access point mode?')) return;
  const j = await post('/api/net/forget');
  say('#net-msg', j.ok ? 'forgotten — the device will come back as an access point'
                       : j.error, j.ok ? 'good' : 'bad');
});

/* ----------------------------------------------------------------- gpio */
function renderGpio(g, available){
  const list = $('#gpio-list');
  if (!g.pins.length){
    list.innerHTML = '<p class="hint">nothing configured yet</p>';
  } else {
    list.innerHTML = '';
    g.pins.forEach(p => {
      const el = document.createElement('div');
      el.className = 'pin' + (p.value ? ' on' : '') + (p.driving ? '' : ' stale');
      const nm = document.createElement('span');
      nm.className = 'nm';
      nm.textContent = p.name || '(unnamed)';
      const btn = document.createElement('button');
      btn.textContent = p.value ? 'ON' : 'OFF';
      btn.addEventListener('click', async () => {
        const r = await post('/api/gpio/toggle', { pin:p.pin });
        if (!r.ok){ say('#gpio-msg', r.error, 'bad'); return; }
        refreshGpio();
      });
      const del = document.createElement('button');
      del.className = 'danger';
      del.textContent = '×';
      del.title = 'stop restoring this pin at boot';
      del.addEventListener('click', async () => {
        const r = await post('/api/gpio/forget', { pin:p.pin });
        if (!r.ok) say('#gpio-msg', r.error, 'bad');
        refreshGpio();
      });
      el.append(nm, btn, del);
      const tag = document.createElement('span');
      tag.className = 'n';
      tag.textContent = 'GPIO' + p.pin;
      el.prepend(tag);
      if (!p.driving) nm.textContent += ' — not available';
      list.appendChild(el);
    });
  }

  const sel = $('#gpio-pin');
  const current = sel.value;
  sel.innerHTML = '';
  const used = g.pins.map(p => p.pin);
  available.filter(p => !used.includes(p)).forEach(p => {
    const o = document.createElement('option');
    o.value = p; o.textContent = 'GPIO' + p;
    sel.appendChild(o);
  });
  if (current && available.includes(+current)) sel.value = current;
}

// Why a pin is refused, in words rather than the short code the API sends.
const PIN_WHY = {
  flash:   'wired to the SPI flash - driving it could corrupt the firmware',
  console: 'UART0, and the USB serial console',
  boot:    'sampled at reset; pulling it low starts the bootloader',
  uart:    'the UART is using this pair',
  led:     'the status LED already owns it'
};

// One row per pin, all 17 of them, driven or not.
function renderGpioAll(g){
  const btn = (act, pin, label, cls) =>
    '<button data-act="' + act + '" data-pin="' + pin + '"' +
    (cls ? ' class="' + cls + '"' : '') + '>' + label + '</button>';

  $('#gpio-all').innerHTML =
    '<tr><th>Pin</th><th>Now</th><th>Why</th><th></th></tr>' +
    g.all.map(p => {
      const now = p.d ? 'output ' + (p.l ? 'high' : 'low')
                      : 'input' + (p.u && p.l ? ', pulled high'
                                              : p.u ? ', pulled low' : '');
      const why = p.u ? '' : esc(PIN_WHY[p.w] || p.w || 'not available');
      const acts = !p.u ? ''
        : btn('high', p.p, 'High') + btn('low', p.p, 'Low') +
          (p.d ? btn('in', p.p, 'Release', 'danger') : '') +
          (p.r ? btn('forget', p.p, 'Forget', 'danger') : '');
      return '<tr class="' + (p.u ? '' : 'off') + '">' +
        '<td class="pk">GPIO' + p.p +
          (p.n ? ' <small>' + esc(p.n) + '</small>' : '') + '</td>' +
        '<td>' + now + '</td>' +
        '<td class="why">' + why + '</td>' +
        '<td class="acts">' + acts + '</td></tr>';
    }).join('');
}

// The buttons are rebuilt on every poll, so listen on the table itself rather
// than rebinding a handler per row.
$('#gpio-all').addEventListener('click', async e => {
  const b = e.target.closest('button[data-act]');
  if (!b) return;

  const pin = b.dataset.pin;
  const act = b.dataset.act;
  const call = { high: ['/api/gpio/set', { pin, value:1 }],
                 low:  ['/api/gpio/set', { pin, value:0 }],
                 'in': ['/api/gpio/input', { pin }],
                 forget: ['/api/gpio/forget', { pin }] }[act];

  const r = await post(call[0], call[1]);
  if (!r.ok) say('#gpio-msg', r.error, 'bad');
  else if (act === 'forget') say('#gpio-msg', 'GPIO' + pin + ' will not be restored at boot', 'good');
  else if (act === 'in') say('#gpio-msg', 'GPIO' + pin + ' is an input again', 'good');
  refreshGpio();
});

async function refreshGpio(){
  try {
    const g = await (await fetch('/api/gpio')).json();
    renderGpioAll(g);
    renderGpio(g, g.available);
    if (stateCache) stateCache.gpioAvailable = g.available;
  } catch (e){}
}

$('#gpio-add').addEventListener('click', async () => {
  const pin = $('#gpio-pin').value;
  if (!pin){ say('#gpio-msg', 'no pin left to add', 'bad'); return; }
  const r = await post('/api/gpio/remember', { pin, name:$('#gpio-name').value });
  if (!r.ok){ say('#gpio-msg', r.error, 'bad'); return; }
  say('#gpio-msg', 'GPIO' + pin + ' will be restored at boot', 'good');
  $('#gpio-name').value = '';
  refreshGpio();
});

const EOL_NAME = {crlf:'CRLF', cr:'CR', lf:'LF', '':'no line ending'};

// emptyLine sends the terminator on its own, which is the point of a device
// that just wants a bare newline. The text box is left as it is - the point of
// the button is not to throw away what was already typed.
async function sendToUart(emptyLine){
  const text = $('#send-text').value;
  const eol  = $('#send-eol').value;
  if (!emptyLine && !text.trim()){
    if (!eol) { say('#send-msg', 'nothing to send', 'bad'); return; }
  }

  $('#btn-send').disabled = true;
  const r = await post('/api/uart/send', { text: emptyLine ? '' : text, eol });
  $('#btn-send').disabled = false;

  if (!r.ok){ say('#send-msg', r.error, 'bad'); return; }
  say('#send-msg', emptyLine
    ? 'sent an empty line (' + EOL_NAME[eol] + ')'
    : 'sent ' + r.sent + ' bytes', 'good');
  if (!emptyLine) $('#send-text').value = '';
}
$('#btn-send').addEventListener('click', () => sendToUart(false));
$('#btn-send-eol').addEventListener('click', () => sendToUart(true));

// Control characters cannot go through the text field, which is trimmed; they
// are sent as hex so the byte arrives exactly as it is.
$('#btn-ctrlc').addEventListener('click', async () => {
  const r = await post('/api/uart/send', { raw:'03', eol:'none' });
  if (!r.ok){ say('#send-msg', r.error, 'bad'); return; }
  say('#send-msg', 'sent Ctrl-C (0x03)', 'good');
});
$('#send-text').addEventListener('keydown', e => {
  if (e.key === 'Enter') sendToUart(false);
});

/* ---------------------------------------------------------------- system */
$('#btn-reboot').addEventListener('click', async () => {
  if (!confirm('Reboot the device?')) return;
  say('#sys-msg', 'rebooting…');
  await post('/api/system/reboot');
});
$('#btn-factory').addEventListener('click', async () => {
  if (!confirm('Erase every stored setting and return to access point mode?')) return;
  say('#sys-msg', 'erasing…');
  await post('/api/system/factory');
});

/* ---------------------------------------------------------- firmware OTA */
function bar(sel, pct){ const b = $(sel); b.classList.toggle('on', pct > 0); b.firstElementChild.style.width = pct + '%'; }

let otaPoll = null;

function watchOta(){
  if (otaPoll) return;
  otaPoll = setInterval(async () => {
    let o = {};
    try { o = (await (await fetch('/api/update/status')).json()).ota || {}; }
    catch (e) { return; }                       // gone: it is rebooting
    const pct = o.total ? Math.round(o.written * 100 / o.total) : 0;
    if (o.state === 'downloading'){
      bar('#ota-bar', pct);
      say('#ota-msg', 'installing… ' + pct + '%');
    } else if (o.state === 'failed'){
      clearInterval(otaPoll); otaPoll = null;
      bar('#ota-bar', 0);
      say('#ota-msg', o.error, 'bad');
    } else if (o.state === 'rebooting'){
      clearInterval(otaPoll); otaPoll = null;
      bar('#ota-bar', 0);
      say('#ota-msg', 'installed, rebooting…', 'good');
    }
  }, 600);
}

$('#btn-check').addEventListener('click', async () => {
  say('#ota-msg', 'contacting the manifest…');
  const res = await post('/api/update/check');
  if (!res.ok){ say('#ota-msg', res.error || 'the device did not answer', 'bad'); return; }

  const o = res.ota || {};
  $('#btn-flash').disabled = !o.available;
  $('#ota-info').textContent = o.notes || '';
  say('#ota-msg',
    o.state === 'failed' ? o.error
      : o.available ? 'version ' + o.latest + ' is available, running ' + o.current
      : 'running ' + o.current + ' is the newest published build',
    o.state === 'failed' ? 'bad' : o.available ? 'good' : '');
});

$('#btn-flash').addEventListener('click', async () => {
  if (!confirm('Download and install the new firmware?')) return;
  say('#ota-msg', 'starting…');
  const res = await post('/api/update/flash');
  if (!res.ok || (res.ota && res.ota.state === 'failed')){
    say('#ota-msg', (res.ota && res.ota.error) || res.error, 'bad');
    return;
  }
  watchOta();
});

/* ------------------------------------------------------- firmware upload */
$('#file').addEventListener('change', e => { $('#btn-upload').disabled = !e.target.files.length; });
$('#btn-upload').addEventListener('click', () => {
  const f = $('#file').files[0];
  if (!f) return;
  const fd = new FormData();
  fd.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = e => {
    if (e.lengthComputable) { bar('#up-bar', Math.round(e.loaded * 100 / e.total));
                              say('#up-msg', 'uploading… ' + Math.round(e.loaded / 1024) + ' kB'); }
  };
  xhr.onload = () => {
    const j = JSON.parse(xhr.responseText || '{}');
    say('#up-msg', j.ok ? 'accepted, rebooting' : j.error, j.ok ? 'good' : 'bad');
  };
  xhr.onerror = () => say('#up-msg', 'upload failed', 'bad');
  xhr.send(fd);
});

/* ------------------------------------------------------------------ loop */
if (!selectTab(location.hash.slice(1) || 'log')) selectTab('log');
syncFollow();
renderLog();
renderConsole();
refreshState();
setInterval(refreshState, 4000);
setInterval(async () => {
  if ($('#tab-log').classList.contains('active') ||
      $('#tab-comms').classList.contains('active')) await pump();
  if (dirty){ renderLog(); renderConsole(); }
}, 800);
)UI";
