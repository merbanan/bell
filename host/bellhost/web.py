"""Web UI: dashboard, logs, onboarding, live viewer."""

from __future__ import annotations

import os

import aiohttp
from aiohttp import web

from . import config, registry as reg, logs as logs_mod, bridge as bridge_mod

# The module's access point address is fixed in the firmware, so the host
# always has one address that works even when the station interface is down.
MODULE_AP_IP = "192.168.4.1"
web_site = None


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
        return web.Response(text=self.render(PAGE_DASH), content_type="text/html")

    async def logs_page(self, request):
        return web.Response(text=self.render(PAGE_LOGS), content_type="text/html")

    async def onboard_page(self, request):
        return web.Response(text=self.render(PAGE_ONBOARD), content_type="text/html")

    async def live_page(self, request):
        slot = request.match_info["slot"]
        return web.Response(
            text=self.render(PAGE_LIVE).replace("{{SLOT}}", slot),
            content_type="text/html")

    async def settings_page(self, request):
        return web.Response(text=self.render(PAGE_SETTINGS),
                            content_type="text/html")

    @staticmethod
    def render(page: str) -> str:
        """Fills the shared stylesheet and nav bar into a page template."""
        return page % {"css": BASE_CSS, "nav": NAV}

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
        p = self.bridge.pending_sessions.get(mac)
        sta_ip = p._peer() if p is not None else ""
        # Module detail comes from the module's own web UI: the station
        # interface first, then the access point.
        async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=4)) as cs:
            async def fetch(ip: str):
                r = await cs.get(f"http://{ip}/api/state")
                return await r.json()
            detail = None
            try:
                detail = await fetch(sta_ip)
            except Exception:
                pass
            if detail is None:
                try:
                    ap_state = await fetch(MODULE_AP_IP)
                except Exception:
                    ap_state = None
                sta_ip = str(ap_state.get("sta", "")) if ap_state else ""
                detail = ap_state
                if sta_ip:
                    try:
                        detail = await fetch(sta_ip)
                    except Exception:
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

    # ------------------------------------------------------------------ settings

    async def api_settings(self, request):
        cfg = self.cfg
        return web.json_response({
            "modulePort": cfg.module_port,
            "bridgeBase": cfg.bridge_base,
            "webPort": cfg.web_port,
            "webBind": cfg.web_bind,
            "storage": cfg.storage,
            "modules": cfg.modules,
        })

    async def save_settings(self, request):
        body = await request.json()
        changed = []
        restart = []
        cfg = self.cfg
        mp = body.get("modulePort")
        if mp is not None:
            p = int(mp)
            if not (1 <= p <= 65535):
                return web.json_response({"ok": False,
                                          "error": "module port out of range"},
                                         status=400)
            if p != cfg.module_port:
                try:
                    await self.bridge.rebind_module_port(p)
                except OSError as e:
                    return web.json_response({"ok": False, "error": str(e)},
                                             status=400)
                cfg.module_port = p
                changed.append("modulePort")
        bb = body.get("bridgeBase")
        if bb is not None:
            p = int(bb)
            if not (1 <= p <= 65535):
                return web.json_response({"ok": False,
                                          "error": "bridge base out of range"},
                                         status=400)
            if p != cfg.bridge_base:
                cfg.bridge_base = p
                changed.append("bridgeBase")
        wp = body.get("webPort")
        if wp is not None:
            p = int(wp)
            if not (1 <= p <= 65535):
                return web.json_response({"ok": False,
                                          "error": "web port out of range"},
                                         status=400)
            if p != cfg.web_port:
                cfg.web_port = p
                changed.append("webPort")
                restart.append("web port")
        wb = body.get("webBind")
        if wb is not None:
            s = str(wb).strip()
            if s and s != cfg.web_bind:
                cfg.web_bind = s
                changed.append("webBind")
                restart.append("web bind")
        st = body.get("storage")
        if st is not None:
            s = str(st).strip()
            if s and s != cfg.storage:
                cfg.storage = s
                changed.append("storage")
                restart.append("log storage")
        config.save(cfg)
        return web.json_response({
            "ok": True, "changed": changed, "restart": restart})

    async def api_module_state(self, request):
        slot = request.match_info["slot"]
        m = self.bridge.module_for_viewer(slot)
        if m is None:
            return web.json_response({"ok": False, "error": "no such slot"},
                                     status=404)
        s = m.session
        if s is None:
            return web.json_response({"ok": False, "error": "module offline"},
                                     status=503)
        mac = m.entry["mac"]
        sta_ip = s._peer()
        # Reach the module on its station address first, then its access point.
        async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=4)) as cs:
            async def fetch(ip: str):
                r = await cs.get(f"http://{ip}/api/state")
                return await r.json()
            detail = None
            try:
                detail = await fetch(sta_ip)
            except Exception:
                pass
            if detail is None:
                try:
                    ap_state = await fetch(MODULE_AP_IP)
                except Exception:
                    ap_state = None
                sta_ip = str(ap_state.get("sta", "")) if ap_state else ""
                detail = ap_state
                if sta_ip:
                    try:
                        detail = await fetch(sta_ip)
                    except Exception:
                        detail = ap_state
        if detail is None:
            return web.json_response(
                {"ok": False, "error": "module unreachable"}, status=503)
        detail["slot"] = slot
        detail["mac"] = mac
        return web.json_response(detail)

    async def api_module_save(self, request):
        """Posts a module settings form straight to the module's own web UI."""
        slot = request.match_info["slot"]
        path = request.match_info["path"]
        m = self.bridge.module_for_viewer(slot)
        if m is None:
            return web.json_response({"ok": False, "error": "no such slot"},
                                     status=404)
        s = m.session
        if s is None:
            return web.json_response({"ok": False, "error": "module offline"},
                                     status=503)
        # The station address works when the module is on the LAN; the access
        # point address is the fallback that always answers.
        async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=6)) as cs:
            async def post(ip: str):
                r = await cs.post(f"http://{ip}/api/{path}",
                                  data=await request.post())
                return await r.json()
            try:
                ap_state = await cs.get(f"http://{MODULE_AP_IP}/api/state")
                ap_state = await ap_state.json()
            except Exception:
                ap_state = None
            sta_ip = str(ap_state.get("sta", "")) if ap_state else ""
            try:
                out = await post(sta_ip) if sta_ip else await post(MODULE_AP_IP)
            except Exception as e:
                return web.json_response(
                    {"ok": False, "error": f"module unreachable ({e})"},
                    status=503)
        return web.json_response(out)

    async def log_file(self, request):
        slot = request.match_info["slot"]
        day = request.match_info["day"]
        if "/" in slot or "/" in day or ".." in slot or ".." in day:
            return web.Response(status=400)
        if day.endswith(".log"):
            day = day[:-4]                  # the page links already carry .log
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
    app.router.add_get("/settings", w.settings_page)
    app.router.add_get("/api/state", w.api_state)
    app.router.add_get("/api/pending/{mac}", w.api_pending)
    app.router.add_post("/api/onboard", w.api_onboard)
    app.router.add_get("/api/logs", w.api_logs)
    app.router.add_get("/api/logs/{slot}/{day}", w.log_file)
    app.router.add_get("/live/{slot}", w.live_page)
    app.router.add_get("/ws/{slot}", w.ws)
    app.router.add_get("/api/settings", w.api_settings)
    app.router.add_post("/api/settings", w.save_settings)
    app.router.add_get("/api/module/{slot}/state", w.api_module_state)
    app.router.add_post("/api/module/{slot}/{path:.*}", w.api_module_save)
    return app


