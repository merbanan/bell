"""Web UI: dashboard, logs, onboarding, live viewer."""

from __future__ import annotations

import os

import aiohttp
from aiohttp import web

from . import config, registry as reg, logs as logs_mod, bridge as bridge_mod

# The module's access point address is fixed in the firmware, so the host
# always has one address that works even when the station interface is down.
MODULE_AP_IP = "192.168.4.1"


class Web:
    """aiohttp app for the host's web UI."""

    def __init__(self, bridge, cfg: config.HostConfig,
                 registry: reg.Registry, logs: logs_mod.Logs):
        self.bridge = bridge
        self.cfg = cfg
        self.registry = registry
        self.logs = logs
        self._dirty = 0

    def pending_dirty(self) -> None:
        """Called when a module appears or disappears; the dashboard polls, so
        nothing else is needed."""
        self._dirty += 1

    # ------------------------------------------------------------------ pages

    async def index(self, request):
        return web.Response(text=PAGE_DASH, content_type="text/html")

    async def logs_page(self, request):
        return web.Response(text=PAGE_LOGS, content_type="text/html")

    async def onboard_page(self, request):
        return web.Response(text=PAGE_ONBOARD, content_type="text/html")

    async def live_page(self, request):
        slot = request.match_info["slot"]
        return web.Response(
            text=PAGE_LIVE.replace("{{SLOT}}", slot), content_type="text/html")

    # ------------------------------------------------------------------ apis

    async def api_state(self, request):
        mods = []
        for slot, m in sorted(self.bridge.modules.items()):
            s = m.session
            mods.append({
                "slot": slot,
                "mac": m.entry["mac"],
                "rw": m.entry["rw"],
                "ro": m.entry["ro"],
                "online": s is not None,
                "operators": len(m.ops),
                "viewers": len(m.viewers),
                "moduleQueue": len(s.outq) if s else 0,
            })
        pend = [{"mac": mac, "connected": info["connected"]}
                for mac, info in sorted(self.registry.pending.items())]
        return web.json_response({"modules": mods, "pending": pend})

    async def api_pending(self, request):
        mac = request.match_info["mac"].strip().upper()
        if mac not in self.registry.pending:
            return web.json_response({"ok": False, "error": "not pending"},
                                     status=404)
        info = self.registry.pending[mac]
        connected = info["connected"] and mac in self.bridge.pending_sessions
        # Module detail comes from the module's own web UI: the station
        # interface first, then the access point.
        async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=4)) as cs:
            async def fetch(ip: str):
                r = await cs.get(f"http://{ip}/api/state")
                return await r.json()
            try:
                ap_state = await fetch(MODULE_AP_IP)
            except Exception:
                ap_state = None
            sta_ip = str(ap_state.get("sta", "")) if ap_state else ""
            if sta_ip:
                try:
                    detail = await fetch(sta_ip)
                except Exception:
                    detail = ap_state
            else:
                detail = ap_state
            gpio = None
            if detail:
                try:
                    gr = await cs.get(
                        f"http://{sta_ip or MODULE_AP_IP}/api/gpio")
                    gpio = await gr.json()
                except Exception:
                    gpio = None
        if not detail:
            return web.json_response({"mac": mac, "connected": connected,
                                      "reachable": False})
        out = {"mac": mac, "connected": connected,
               "reachable": True, "ip": sta_ip or MODULE_AP_IP}
        out.update(detail)
        if gpio is not None:
            out["gpio"] = gpio
        return web.json_response(out)

    async def api_onboard(self, request):
        body = await request.json()
        mac = str(body.get("mac", "")).strip()
        name = str(body.get("name", "")).strip()
        try:
            entry = self.registry.assign(mac.encode("ascii", "ignore"), name)
        except ValueError as e:
            return web.json_response({"ok": False, "error": str(e)}, status=400)
        m = self.bridge.slot_module(entry["name"])
        self.bridge.bind_pending(entry)
        self.logs.event(m.slot,
                        f"assigned by onboarding ({entry['rw']}/{entry['ro']})")
        return web.json_response({"ok": True, "slot": m.slot,
                                  "rw": entry["rw"], "ro": entry["ro"]})

    async def api_logs(self, request):
        root = config.storage_dir(self.cfg)
        slots = sorted(os.listdir(root)) if os.path.isdir(root) else []
        out = {}
        for slot in slots:
            d = os.path.join(root, slot)
            files = sorted(f for f in os.listdir(d) if f.endswith(".log"))
            out[slot] = [{"file": f,
                          "size": os.path.getsize(os.path.join(d, f))}
                         for f in files]
        return web.json_response({"slots": out})

    async def log_file(self, request):
        slot = request.match_info["slot"]
        day = request.match_info["day"]
        if "/" in slot or "/" in day or ".." in slot or ".." in day:
            return web.Response(status=400)
        path = os.path.join(config.storage_dir(self.cfg), slot, f"{day}.log")
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except FileNotFoundError:
            return web.Response(status=404)
        if request.query.get("download"):
            return web.Response(body=data,
                                headers={"Content-Disposition":
                                         f'attachment; filename="{slot}-{day}.log"'})
        return web.Response(body=data, content_type="text/plain")

    # ------------------------------------------------------------------ ws

    async def ws(self, request):
        slot = request.match_info["slot"]
        m = self.bridge.module_for_viewer(slot)
        if m is None:
            return web.Response(status=404)
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        info = request.transport.get_extra_info("peername") or ("?",)
        peer = info[0] if isinstance(info, tuple) else str(info)
        v = bridge_mod.ViewerSession(ws, m, self.bridge, peer)
        m.viewers.append(v)
        await v.run()
        return ws


