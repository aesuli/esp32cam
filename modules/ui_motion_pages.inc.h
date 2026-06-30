#pragma once

// Motion settings and motion graph page templates.

static const char MOTION_HTML[] PROGMEM = R"html(<!DOCTYPE html>
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
h3{color:#7dd3fc;margin-bottom:10px}
.cg{margin-bottom:12px}
.cg label{display:block;font-size:.9em;color:#bbb;margin-bottom:6px}
input[type=range],select{width:100%}
select,input{background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px;padding:8px}
.row{display:grid;grid-template-columns:1fr 120px;gap:10px;align-items:center}
.small{font-size:.82em;color:#9fb3d1;margin-top:4px}
.status{min-height:20px;margin-top:10px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.btn{background:#e94560;color:#fff;border:none;border-radius:4px;padding:9px 14px;cursor:pointer}
.btn:hover{background:#c73652}
</style>
</head>
<body>
__APP_NAV__
<header><h1>🚶 Motion Detection</h1></header>
<div class="wrap">
  <div class="panel">
    <h3>Settings</h3>
    <div class="cg"><a href="/motion/graph" style="color:#7dd3fc;text-decoration:none">Open Motion Graph</a></div>
    <div class="cg"><label><input id="enabled" type="checkbox"> Enable motion detection</label></div>
    <div class="small">After enabling, motion detection arms after a fixed 10-second delay.</div>

    <div class="cg"><label><input id="capture_image" type="checkbox"> Capture image(s) on motion</label></div>
    <div class="cg row">
      <label for="image_count">Number of images (1-10)</label>
      <input id="image_count" type="number" min="1" max="10" step="1">
    </div>
    <div class="cg row">
      <label for="image_delay_ds">Delay between images (0.1-2.0 sec)</label>
      <input id="image_delay_ds" type="number" min="0.1" max="2.0" step="0.1">
    </div>

    <div class="cg"><label><input id="capture_video" type="checkbox"> Capture video on motion</label></div>
    <div class="cg row">
      <label for="video_duration_sec">Video duration (1-30 sec)</label>
      <input id="video_duration_sec" type="number" min="1" max="30" step="1">
    </div>
    <div class="cg"><label><input id="flash_on_capture" type="checkbox"> Turn on flash while capturing motion media</label></div>

    <div class="cg row">
      <label for="detection_interval_sec">Interval between detections</label>
      <select id="detection_interval_sec">
        <option value="0">Soon after capture</option>
        <option value="5">+5 seconds</option>
        <option value="10">+10 seconds</option>
        <option value="30">+30 seconds</option>
        <option value="60">+1 minute</option>
        <option value="600">+10 minutes</option>
      </select>
    </div>
    <div class="small">This cooldown applies while the device is running.</div>

    <div class="cg row">
      <label for="notify_url">Notify URL (GET on motion detect)</label>
      <input id="notify_url" type="url" placeholder="http://example.local/motion">
    </div>
    <div class="cg"><label><input id="notify_enabled" type="checkbox"> Enable notify URL request</label></div>

    <div class="small">Changes are saved automatically when you modify a setting.</div>
    <div class="status" id="status"></div>
  </div>
</div>
__APP_FOOTER__
<script>
function id(n){return document.getElementById(n);}
function setStatus(msg,err){var e=id('status');e.textContent=msg||'';e.className=err?'status error':'status';}
function asInt(v,d){var n=parseInt(v,10);return isNaN(n)?d:n;}
function formData(obj){return Object.keys(obj).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(obj[k]);}).join('&');}
var saveTimer=0;
var savePending=false;
var saveInFlight=false;

function buildPayload(){
  return {
    enabled:id('enabled').checked?1:0,
    captureImage:id('capture_image').checked?1:0,
    imageCount:asInt(id('image_count').value,1),
    imageDelayDs:Math.round((parseFloat(id('image_delay_ds').value)||0.1)*10),
    captureVideo:id('capture_video').checked?1:0,
    flashOnCapture:id('flash_on_capture').checked?1:0,
    videoDurationSec:asInt(id('video_duration_sec').value,5),
    detectionIntervalSec:asInt(id('detection_interval_sec').value,0),
    notifyUrl:id('notify_url').value||'',
    notifyEnabled:id('notify_enabled').checked?1:0
  };
}

function saveConfig(){
  if(saveInFlight){
    savePending=true;
    return;
  }
  saveInFlight=true;
  setStatus('Saving...',false);
  fetch('/motion/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData(buildPayload())})
    .then(function(r){
      return r.text().then(function(t){
        setStatus(t||'Saved',!r.ok);
        if(r.ok){loadConfig();}
      });
    })
    .catch(function(e){setStatus(e.message,true);})
    .finally(function(){
      saveInFlight=false;
      if(savePending){
        savePending=false;
        saveConfig();
      }
    });
}

function scheduleSave(){
  if(saveTimer){clearTimeout(saveTimer);}
  saveTimer=setTimeout(function(){
    saveTimer=0;
    saveConfig();
  },180);
}

function bindAutoSave(controlId){
  var el=id(controlId);
  el.addEventListener('change',scheduleSave);
}

function loadConfig(){
  fetch('/motion/config').then(function(r){
    if(!r.ok){throw new Error('Failed to load motion config');}
    return r.json();
  }).then(function(c){
    id('enabled').checked=!!c.enabled;
    id('capture_image').checked=!!c.captureImage;
    id('image_count').value=c.imageCount;
    id('image_delay_ds').value=((c.imageDelayDs||1)/10).toFixed(1);
    id('capture_video').checked=!!c.captureVideo;
    id('flash_on_capture').checked=!!c.flashOnCapture;
    id('video_duration_sec').value=c.videoDurationSec;
    id('detection_interval_sec').value=String(c.detectionIntervalSec||0);
    id('notify_url').value=c.notifyUrl||'';
    id('notify_enabled').checked=!!c.notifyEnabled;
  }).catch(function(e){setStatus(e.message,true);});
}

bindAutoSave('enabled');
bindAutoSave('capture_image');
bindAutoSave('image_count');
bindAutoSave('image_delay_ds');
bindAutoSave('capture_video');
bindAutoSave('flash_on_capture');
bindAutoSave('video_duration_sec');
bindAutoSave('detection_interval_sec');
bindAutoSave('notify_url');
bindAutoSave('notify_enabled');
loadConfig();
</script>
</body>
</html>)html";

static const char MOTION_GRAPH_HTML[] PROGMEM = R"html(<!DOCTYPE html>
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
header h1{color:#e94560;font-size:1.2em}
.wrap{max-width:920px;margin:0 auto;padding:14px}
.panel{background:#16213e;border-radius:8px;padding:14px}
#graph{width:100%;height:260px;border:1px solid #234573;border-radius:6px;background:#0e1b3a}
.meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px;margin-top:10px;font-size:.88em;color:#b9c7dd}
.status{min-height:20px;margin-top:8px;color:#7dd3fc;font-size:.9em}
.status.error{color:#ff8a8a}
</style>
</head>
<body>
__APP_NAV__
<header><h1>🚶 Motion Graph (Live PIR Readings)</h1></header>
<div class="wrap">
  <div class="panel">
    <canvas id="graph"></canvas>
    <div class="meta">
      <div>Raw: <span id="raw">-</span></div>
      <div>Latched: <span id="latched">-</span></div>
      <div>Edge Count: <span id="edgecount">-</span></div>
      <div>Signal: <span id="signal">-</span></div>
      <div>High(ms): <span id="highms">-</span></div>
      <div>Last Trigger Ago(ms): <span id="lastms">-</span></div>
    </div>
    <div class="status" id="status"></div>
  </div>
</div>
__APP_FOOTER__
<script>
var points=[];
var maxPoints=180;
var lastFetchOk=true;
var lastEdgeCount=0;
var edgePulseFrames=0;
function id(n){return document.getElementById(n);}
function setStatus(msg,err){var e=id('status');e.textContent=msg||'';e.className=err?'status error':'status';}
function draw(){
  var c=id('graph');
  var ctx=c.getContext('2d');
  var w=c.clientWidth,h=c.clientHeight;
  if(c.width!==w||c.height!==h){c.width=w;c.height=h;}
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#234573';
  for(var i=0;i<=5;i++){var y=(h/5)*i;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke();}
  if(points.length<2){return;}
  ctx.strokeStyle='#7dd3fc';
  ctx.lineWidth=2;
  ctx.beginPath();
  for(var j=0;j<points.length;j++){
    var x=(j/(maxPoints-1))*w;
    var y=h-(points[j]/100)*h;
    if(j===0)ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.stroke();
}
function poll(){
  fetch('/motion/readings').then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();}).then(function(m){
    id('raw').textContent=m.rawHigh?'HIGH':'LOW';
    id('latched').textContent=m.latched?'YES':'NO';
    id('edgecount').textContent=String(m.edgeCount||0);
    id('signal').textContent=String(m.signal);
    id('highms').textContent=String(m.highDurationMs);
    id('lastms').textContent=String(m.sinceLastDetectedMs);
    var edgeCount=Number(m.edgeCount||0);
    if(edgeCount>lastEdgeCount){
      edgePulseFrames=4;
    }
    lastEdgeCount=edgeCount;
    var plottedSignal=Math.max(0,Math.min(100,m.signal||0));
    if(edgePulseFrames>0){
      plottedSignal=Math.max(plottedSignal,95);
      edgePulseFrames--;
    }
    points.push(plottedSignal);
    if(points.length>maxPoints)points.shift();
    draw();
    if(!lastFetchOk){setStatus('Connection restored',false);} else {setStatus('',false);}
    lastFetchOk=true;
  }).catch(function(err){
    lastFetchOk=false;
    setStatus('Failed to fetch motion readings: '+(err.message||'network error'),true);
  });
}
setInterval(poll,250);
window.addEventListener('resize',draw);
poll();
</script>
</body>
</html>)html";