# ------------------------------------------------------------------ pages
# Kept as plain strings; the pages are small and the JS is tiny.

NAV = """<nav>
<a href="/">dashboard</a> <a href="/onboard">onboard</a> <a href="/logs">logs</a>
<a href="/settings">settings</a>
</nav>"""

BASE_CSS = """
body{font:14px monospace;background:#111;color:#ddd;margin:16px}
nav{margin:12px 0 20px;padding:6px 0;border-bottom:1px solid #333}
nav a{color:#8cf;margin-right:18px;text-decoration:none}
nav a:hover{color:#adf}
h1{margin:0 0 12px;font-size:18px}
a{color:#8cf}
input{background:#222;color:#ddd;border:1px solid #444;padding:4px;margin-right:6px}
button{background:#333;color:#ddd;border:1px solid #555;padding:4px 10px}
button:hover{background:#444}
select{background:#222;color:#ddd;border:1px solid #444;padding:4px}
.ok{color:#8d8}.err{color:#d88}.off{color:#d88}
td,th{padding:4px 10px;border-bottom:1px solid #333;text-align:left}
table{border-collapse:collapse;margin-top:12px}
label{display:block;margin:10px 0 2px}
.small{font-size:12px;color:#888}
.box{border:1px solid #444;padding:12px;margin:16px 0;background:#181818}
.box h2{margin:0 0 10px;font-size:16px}"""