def build_app(bridge, cfg, registry, logs) -> web.Application:
    w = Web(bridge, cfg, registry, logs)
    bridge.web = w
    app = web.Application()
    app.router.add_get("/", w.index)
    app.router.add_get("/logs", w.logs_page)
    app.router.add_get("/onboard", w.onboard_page)
    app.router.add_get("/api/state", w.api_state)
    app.router.add_get("/api/pending/{mac}", w.api_pending)
    app.router.add_post("/api/onboard", w.api_onboard)
    app.router.add_get("/api/logs", w.api_logs)
    app.router.add_get("/api/logs/{slot}/{day}", w.log_file)
    app.router.add_get("/live/{slot}", w.live_page)
    app.router.add_get("/ws/{slot}", w.ws)
    return app


# ------------------------------------------------------------------ pages
# Kept as plain strings; the pages are small and the JS is tiny.

PAGE_DASH = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host</title>
<style>
body{font:14px monospace;background:#111;color:#ddd;margin:16px}
a{color:#8cf}td,th{padding:4px 10px;border-bottom:1px solid #333;text-align:left}
table{border-collapse:collapse;margin-top:12px}
.ok{color:#8d8}.off{color:#d88}
</style></head><body>
<h1>bell console-server host</h1>
<p id="pend"></p>
<table id="t"><tr><th>slot</th><th>MAC</th><th>RW</th><th>RO</th>
<th>module</th><th>ops</th><th>viewers</th><th>module queue</th>
<th>live</th><th>logs</th></tr></table>
<script>
async function poll(){
  const s = await (await fetch('/api/state')).json();
  const pend = document.getElementById('pend');
  pend.innerHTML = s.pending.length
    ? 'pending: ' + s.pending.map(p =>
        `<a href="/onboard?mac=${encodeURIComponent(p.mac)}">`
        + `${p.mac}${p.connected ? ' (online)' : ' (offline)'}</a>`).join(', ')
    : '';
  const t = document.getElementById('t');
  while (t.rows.length > 1) t.deleteRow(1);
  for (const m of s.modules) {
    const r = t.insertRow();
    r.innerHTML = `<td>${m.slot}</td><td>${m.mac}</td><td>${m.rw}</td><td>${m.ro}</td>`
      + `<td class="${m.online ? 'ok' : 'off'}">${m.online ? 'online' : 'offline'}</td>`
      + `<td>${m.operators}</td><td>${m.viewers}</td><td>${m.moduleQueue}</td>`
      + `<td><a href="/live/${encodeURIComponent(m.slot)}">live</a></td>`
      + `<td><a href="/logs?slot=${encodeURIComponent(m.slot)}">logs</a></td>`;
  }
  setTimeout(poll, 2000);
}
poll();
</script></body></html>"""

PAGE_LOGS = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host logs</title>
<style>
body{font:14px monospace;background:#111;color:#ddd;margin:16px}
a{color:#8cf}li{margin:2px 0}
h3{margin:16px 0 4px}
</style></head><body>
<h1>logs</h1>
<div id="out"></div>
<script>
const want = new URLSearchParams(location.search).get('slot');
async function load(){
  const s = await (await fetch('/api/logs')).json();
  const out = document.getElementById('out');
  out.innerHTML = '';
  for (const [slot, files] of Object.entries(s.slots)) {
    if (want && slot !== want) continue;
    const h = document.createElement('h3'); h.textContent = slot; out.appendChild(h);
    for (const f of files) {
      const p = document.createElement('p');
      p.innerHTML = `<a href="/api/logs/${encodeURIComponent(slot)}/${f.file}">${f.file}</a>`
        + ` (${f.size} bytes) `
        + `<a href="/api/logs/${encodeURIComponent(slot)}/${f.file}?download=1">download</a>`;
      out.appendChild(p);
    }
  }
}
load();
</script></body></html>"""

PAGE_ONBOARD = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host onboarding</title>
<style>
body{font:14px monospace;background:#111;color:#ddd;margin:16px}
input{background:#222;color:#ddd;border:1px solid #444;padding:4px;margin-right:6px}
button{background:#333;color:#ddd;border:1px solid #555;padding:4px 10px}
.ok{color:#8d8}.err{color:#d88}
table{border-collapse:collapse;margin-top:12px}
td{padding:4px 10px;border-bottom:1px solid #333;text-align:left}
a{color:#8cf}
</style></head><body>
<h1>onboarding</h1>
<p><a href="/">dashboard</a></p>
<div id="out"></div>
<script>
const want = new URLSearchParams(location.search).get('mac');
async function load(){
  const r = await fetch('/api/pending/' + encodeURIComponent(want));
  const d = await r.json();
  const out = document.getElementById('out');
  if (!d.ok || !d.reachable) {
    out.innerHTML = `<p class="${d.connected ? 'ok' : 'err'}">`
      + `${want}: ${d.connected ? 'connected, ' : 'offline, '}`
      + `${d.reachable ? '' : 'module web unreachable'}</p>`;
    return;
  }
  const gpio = d.gpio || {available: [], all: []};
  const rows = [];
  const add = (k, v) => rows.push(`<tr><td>${k}</td><td>${v}</td></tr>`);
  add('fw', d.fw);
  add('mode', d.mode);
  add('ap ip', d.ap);
  add('sta ip', d.sta);
  add('ssid', d.ssid);
  add('rssi', d.rssi + ' dBm');
  add('hostname', d.hostname);
  add('telnet rw', d.telnetPort);
  add('telnet ro', d.telnetPortRo);
  add('sta ssid', d.net.staSsid);
  add('sta pass', d.net.staPassSet ? 'set' : 'not set');
  add('ap ssid', d.net.apSsid);
  add('ap pass', d.net.apPassSet ? 'set' : 'not set');
  add('ap channel', d.net.apChannel);
  add('ap hidden', d.net.apHidden);
  add('host', d.net.hostHost + ':' + d.net.hostPort);
  add('uplink', d.uplink.open ? 'open' : 'closed');
  add('uart', `${d.uart.baud} ${d.uart.bits}${d.uart.parity}${d.uart.stops}`);
  add('uart pins', d.uart.swapPins ? 'swapped' : 'standard');
  add('gpio', gpio.available.join(', ') || 'none');
  out.innerHTML = `<table>${rows.join('')}</table>`
    + `<p><input id="name" size="20" placeholder="slot name"> `
    + `<button onclick="assign()">assign</button></p>`;
}
async function assign(){
  const n = document.getElementById('name');
  const r = await fetch('/api/onboard', {method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({mac: want, name: n.value})});
  const j = await r.json();
  const s = document.createElement('p');
  s.className = j.ok ? 'ok' : 'err';
  s.textContent = j.ok ? `${want} assigned as ${j.slot} (rw ${j.rw}, ro ${j.ro})`
                       : j.error;
  document.body.appendChild(s);
}
load();
</script></body></html>"""

PAGE_LIVE = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host live - {{SLOT}}</title>
<style>
body{font:14px monospace;background:#111;color:#ddd;margin:16px}
#term{background:#000;color:#8d8;border:1px solid #444;height:360px;
overflow-y:auto;white-space:pre-wrap;word-break:break-all;padding:8px}
#type{background:#222;color:#ddd;border:1px solid #444;width:60%;padding:6px}
button{background:#333;color:#ddd;border:1px solid #555;padding:4px 10px}
.rw{color:#8d8}.ro{color:#d88}
</style></head><body>
<h1>live - {{SLOT}}</h1>
<p id="mode"></p>
<div id="term"></div>
<p><input id="type" placeholder="type to the module">
<button onclick="send()">send</button>
<span id="stat"></span></p>
<script>
const slot = decodeURIComponent(location.pathname.split('/').pop());
const ws = new WebSocket(`ws://${location.host}/ws/${encodeURIComponent(slot)}`);
let rw = false;
const term = document.getElementById('term');
const modeEl = document.getElementById('mode');
const stat = document.getElementById('stat');
function esc(b){ return b===13?'\\\\r': b===10?'\\\\n': b===27?'\\\\e':
  b<32||b>126 ? '\\\\x'+b.toString(16).padStart(2,'0') : String.fromCharCode(b); }
ws.onopen = () => stat.textContent = 'connected';
ws.onclose = () => stat.textContent = 'closed';
ws.onmessage = (e) => {
  if (typeof e.data === 'string') {           // the one mode frame
    rw = e.data === 'rw';
    modeEl.innerHTML = `<span class="${rw ? 'rw' : 'ro'}">`
      + `${rw ? 'read/write viewer' : 'read-only viewer'}</span>`;
    return;
  }
  const buf = new Uint8Array(e.data);
  let s = '';
  for (const b of buf) s += esc(b);
  term.textContent += s;
  term.scrollTop = term.scrollHeight;
};
function send(){
  const v = document.getElementById('type').value;
  if (!v) return;
  if (!rw) { stat.textContent = 'read-only viewer'; return; }
  ws.send(v + '\\\\r');
  document.getElementById('type').value = '';
}
</script></body></html>"""
