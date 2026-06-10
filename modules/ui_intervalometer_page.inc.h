#pragma once

// Intervalometer/timelapse page template.

static const char INTERVALOMETER_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__PAGE_TITLE__</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px}
header h1{color:#e94560;font-size:1.3em}
.wrap{max-width:760px;margin:0 auto;padding:14px}
.panel{background:#16213e;border-radius:8px;padding:14px}
.cg{margin-bottom:12px}
.cg label{display:block;font-size:.9em;color:#bbb;margin-bottom:6px}
.row{display:grid;grid-template-columns:1fr 180px;gap:10px;align-items:center}
input,select{width:100%;background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px;padding:8px}
.status{min-height:22px;margin-top:12px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.small{font-size:.83em;color:#9fb3d1;margin-top:6px}
.btn{background:#e94560;color:#fff;border:none;border-radius:4px;padding:10px 14px;cursor:pointer}
.btn:hover{background:#c73652}
.btn:disabled{opacity:.6;cursor:not-allowed}
.actions{display:flex;gap:10px;flex-wrap:wrap}
</style>
</head>
<body>
__APP_NAV__
<header><h1>⏱ Intervalometer</h1></header>
<div class="wrap">
  <div class="panel">
    <div class="cg row">
      <label for="interval_value">Interval value (1-100000)</label>
      <input id="interval_value" type="number" min="1" max="100000" step="1" value="60">
    </div>
    <div class="cg row">
      <label for="interval_unit">Interval unit</label>
      <select id="interval_unit">
        <option value="0">Seconds</option>
        <option value="1">Minutes</option>
        <option value="2">Hours</option>
        <option value="3">Days</option>
      </select>
    </div>
    <div class="cg row">
      <label for="burst_count">Shots per interval (1-10)</label>
      <input id="burst_count" type="number" min="1" max="10" step="1" value="1">
    </div>
    <div class="cg row">
      <label for="burst_delay_sec">Delay between burst shots (seconds, 1-100)</label>
      <input id="burst_delay_sec" type="number" min="1" max="100" step="1" value="1">
    </div>
    <div class="cg row">
      <label for="continue_after_power_loss">Continue after power loss</label>
      <select id="continue_after_power_loss">
        <option value="0" selected>No</option>
        <option value="1">Yes</option>
      </select>
    </div>
    <div class="small">When started, the camera takes the first burst immediately, then sleeps and wakes by timer. WiFi and motion functions are disabled until power cycle.</div>
    <div class="small">If continue-after-power-loss is enabled, timelapse resumes after boot unless RX button (GPIO3) is held during power-on.</div>
    <div class="actions" style="margin-top:12px">
      <button id="start_btn" class="btn" type="button">Start Timelapse</button>
    </div>
    <div id="status" class="status"></div>
  </div>
</div>
__APP_FOOTER__
<script>
function id(n){return document.getElementById(n);} 
function setStatus(msg,err){var e=id('status');e.textContent=msg||'';e.className=err?'status error':'status';}
function asInt(v,d){var n=parseInt(v,10);return isNaN(n)?d:n;}
function clamp(v,min,max){if(v<min)return min;if(v>max)return max;return v;}
function payload(){
  return {
    intervalValue:clamp(asInt(id('interval_value').value,60),1,100000),
    intervalUnit:clamp(asInt(id('interval_unit').value,0),0,3),
    burstCount:clamp(asInt(id('burst_count').value,1),1,10),
    burstDelaySec:clamp(asInt(id('burst_delay_sec').value,1),1,100),
    continueAfterPowerLoss:asInt(id('continue_after_power_loss').value,0)?1:0
  };
}
function encodeForm(obj){
  return Object.keys(obj).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(obj[k]);}).join('&');
}
function fill(c){
  id('interval_value').value=c.intervalValue||60;
  id('interval_unit').value=String(c.intervalUnit||0);
  id('burst_count').value=c.burstCount||1;
  id('burst_delay_sec').value=c.burstDelaySec||1;
  id('continue_after_power_loss').value=c.continueAfterPowerLoss?'1':'0';
}
var saveTimer=0;
var savePending=false;
var saveInFlight=false;
function loadCfg(){
  fetch('/intervalometer/config').then(function(r){
    if(!r.ok){throw new Error('Failed to load intervalometer config');}
    return r.json();
  }).then(function(c){
    fill(c);
  }).catch(function(e){setStatus(e.message,true);});
}
function saveCfg(){
  if(saveInFlight){
    savePending=true;
    return;
  }
  saveInFlight=true;
  var p=payload();
  id('interval_value').value=p.intervalValue;
  id('interval_unit').value=String(p.intervalUnit);
  id('burst_count').value=p.burstCount;
  id('burst_delay_sec').value=p.burstDelaySec;
  setStatus('Saving...',false);
  fetch('/intervalometer/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:encodeForm(p)})
    .then(function(r){
      return r.text().then(function(t){
        setStatus(t||'Saved',!r.ok);
      });
    })
    .catch(function(e){setStatus(e.message,true);})
    .finally(function(){
      saveInFlight=false;
      if(savePending){
        savePending=false;
        saveCfg();
      }
    });
}
function scheduleSave(){
  if(saveTimer){clearTimeout(saveTimer);}
  saveTimer=setTimeout(function(){
    saveTimer=0;
    saveCfg();
  },180);
}
function startTimelapse(){
  if(!confirm('Start intervalometer now? Device will stop other services until a power cycle.')){
    return;
  }
  var p=payload();
  id('start_btn').disabled=true;
  setStatus('Starting intervalometer...',false);
  fetch('/intervalometer/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:encodeForm(p)})
    .then(function(r){
      return r.text().then(function(t){
        setStatus(t||'Intervalometer started',!r.ok);
      });
    })
    .catch(function(e){
      setStatus(e.message,true);
      id('start_btn').disabled=false;
    });
}
id('interval_value').addEventListener('change',scheduleSave);
id('interval_unit').addEventListener('change',scheduleSave);
id('burst_count').addEventListener('change',scheduleSave);
id('burst_delay_sec').addEventListener('change',scheduleSave);
id('continue_after_power_loss').addEventListener('change',scheduleSave);
id('start_btn').addEventListener('click',startTimelapse);
loadCfg();
</script>
</body>
</html>)html";