PAGE_DASH = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host</title>
<style>%(css)s</style></head><body>
%(nav)s
<h1>bell console-server host</h1>
<div class="box"><h2>pending modules</h2>
<p id="pend"></p>
<p id="pendnone" class="small">no pending modules</p>
</div>
<div class="box"><h2>assigned modules</h2>
<table id="t"><tr><th>slot</th><th>MAC</th><th>RW</th><th>RO</th>
<th>module</th><th>ops</th><th>viewers</th><th>module queue</th>
<th>live</th><th>logs</th><th>settings</th></tr></table>
</div>
<script>
async function poll(){
  const s = await (await fetch('/api/state')).json();
  const pend = document.getElementById('pend');
  const pendnone = document.getElementById('pendnone');
  pendnone.style.display = s.pending.length ? 'none' : 'block';
  pend.innerHTML = s.pending.map(p =>
    `<a href="/onboard?mac=${encodeURIComponent(p.mac)}">`
    + `${p.mac}${p.connected ? ' (online)' : ' (offline)'}</a>`).join(' ');
  const t = document.getElementById('t');
  while (t.rows.length > 1) t.deleteRow(1);
  for (const m of s.modules) {
    const r = t.insertRow();
    r.innerHTML = `<td>${m.slot}</td><td>${m.mac}</td><td>${m.rw}</td><td>${m.ro}</td>`
      + `<td class="${m.online ? 'ok' : 'off'}">${m.online ? 'online' : 'offline'}</td>`
      + `<td>${m.operators}</td><td>${m.viewers}</td><td>${m.moduleQueue}</td>`
      + `<td><a href="/live/${encodeURIComponent(m.slot)}">live</a></td>`
      + `<td><a href="/logs?slot=${encodeURIComponent(m.slot)}">logs</a></td>`
      + `<td><a href="/settings#${encodeURIComponent(m.slot)}">settings</a></td>`;
  }
  setTimeout(poll, 2000);
}
poll();
</script></body></html>"""

PAGE_LOGS = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host logs</title>
<style>%(css)s</style></head><body>
%(nav)s
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
    const box = document.createElement('div');
    box.className = 'box';
    const h = document.createElement('h2'); h.textContent = slot; box.appendChild(h);
    for (const f of files) {
      const p = document.createElement('p');
      p.innerHTML = `<a href="/api/logs/${encodeURIComponent(slot)}/${f.file}">${f.file}</a>`
        + ` (${f.size} bytes) `
        + `<a href="/api/logs/${encodeURIComponent(slot)}/${f.file}?download=1">download</a>`;
      box.appendChild(p);
    }
    out.appendChild(box);
  }
}
load();
</script></body></html>"""

