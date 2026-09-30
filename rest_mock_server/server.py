#!/usr/bin/env python3
"""REST test backend - pairs with the REST backend of the esp32 project
AWS_mqtt_REST_API.

Device behaviour (see main/rest_api.c):
  POST <host>:<port><rest_path>      report JSON:
      {"device","version","temperature","humidity","rssi","uptime","lamp"}
  GET  <host>:<port><rest_cmd_path>  poll for a lamp command, expects:
      {"lamp":"ON"} / {"lamp":"OFF"} or 204 No Content

This service additionally serves a browser dashboard:
  GET  /               dashboard (live values + history chart + lamp switch)
  GET  /api/latest     newest report
  GET  /api/history    most recent N reports (200 by default)
  POST /api/lamp       set the lamp manually (effective on the next poll)
  GET  /api/auto       ON/OFF (hand lamp commands to auto mode)
"""
import json
import os
import sqlite3
import threading
import time
from datetime import datetime, timezone

from flask import Flask, Response, g, jsonify, request

# The database defaults to the script directory, so it just runs on Windows
# with no volume to mount.
DB_PATH = os.environ.get(
    "DB_PATH", os.path.join(os.path.dirname(os.path.abspath(__file__)), "telemetry.db")
)
# Report endpoint; must match APP_REST_REPORT_PATH in the firmware
REPORT_PATH = os.environ.get("REPORT_PATH", "/report")
# Lamp command poll endpoint; must match APP_REST_CMD_PATH in the firmware
CMD_PATH = os.environ.get("CMD_PATH", "/lampcmd")

app = Flask(__name__)
_lock = threading.Lock()

# Lamp command currently headed for the device. None means 204 (no command,
# the device leaves the lamp as it is).
_lamp_cmd = None


