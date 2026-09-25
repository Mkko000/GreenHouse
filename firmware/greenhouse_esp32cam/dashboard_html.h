// Dashboard page served at "/" by greenhouse_esp32cam.ino.
// Kept in its own header: raw string literals inside an .ino file confuse the
// Arduino IDE's automatic prototype generation.
#pragma once
#include <pgmspace.h>

// Single-page dashboard: polls /status every 2 s and refreshes the camera
// snapshot every 3 s, without reloading the page.
const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Greenhouse</title>
<style>
body{font-family:Arial,sans-serif;background:#0d1b0d;color:#eee;margin:0;padding:16px}
h1{color:#4caf50;text-align:center;margin:8px 0}
#state{text-align:center;color:#9e9e9e;margin-bottom:12px}
.grid{display:flex;flex-wrap:wrap;justify-content:center;gap:12px;margin:12px 0}
.card{background:#1b2e1b;border:1px solid #2e7d32;border-radius:12px;padding:16px;min-width:130px;text-align:center}
.val{font-size:1.9em;color:#69f0ae;font-weight:bold}
.row{text-align:center;margin:6px 0}
button{background:#2e7d32;color:#fff;border:none;padding:10px 16px;border-radius:8px;margin:4px;font-size:1em;cursor:pointer}
button.on{background:#f9a825;color:#111}
button.auto{background:#006064}
img{display:block;width:100%;max-width:640px;margin:16px auto;border-radius:12px}
</style></head><body>
<h1>&#127807; Smart Greenhouse</h1>
<div id="state">Connecting...</div>
<div class="grid">
 <div class="card"><div class="val" id="t">--</div>Temperature</div>
 <div class="card"><div class="val" id="h">--</div>Humidity</div>
 <div class="card"><div class="val" id="s">--</div>Soil moisture</div>
 <div class="card"><div class="val" id="l">--</div>Light</div>
</div>
<div class="row"><button id="pump" onclick="toggle('PUMP','pump')">Pump</button>
<button id="fan" onclick="toggle('FAN','fan')">Fans</button>
<button id="leds" onclick="toggle('LED','leds')">Lights</button></div>
<div class="row"><button class="auto" onclick="send('AUTO')">Auto mode</button></div>
<img id="cam" alt="Camera">
<script>
let last={};
function fmt(v,u){return v===null||v===undefined?'--':v+u}
async function refresh(){
 try{
  const r=await fetch('/status');last=await r.json();
  document.getElementById('t').textContent=fmt(last.temperature,'°C');
  document.getElementById('h').textContent=fmt(last.humidity,'%');
  document.getElementById('s').textContent=fmt(last.soil,'%');
  document.getElementById('l').textContent=fmt(last.light,'');
  for(const k of ['pump','fan','leds'])document.getElementById(k).classList.toggle('on',last[k]);
  document.getElementById('state').textContent=!last.online?'Arduino offline - check the UART wiring'
   :(last.auto?'AUTO mode':'MANUAL mode')+(last.camera?'':' - camera not available');
 }catch(e){document.getElementById('state').textContent='Dashboard offline';}
}
async function send(cmd){await fetch('/ctrl?cmd='+cmd);setTimeout(refresh,300);}
function toggle(name,key){send(name+(last[key]?'_OFF':'_ON'));}
function snap(){if(last.camera!==false)document.getElementById('cam').src='/capture?t='+Date.now();}
refresh();snap();setInterval(refresh,2000);setInterval(snap,3000);
</script></body></html>)HTML";
