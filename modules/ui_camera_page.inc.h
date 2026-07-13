#pragma once

// Camera page template.

static const char MAIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
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
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
header span{font-size:.85em;color:#888}
.main{display:flex;flex-wrap:wrap;gap:12px;padding:12px}
.stream-panel{flex:1 1 400px;text-align:center}
.stream-shell{position:relative;width:100%;max-width:800px;min-height:200px;margin:0 auto;border:2px solid #0f3460;border-radius:6px;background:#111;overflow:hidden}
.stream-panel img{position:absolute;left:0;top:0;width:100%;height:100%;object-fit:contain;background:#111}
.stream-panel img.rot90{transform:rotate(90deg);transform-origin:center center}
.stream-panel img.hidden{display:none}
.stream-placeholder{display:none;position:absolute;inset:0;background:#111827;color:#7dd3fc;align-items:center;justify-content:center;padding:24px;font-size:.95em}
.stream-placeholder.visible{display:flex}
.btn{display:inline-block;margin-top:8px;padding:8px 20px;background:#e94560;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;font-size:.9em}
.btn:hover{background:#c73652}
.btn.flash-on{background:#fbbf24}
.controls{flex:0 1 280px;background:#16213e;border-radius:8px;padding:14px;height:fit-content}
.controls h3{color:#e94560;margin-bottom:12px;font-size:1em}
.cg{margin-bottom:10px}
.cg label{display:block;font-size:.82em;margin-bottom:3px;color:#bbb}
.cg select,.cg input[type=range]{width:100%}
.cg select{background:#0f3460;color:#eee;border:none;border-radius:4px;padding:4px}
.cg .row{display:flex;justify-content:space-between;align-items:center}
@media (max-width:640px){.main{flex-direction:column}.controls{flex:0 1 auto}}
</style>
</head>
<body>
__APP_NAV__
<header>
  <h1>📷 ESP32-CAM</h1>
  <span id="ip_label"></span>
</header>
<div class="main">
  <div class="stream-panel">
    <div id="stream_shell" class="stream-shell">
      <img id="stream" alt="Loading stream...">
      <div id="stream_placeholder" class="stream-placeholder">Stream hidden</div>
    </div>
    <br>
    <button class="btn" id="stream_toggle_btn">🙈 Hide Stream</button>
    <button class="btn" id="stream_open_btn">🎞️ Stream</button>
    <button class="btn" id="snapshot_open_btn">📷 Snapshot</button>
    <button class="btn" id="flash_btn">💡 Flash Off</button>
    <div id="rec_status" style="margin-top:8px;font-size:.85em;color:#7dd3fc"></div>
  </div>
  <div class="controls">
    <h3>Camera Settings</h3>
    <div class="cg">
      <label>Resolution</label>
      <select id="framesize">__FRAME_SIZE_OPTIONS__</select>
    </div>
    <div class="cg">
      <div class="row"><label>Brightness</label><span id="brightness_v">0</span></div>
      <input type="range" id="brightness" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>Contrast</label><span id="contrast_v">0</span></div>
      <input type="range" id="contrast" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>Saturation</label><span id="saturation_v">0</span></div>
      <input type="range" id="saturation" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>JPEG Quality</label><span id="quality_v">12</span></div>
      <input type="range" id="quality" min="4" max="63" value="12">
    </div>
    <div class="cg">
      <label>Special Effect</label>
      <select id="special_effect">
        <option value="0">None</option>
        <option value="1">Negative</option>
        <option value="2">Grayscale</option>
        <option value="3">Red Tint</option>
        <option value="4">Green Tint</option>
        <option value="5">Blue Tint</option>
        <option value="6">Sepia</option>
      </select>
    </div>
    <div class="cg">
      <label>White Balance Mode</label>
      <select id="wb_mode">
        <option value="0">Auto</option>
        <option value="1">Sunny</option>
        <option value="2">Cloudy</option>
        <option value="3">Office</option>
        <option value="4">Home</option>
      </select>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="awb" checked> Auto White Balance</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="aec" checked> Auto Exposure</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="hmirror"> Horizontal Mirror</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="vflip"> Vertical Flip</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="lenc"> Lens Correction</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="view_rotate_90"> Rotate View 90°</label>
    </div>
  </div>
</div>
__APP_FOOTER__
<script>
var streamVisible=true;
var viewRotate90=false;
var streamUrl='http://'+window.location.hostname+':81/stream?t='+encodeURIComponent('__ROUTE_TOKEN__');
var directStreamUrl='http://'+window.location.hostname+':81/stream';
var snapshotUrl='http://'+window.location.hostname+':80/snapshot.jpg';
function id(n){return document.getElementById(n);}
function chk(el){return el.checked?1:0;}
function setStatus(text){
  var status=id('rec_status');
  if(status)status.textContent=text||'';
}
function ctrl(v,val,persist){
  var url='/control?var='+encodeURIComponent(v)+'&val='+encodeURIComponent(val);
  if(persist===false){url+='&persist=0';}
  else{url+='&persist=1';}
  return fetch(url).then(function(r){
    return r.text().then(function(text){
      return {ok:r.ok,text:text||''};
    });
  });
}
function closeStreamConnection(){
  if(!navigator.sendBeacon){
    fetch('/stream/close',{method:'POST',keepalive:true}).catch(function(){});
    return;
  }
  navigator.sendBeacon('/stream/close',new Blob(['close'],{type:'text/plain'}));
}
function releaseStream(notifyServer){
  var img=id('stream');
  if(img.src){
    img.dataset.src=img.dataset.src||img.src;
    img.removeAttribute('src');
  }
  if(notifyServer){closeStreamConnection();}
}
function setStreamVisibility(isVisible){
  var img=id('stream');
  var placeholder=id('stream_placeholder');
  var toggle=id('stream_toggle_btn');
  streamVisible=!!isVisible;
  img.classList.toggle('hidden',!streamVisible);
  placeholder.classList.toggle('visible',!streamVisible);
  toggle.textContent=streamVisible?'🙈 Hide Stream':'👁️ Show Stream';
  if(streamVisible){
    if(!img.dataset.src){img.dataset.src=streamUrl;}
    if(img.src!==img.dataset.src){img.src=img.dataset.src;}
  }else{
    releaseStream(true);
  }
}
function updateStreamLayout(){
  var shell=id('stream_shell');
  var img=id('stream');
  if(!shell||!img)return;
  var w=shell.clientWidth||800;
  var nw=img.naturalWidth||640;
  var nh=img.naturalHeight||480;
  if(nw<=0||nh<=0){nw=640;nh=480;}
  var ratio=viewRotate90?(nw/nh):(nh/nw);
  var h=Math.max(200,Math.round(w*ratio));
  shell.style.height=h+'px';
}
function setViewRotation(isRotated){
  var img=id('stream');
  viewRotate90=!!isRotated;
  img.classList.toggle('rot90',viewRotate90);
  updateStreamLayout();
}
function bindFrameSizeControl(){
  var el=id('framesize');
  if(!el)return;
  el.addEventListener('change',function(){
    var shouldResumeStream=streamVisible;
    setStatus('Applying resolution change...');
    if(shouldResumeStream){releaseStream(true);}
    ctrl('framesize',el.value,true).then(function(result){
      return loadStatus().catch(function(){}).then(function(){
        if(!result.ok){setStatus(result.text||'Failed to change resolution');}
        if(shouldResumeStream){setTimeout(function(){setStreamVisibility(true);},150);}
      });
    }).catch(function(){
      if(shouldResumeStream){setStreamVisibility(true);}
      setStatus('Failed to change resolution');
    });
  });
}
function bindSelectControl(name){
  var el=id(name);
  if(!el)return;
  el.addEventListener('change',function(){ctrl(name,el.value);});
}
function bindRangeControl(name){
  var el=id(name);
  var valueEl=id(name+'_v');
  if(!el)return;
  el.addEventListener('input',function(){
    ctrl(name,el.value,false);
    if(valueEl)valueEl.innerText=el.value;
  });
  el.addEventListener('change',function(){ctrl(name,el.value,true);});
}
function bindCheckboxControl(name){
  var el=id(name);
  if(!el)return;
  el.addEventListener('change',function(){ctrl(name,chk(el));});
}
function applyStatus(s){
  ['framesize','brightness','contrast','saturation','quality','special_effect','wb_mode'].forEach(function(k){
    if(s[k]!==undefined){var e=id(k);if(e)e.value=s[k];var v=id(k+'_v');if(v)v.innerText=s[k];}
  });
  ['awb','aec','hmirror','vflip','lenc'].forEach(function(k){if(s[k]!==undefined){var e=id(k);if(e)e.checked=!!s[k];}});
  if(s.view_rotate_90!==undefined){
    var rotateCb=id('view_rotate_90');
    if(rotateCb)rotateCb.checked=!!s.view_rotate_90;
    setViewRotation(!!s.view_rotate_90);
  }
  setStreamVisibility(s.stream_visible!==undefined?!!s.stream_visible:true);
  if(s.recording_active!==undefined){setStatus(s.recording_active?(s.recording_motion?'Motion recording...':'Recording...'):'');}
}
function loadStatus(){
  return fetch('/status').then(function(r){return r.json();}).then(function(s){
    applyStatus(s);
    return s;
  });
}
bindFrameSizeControl();
['special_effect','wb_mode'].forEach(bindSelectControl);
['brightness','contrast','saturation','quality'].forEach(bindRangeControl);
['awb','aec','hmirror','vflip','lenc','view_rotate_90'].forEach(bindCheckboxControl);
id('view_rotate_90').addEventListener('change',function(){setViewRotation(chk(id('view_rotate_90')));});
id('stream_toggle_btn').addEventListener('click',function(){
  var nextVisible=!streamVisible;
  setStreamVisibility(nextVisible);
  ctrl('stream_visible',nextVisible?1:0,true);
});
id('stream_open_btn').addEventListener('click',function(){
  window.open(directStreamUrl,'_blank','noopener');
});
id('snapshot_open_btn').addEventListener('click',function(){
  window.open(snapshotUrl,'_blank','noopener');
});
id('flash_btn').addEventListener('click',function(){
  var isFlashOn=id('flash_btn').classList.contains('flash-on');
  fetch('/control?var=flash&val='+(isFlashOn?0:1)).then(function(r){
    if(r.ok){
      if(isFlashOn){
        id('flash_btn').classList.remove('flash-on');
        id('flash_btn').textContent='💡 Flash Off';
      }else{
        id('flash_btn').classList.add('flash-on');
        id('flash_btn').textContent='💡 Flash On';
      }
    }
  });
});
var h=window.location.hostname;
id('stream').dataset.src=streamUrl;
id('stream').addEventListener('load',updateStreamLayout);
id('ip_label').innerText=h;
window.addEventListener('resize',updateStreamLayout);
window.addEventListener('pagehide',function(){releaseStream(true);});
window.addEventListener('beforeunload',function(){releaseStream(true);});
setStreamVisibility(false);
updateStreamLayout();
loadStatus().catch(function(){
  setStreamVisibility(true);
});
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────