PAGE_ONBOARD = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host onboarding</title>
<style>%(css)s</style></head><body>
%(nav)s
<h1>onboarding</h1>
<div id="out"></div>
<script>
const want = new URLSearchParams(location.search).get('mac');
async function load(){
  const r = await fetch('/api/pending/' + encodeURIComponent(want));
  const d = await r.json();
  const out = document.getElementById('out');
  if (!d.reachable) {
    out.innerHTML = `<div class="box"><h2>${want}</h2>`
      + `<p class="${d.connected ? 'ok' : 'err'}">`
      + `${d.connected ? 'connected, ' : 'offline, '}`
      + `${d.reachable ? '' : 'module web unreachable'}</p></div>`;
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
  out.innerHTML = `<div class="box"><h2>${want}</h2>`
    + `<table>${rows.join('')}</table>`
    + `<p><label>slot name</label><input id="name" size="20" placeholder="slot name"> `
    + `<button onclick="assign()">assign</button></p>`
    + `<p id="msg"></p></div>`;
}
async function assign(){
  const n = document.getElementById('name');
  const msg = document.getElementById('msg');
  const r = await fetch('/api/onboard', {method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({mac: want, name: n.value})});
  const j = await r.json();
  msg.className = j.ok ? 'ok' : 'err';
  msg.innerHTML = j.ok ? `${want} assigned as ${j.slot} (rw ${j.rw}, ro ${j.ro}) `
    + `<a href="/live/${encodeURIComponent(j.slot)}">live</a> `
    + `<a href="/settings#${encodeURIComponent(j.slot)}">settings</a>`
                       : j.error;
}
load();
</script></body></html>"""

PAGE_LIVE = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host live - {{SLOT}}</title>
<style>%(css)s</style></head><body>
%(nav)s
<h1>live - {{SLOT}}</h1>
<div class="box">
<p id="mode"></p>
<div id="term"></div>
<p><label>type to the module</label>
<input id="type" placeholder="type to the module">
<button onclick="send()">send</button>
<span id="stat" class="small"></span></p>
</div>
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

PAGE_SETTINGS = """<!doctype html>
<html><head><meta charset="utf-8"><title>bell host settings</title>
<style>%(css)s</style></head><body>
%(nav)s
<h1>settings</h1>
<div class="box" id="host"><h2>host server properties</h2>
<p class="small">Changes apply to the running host where they can; port moves
that need a restart are listed in the answer.</p>
<p><label>module connect port</label><input id="modulePort" size="8"></p>
<p><label>bridge port base</label><input id="bridgeBase" size="8"></p>
<p><label>web port</label><input id="webPort" size="8"></p>
<p><label>web bind address</label><input id="webBind" size="18"></p>
<p><label>log storage directory</label><input id="storage" size="30"></p>
<p><button onclick="save()">save</button> <span id="msg"></span></p>
</div>
<div class="box" id="modules"><h2>module settings</h2>
<p class="small">Sent straight to the module's own web UI.</p>
<div id="out"></div>
</div>
<script>
async function loadHost(){
  const d = await (await fetch('/api/settings')).json();
  document.getElementById('modulePort').value = d.modulePort;
  document.getElementById('bridgeBase').value = d.bridgeBase;
  document.getElementById('webPort').value = d.webPort;
  document.getElementById('webBind').value = d.webBind;
  document.getElementById('storage').value = d.storage;
}
async function save(){
  const body = {
    modulePort: document.getElementById('modulePort').value,
    bridgeBase: document.getElementById('bridgeBase').value,
    webPort: document.getElementById('webPort').value,
    webBind: document.getElementById('webBind').value,
    storage: document.getElementById('storage').value,
  };
  const r = await fetch('/api/settings', {method:'POST',
    headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
  const j = await r.json();
  const msg = document.getElementById('msg');
  msg.className = j.ok ? 'ok' : 'err';
  msg.textContent = j.ok ? ('saved' + (j.restart.length ? ', needs restart: '
                              + j.restart.join(', ') : ''))
                         : j.error;
  if (j.ok) loadHost();
}
async function loadModules(){
  const s = await (await fetch('/api/state')).json();
  const out = document.getElementById('out');
  out.innerHTML = '';
  for (const m of s.modules) {
    const box = document.createElement('div');
    box.className = 'box'; box.id = encodeURIComponent(m.slot);
    box.innerHTML = `<h2>${m.slot} <span class="small">${m.online ? 'online' : 'offline'}</span></h2>`
      + `<p><label>wifi ssid</label><input id="staSsid_${m.slot}" size="24"> `
      + `<label>wifi password</label><input id="staPass_${m.slot}" size="24" type="password"></p>`
      + `<p><label>ap ssid</label><input id="apSsid_${m.slot}" size="24"> `
      + `<label>ap password</label><input id="apPass_${m.slot}" size="24" type="password"></p>`
      + `<p><label>ap channel</label><input id="apChannel_${m.slot}" size="4"> `
      + `<label>ap hidden</label><input id="apHidden_${m.slot}" type="checkbox"></p>`
      + `<p><label>uart baud</label><input id="baud_${m.slot}" size="8"> `
      + `<label>uart parity</label><select id="parity_${m.slot}">`
      + `<option>N</option><option>E</option><option>O</option></select> `
      + `<label>uart bits</label><input id="bits_${m.slot}" size="3"> `
      + `<label>uart stops</label><input id="stops_${m.slot}" size="3"></p>`
      + `<p><label>swap pins</label><input id="swapPins_${m.slot}" type="checkbox"> `
      + `<label>local echo</label><input id="localEcho_${m.slot}" type="checkbox"> `
      + `<label>crlf tx</label><input id="crlfTx_${m.slot}" type="checkbox"> `
      + `<label>log tx</label><input id="logTx_${m.slot}" type="checkbox"></p>`
      + `<p><label>status led</label><input id="statusLed_${m.slot}" type="checkbox"> `
      + `<label>led seconds</label><input id="ledSeconds_${m.slot}" size="6"></p>`
      + `<p><label>device name</label><input id="hostname_${m.slot}" size="20"></p>`
      + `<p><label>host address</label><input id="hostHost_${m.slot}" size="20"> `
      + `<label>host port</label><input id="hostPort_${m.slot}" size="6"></p>`
      + `<p><button onclick="saveNet('${m.slot}')">save net</button> `
      + `<button onclick="saveUart('${m.slot}')">save uart</button> `
      + `<button onclick="reboot('${m.slot}')">reboot</button> `
      + `<span id="msg_${m.slot}"></span></p>`
      + `<p><a href="/live/${encodeURIComponent(m.slot)}">live</a> `
      + `<a href="/logs?slot=${encodeURIComponent(m.slot)}">logs</a></p>`;
    out.appendChild(box);
    if (m.online) {
      const d = await (await fetch(`/api/module/${encodeURIComponent(m.slot)}/state`)).json();
      document.getElementById(`staSsid_${m.slot}`).value = d.net.staSsid || '';
      document.getElementById(`apSsid_${m.slot}`).value = d.net.apSsid || '';
      document.getElementById(`apChannel_${m.slot}`).value = d.net.apChannel;
      document.getElementById(`apHidden_${m.slot}`).checked = !!d.net.apHidden;
      document.getElementById(`baud_${m.slot}`).value = d.uart.baud;
      document.getElementById(`parity_${m.slot}`).value = d.uart.parity;
      document.getElementById(`bits_${m.slot}`).value = d.uart.bits;
      document.getElementById(`stops_${m.slot}`).value = d.uart.stops;
      document.getElementById(`swapPins_${m.slot}`).checked = !!d.uart.swapPins;
      document.getElementById(`localEcho_${m.slot}`).checked = !!d.uart.localEcho;
      document.getElementById(`crlfTx_${m.slot}`).checked = !!d.uart.crlfTx;
      document.getElementById(`logTx_${m.slot}`).checked = !!d.uart.logTx;
      document.getElementById(`statusLed_${m.slot}`).checked = !!d.uart.statusLed;
      document.getElementById(`ledSeconds_${m.slot}`).value = d.uart.ledSeconds;
      document.getElementById(`hostname_${m.slot}`).value = d.hostname || '';
      document.getElementById(`hostHost_${m.slot}`).value = d.net.hostHost || '';
      document.getElementById(`hostPort_${m.slot}`).value = d.net.hostPort;
    }
  }
}
function post(slot, path, body){
  return fetch(`/api/module/${encodeURIComponent(slot)}/${path}`, {method:'POST',
    headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
}
async function saveNet(slot){
  const body = {
    staSsid: document.getElementById(`staSsid_${slot}`).value,
    staPass: document.getElementById(`staPass_${slot}`).value || '*',
    apSsid: document.getElementById(`apSsid_${slot}`).value,
    apPass: document.getElementById(`apPass_${slot}`).value || '*',
    apChannel: document.getElementById(`apChannel_${slot}`).value,
    apHidden: document.getElementById(`apHidden_${slot}`).checked,
    hostname: document.getElementById(`hostname_${slot}`).value,
    hostHost: document.getElementById(`hostHost_${slot}`).value,
    hostPort: document.getElementById(`hostPort_${slot}`).value,
  };
  const j = await (await post(slot, 'net', body)).json();
  const msg = document.getElementById(`msg_${slot}`);
  msg.className = j.ok ? 'ok' : 'err';
  msg.textContent = j.ok ? 'net saved' : j.error;
}
async function saveUart(slot){
  const body = {
    baud: document.getElementById(`baud_${slot}`).value,
    parity: document.getElementById(`parity_${slot}`).value,
    bits: document.getElementById(`bits_${slot}`).value,
    stops: document.getElementById(`stops_${slot}`).value,
    swapPins: document.getElementById(`swapPins_${slot}`).checked,
    localEcho: document.getElementById(`localEcho_${slot}`).checked,
    crlfTx: document.getElementById(`crlfTx_${slot}`).checked,
    logTx: document.getElementById(`logTx_${slot}`).checked,
    statusLed: document.getElementById(`statusLed_${slot}`).checked,
    ledSeconds: document.getElementById(`ledSeconds_${slot}`).value,
  };
  const j = await (await post(slot, 'uart', body)).json();
  const msg = document.getElementById(`msg_${slot}`);
  msg.className = j.ok ? 'ok' : 'err';
  msg.textContent = j.ok ? 'uart saved' : j.error;
}
async function reboot(slot){
  const j = await (await post(slot, 'system/reboot', {})).json();
  const msg = document.getElementById(`msg_${slot}`);
  msg.className = 'ok';
  msg.textContent = 'rebooting';
}
loadHost(); loadModules();
</script></body></html>"""