# ----------------------------------------------------------------------
# Database
# ----------------------------------------------------------------------
def db():
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH, timeout=10)
        g.db.row_factory = sqlite3.Row
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS reports (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          REAL    NOT NULL,
            device      TEXT,
            version     TEXT,
            temperature REAL,
            humidity    REAL,
            rssi        INTEGER,
            uptime      INTEGER,
            lamp        TEXT
        )
        """
    )
    conn.execute("CREATE INDEX IF NOT EXISTS idx_reports_ts ON reports(ts)")
    conn.commit()
    conn.close()


def num(v, cast=float):
    try:
        return cast(v)
    except (TypeError, ValueError):
        return None


# ----------------------------------------------------------------------
# Device-facing endpoints
# ----------------------------------------------------------------------
@app.route(REPORT_PATH, methods=["POST"])
def report():
    """Receive a device report. The firmware only looks at the 2xx status."""
    data = request.get_json(silent=True)
    if data is None:
        data = {}
    ts = time.time()
    row = (
        ts,
        str(data.get("device", "")),
        str(data.get("version", "")),
        num(data.get("temperature")),
        num(data.get("humidity")),
        num(data.get("rssi"), int),
        num(data.get("uptime"), int),
        str(data.get("lamp", "")),
    )
    conn = db()
    conn.execute(
        "INSERT INTO reports (ts,device,version,temperature,humidity,rssi,uptime,lamp)"
        " VALUES (?,?,?,?,?,?,?,?)",
        row,
    )
    conn.commit()
    stamp = datetime.fromtimestamp(ts).strftime("%H:%M:%S")
    print(
        f"[{stamp}] REPORT {row[1]} v{row[2]} "
        f"T={row[3]} H={row[4]} rssi={row[5]} lamp={row[7]}",
        flush=True,
    )
    return jsonify({"ok": True}), 200


@app.route(CMD_PATH, methods=["GET"])
def lamp_cmd():
    """Device polls for a lamp command. Returns 204 when there is none."""
    global _lamp_cmd
    with _lock:
        cmd = _lamp_cmd
        if cmd is not None:
            _lamp_cmd = None  # deliver once and clear, so the device does not
                              # keep receiving the same command every poll
    if cmd is None:
        return Response(status=204)
    print(f"[{datetime.now():%H:%M:%S}] CMD   -> lamp={cmd}", flush=True)
    return jsonify({"lamp": cmd}), 200


# ----------------------------------------------------------------------
# Dashboard API
# ----------------------------------------------------------------------
@app.route("/api/latest")
def api_latest():
    row = db().execute("SELECT * FROM reports ORDER BY ts DESC LIMIT 1").fetchone()
    return jsonify(dict(row) if row else {})


@app.route("/api/history")
def api_history():
    limit = min(int(request.args.get("limit", 200)), 5000)
    rows = db().execute(
        "SELECT * FROM reports ORDER BY ts DESC LIMIT ?", (limit,)
    ).fetchall()
    return jsonify([dict(r) for r in reversed(rows)])


@app.route("/api/lamp", methods=["POST"])
def api_set_lamp():
    global _lamp_cmd
    state = str((request.get_json(silent=True) or {}).get("state", "")).upper()
    if state not in ("ON", "OFF", "NONE"):
        return jsonify({"error": "state must be ON / OFF / NONE"}), 400
    with _lock:
        _lamp_cmd = None if state == "NONE" else state
    return jsonify({"queued": state})


@app.route("/api/stats")
def api_stats():
    conn = db()
    n, = conn.execute("SELECT COUNT(*) FROM reports").fetchone()
    first, = conn.execute("SELECT MIN(ts) FROM reports").fetchone()
    last, = conn.execute("SELECT MAX(ts) FROM reports").fetchone()
    return jsonify(
        {
            "count": n,
            "since": first,
            "last": last,
            "age_s": (time.time() - last) if last else None,
        }
    )


@app.route("/api/clear", methods=["POST"])
def api_clear():
    conn = db()
    conn.execute("DELETE FROM reports")
    conn.commit()
    return jsonify({"ok": True})


DASHBOARD = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 REST Server</title>
<style>
  :root{--bg:#0f1115;--card:#181b22;--line:#272b35;--fg:#e6e8ee;--mut:#8b93a7;--acc:#4f8cff;--ok:#39d98a;--warn:#ffb648}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif}
  header{padding:16px 22px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:14px;flex-wrap:wrap}
  header h1{font-size:16px;margin:0;font-weight:600}
  .dot{width:9px;height:9px;border-radius:50%;background:#555;display:inline-block}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .dot.off{background:#e5484d}
  .mut{color:var(--mut);font-size:12px}
  main{padding:22px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));max-width:1400px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
  .card h2{margin:0 0 12px;font-size:13px;color:var(--mut);font-weight:600;letter-spacing:.4px;text-transform:uppercase}
  .big{font-size:30px;font-weight:600;font-variant-numeric:tabular-nums}
  .big small{font-size:14px;color:var(--mut);font-weight:400;margin-left:4px}
  .kv{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px dashed var(--line)}
  .kv:last-child{border-bottom:none}
  .kv span:first-child{color:var(--mut)}
  .row{display:flex;gap:10px;flex-wrap:wrap}
  button{flex:1;min-width:90px;padding:11px;border-radius:8px;border:1px solid var(--line);
         background:#20242e;color:var(--fg);font-size:14px;cursor:pointer;transition:.15s}
  button:hover{background:#2a2f3b;border-color:#3a4150}
  button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
  button.pri:hover{background:#3d7bf0}
  button.on{background:var(--ok);border-color:var(--ok);color:#04220f;font-weight:600}
  button.dan{color:#ff9a9a}
  #chart{width:100%;height:180px;display:block}
  .legend{display:flex;gap:16px;font-size:12px;color:var(--mut);margin-top:6px}
  .sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:middle}
  table{width:100%;border-collapse:collapse;font-size:12px;font-variant-numeric:tabular-nums}
  th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
  th{color:var(--mut);font-weight:500;position:sticky;top:0;background:var(--card)}
  .scroll{max-height:320px;overflow:auto}
  .grid-full{grid-column:1/-1}
</style>
</head>
<body>
<header>
  <h1>ESP32 REST Server</h1>
  <span class="dot" id="dot"></span>
  <span class="mut" id="link">Waiting for device...</span>
  <span class="mut" id="stats" style="margin-left:auto"></span>
</header>

<main>
  <div class="card">
    <h2>Temperature</h2>
    <div class="big" id="temp">--<small>°C</small></div>
  </div>
  <div class="card">
    <h2>Humidity</h2>
    <div class="big" id="hum">--<small>%RH</small></div>
  </div>
  <div class="card">
    <h2>Device</h2>
    <div class="kv"><span>Device ID</span><b id="dev">--</b></div>
    <div class="kv"><span>Firmware</span><b id="ver">--</b></div>
    <div class="kv"><span>RSSI</span><b id="rssi">--</b></div>
    <div class="kv"><span>Uptime</span><b id="up">--</b></div>
    <div class="kv"><span>Reported lamp</span><b id="replamp">--</b></div>
  </div>
  <div class="card">
    <h2>Lamp Control</h2>
    <div class="row">
      <button id="bON" class="pri" onclick="setLamp('ON')">Turn On</button>
      <button id="bOFF" class="pri" onclick="setLamp('OFF')">Turn Off</button>
      <button onclick="setLamp('NONE')">Cancel</button>
    </div>
    <p class="mut" style="margin:12px 0 0">
      Commands are queued once and cleared as soon as the device picks them up
      on its next poll (1s by default).
    </p>
    <div class="kv" style="margin-top:10px"><span>Queued command</span><b id="queued">none</b></div>
  </div>

  <div class="card grid-full">
    <h2>History (last 120 points)</h2>
    <canvas id="chart"></canvas>
    <div class="legend">
      <span><i class="sw" style="background:#ff7043"></i>Temperature °C</span>
      <span><i class="sw" style="background:#4f8cff"></i>Humidity %RH</span>
    </div>
  </div>

  <div class="card grid-full">
    <h2>Recent Reports</h2>
    <div class="scroll">
      <table>
        <thead><tr><th>Time</th><th>Temp</th><th>Humidity</th><th>RSSI</th><th>Lamp</th><th>Version</th></tr></thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>
    <div class="row" style="margin-top:12px">
      <button class="dan" onclick="clearAll()">Clear history</button>
    </div>
  </div>
</main>

<script>
const $ = id => document.getElementById(id);
let queued = null;

function fmtUptime(s){
  if(s==null) return '--';
  const d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+(h?h+'h ':'')+m+'m';
}

function setLamp(state){
  fetch('/api/lamp',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({state})}).then(r=>r.json()).then(j=>{
      queued = state==='NONE' ? null : state;
      $('queued').textContent = queued ?? 'none';
      toast(state==='NONE'?'Queued command cancelled':'Queued '+state+', waiting for device');
    });
}

function toast(msg){
  const el=$('stats');
  const bak=el.dataset.msg||'';
  el.dataset.msg=msg;
  el.textContent=msg;
  clearTimeout(window._t);
  window._t=setTimeout(()=>{el.textContent=bak;},2500);
}

function clearAll(){
  if(!confirm('Clear all history records?')) return;
  fetch('/api/clear',{method:'POST'}).then(()=>refresh(true));
}

const hist=[];

function refresh(full){
  fetch('/api/history?limit=200').then(r=>r.json()).then(rows=>{
    if(rows.length){
      const d=rows[rows.length-1];
      const fresh = !window._lastTs || d.ts!==window._lastTs;
      window._lastTs = d.ts;

      $('temp').innerHTML=(d.temperature??'--')+'<small>°C</small>';
      $('hum').innerHTML=(d.humidity??'--')+'<small>%RH</small>';
      $('dev').textContent=d.device||'--';
      $('ver').textContent=d.version||'--';
      $('rssi').textContent=d.rssi!=null?d.rssi+' dBm':'--';
      $('up').textContent=fmtUptime(d.uptime);
      $('replamp').textContent=d.lamp||'--';

      const age=Date.now()/1000-d.ts;
      const online=age<10;
      $('dot').className='dot '+(online?'on':'off');
      $('link').textContent=online
        ? 'Online · last report '+age.toFixed(0)+'s ago'
        : 'Offline · last report '+fmtUptime(age)+' ago';

      if(fresh){
        // The reported lamp state matches the queued command -> it was consumed
        if(queued && (d.lamp===queued)){
          queued=null; $('queued').textContent='none';
        }
      }
    }
    if(full || hist.length===0 || rows.length){
      hist.length=0; rows.forEach(r=>hist.push(r));
      $('tbody').innerHTML = rows.slice(-60).reverse().map(r=>{
        const t=new Date(r.ts*1000).toLocaleTimeString('en-GB',{hour12:false});
        return `<tr><td>${t}</td><td>${r.temperature??'--'}</td>
                <td>${r.humidity??'--'}</td><td>${r.rssi??'--'}</td>
                <td>${r.lamp||'--'}</td><td>${r.version||'--'}</td></tr>`;
      }).join('');
      draw();
    }
  });

  fetch('/api/stats').then(r=>r.json()).then(s=>{
    if(!$('stats').dataset.msg){
      $('stats').textContent = s.count?`${s.count} records`:'';
    }
  });
}

function draw(){
  const c=$('chart'), ctx=c.getContext('2d');
  const dpr=window.devicePixelRatio||1;
  const w=c.clientWidth, h=180;
  c.width=w*dpr; c.height=h*dpr; ctx.scale(dpr,dpr);

  const data=hist.slice(-120);
  ctx.clearRect(0,0,w,h);
  if(data.length<2) return;

  const T=data.map(d=>d.temperature).filter(v=>v!=null);
  const H=data.map(d=>d.humidity).filter(v=>v!=null);
  const all=T.concat(H);
  let lo=Math.min(...all), hi=Math.max(...all);
  const pad=Math.max(1,(hi-lo)*0.15); lo-=pad; hi+=pad;

  const X=i=>10+(w-20)*i/(data.length-1);
  const Y=v=>h-16-(h-32)*(v-lo)/(hi-lo||1);

  // Grid
  ctx.strokeStyle='#272b35'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const y=12+(h-28)*i/4;
    ctx.beginPath(); ctx.moveTo(10,y); ctx.lineTo(w-10,y); ctx.stroke();
    ctx.fillStyle='#8b93a7'; ctx.font='10px sans-serif';
    ctx.fillText((hi-(hi-lo)*i/4).toFixed(1), 12, y-3);
  }

  const line=(key,color)=>{
    ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath();
    let started=false;
    data.forEach((d,i)=>{
      const v=d[key];
      if(v==null){started=false;return;}
      const x=X(i), y=Y(v);
      started?ctx.lineTo(x,y):ctx.moveTo(x,y);
      started=true;
    });
    ctx.stroke();
  };
  line('temperature','#ff7043');
  line('humidity','#4f8cff');
}

window.addEventListener('resize',draw);
refresh(true);
setInterval(refresh,2000);
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return Response(DASHBOARD, mimetype="text/html")


if __name__ == "__main__":
    init_db()
    port = int(os.environ.get("PORT", 8080))
    print(f"REST mock server listening on :{port}", flush=True)
    print(f"  device report:  POST http://<host-ip>:{port}{REPORT_PATH}", flush=True)
    print(f"  lamp poll:      GET  http://<host-ip>:{port}{CMD_PATH}", flush=True)
    print(f"  dashboard:      http://localhost:{port}/", flush=True)
    app.run(host="0.0.0.0", port=port, threaded=True)
