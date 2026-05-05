#pragma once

// PROGMEM HTML templates for the web UI. Keeping these here lets
// esp32cam.cpp focus on firmware behavior and route handling.

// ─── HTML pages (stored in flash) ─────────────────────────────────────────────

static const char SETUP_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM Setup</title>
<style>
body{font-family:Arial,sans-serif;max-width:420px;margin:60px auto;
  background:#1a1a2e;color:#eee;padding:0 20px}
h1{text-align:center;color:#e94560}
.fg{margin:14px 0}
label{display:block;margin-bottom:5px;font-size:.9em}
input[type=text],input[type=password]{
  width:100%;padding:10px;box-sizing:border-box;
  background:#16213e;color:#eee;border:1px solid #0f3460;
  border-radius:4px;font-size:1em}
select{
  width:100%;padding:10px;box-sizing:border-box;
  background:#16213e;color:#eee;border:1px solid #0f3460;
  border-radius:4px;font-size:1em}
button,input[type=submit]{
  width:100%;padding:12px;background:#e94560;color:#fff;
  border:none;border-radius:4px;cursor:pointer;font-size:1em;margin-top:10px}
button:hover,input[type=submit]:hover{background:#c73652}
.scan-status{font-size:.82em;color:#7dd3fc;margin-top:8px;min-height:18px}
.scan-status.error{color:#ff8a8a}
.note{font-size:.8em;color:#888;margin-top:18px;text-align:center;line-height:1.5}
</style>
</head>
<body>
<h1>ESP32-CAM Setup</h1>
<form method="POST" action="/save">
  <div class="fg">
    <label>WiFi SSID</label>
    <input id="setup_ssid" type="text" name="ssid" placeholder="Network name" required maxlength="32">
    <button type="button" id="setup_scan_btn">Scan Nearby WiFi</button>
    <select id="setup_scan_list">
      <option value="">Select a scanned network</option>
    </select>
    <div id="setup_scan_status" class="scan-status"></div>
  </div>
  <div class="fg">
    <label>WiFi Password</label>
    <input type="password" name="wpass" placeholder="Leave blank if open" maxlength="64">
  </div>
  <div class="fg">
    <label>Camera Access Password</label>
      <input type="password" name="apass" placeholder="Minimum 8 characters"
        required minlength="8" maxlength="32">
  </div>
  <div class="fg">
    <label>Device Name</label>
      <input type="text" name="dname" placeholder="e.g. Living Room Cam" maxlength="32" value="ESP32-CAM">
  </div>
  <input type="submit" value="Save &amp; Connect">
</form>
<p class="note">
  The device will reboot and join your WiFi network.<br>
  Use the access password to log in (username: <b>admin</b>).
</p>
<script>
function sid(name){return document.getElementById(name);}
function setupSetScanStatus(message,isError){
  var el=sid('setup_scan_status');
  el.textContent=message||'';
  el.className=isError?'scan-status error':'scan-status';
}
function setupRenderScanList(networks){
  var list=sid('setup_scan_list');
  list.innerHTML='<option value="">Select a scanned network</option>';
  networks.forEach(function(network){
    var option=document.createElement('option');
    option.value=network.ssid;
    option.textContent=network.ssid+' ('+network.rssi+' dBm, '+(network.secure?network.security:'Open')+')';
    list.appendChild(option);
  });
}
function setupScanWifi(){
  setupSetScanStatus('Scanning nearby WiFi...',false);
  fetch('/wifi/scan').then(function(r){
    return r.json().then(function(data){
      if(!r.ok){throw new Error(data.error||'Scan failed');}
      return data;
    });
  }).then(function(data){
    setupRenderScanList(data.networks||[]);
    setupSetScanStatus((data.networks||[]).length?'Select an SSID from the list.':'No WiFi networks found.',false);
  }).catch(function(err){
    setupSetScanStatus(err.message||'Failed to scan WiFi',true);
  });
}
sid('setup_scan_btn').addEventListener('click',setupScanWifi);
sid('setup_scan_list').addEventListener('change',function(){
  if(this.value){sid('setup_ssid').value=this.value;}
});
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char SAVED_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>
body{font-family:Arial,sans-serif;text-align:center;
  background:#1a1a2e;color:#eee;padding:60px 20px}
h1{color:#4caf50}
</style>
</head>
<body>
<h1>Configuration Saved!</h1>
<p>The device is rebooting and will connect to your WiFi network.</p>
<p>Find the device IP address on your router and open it in a browser.</p>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char MAIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Camera</title>
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
.stream-panel img{width:100%;max-width:800px;border:2px solid #0f3460;border-radius:6px;background:#111;min-height:200px}
.stream-panel img.hidden{display:none}
.stream-placeholder{display:none;width:100%;max-width:800px;min-height:200px;margin:0 auto;border:2px dashed #234573;border-radius:6px;background:#111827;color:#7dd3fc;align-items:center;justify-content:center;padding:24px;font-size:.95em}
.stream-placeholder.visible{display:flex}
.btn{display:inline-block;margin-top:8px;padding:8px 20px;background:#e94560;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;font-size:.9em}
.btn:hover{background:#c73652}
.btn.recording{background:#c73652;animation:pulse 1s infinite}
.btn.flash-on{background:#fbbf24}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.7}}
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
    <img id="stream" alt="Loading stream...">
    <div id="stream_placeholder" class="stream-placeholder">Stream hidden</div>
    <br>
    <button class="btn" id="stream_toggle_btn">🙈 Hide Stream</button>
    <button class="btn" id="cap_btn">📸 Capture</button>
    <button class="btn" id="rec_btn">⏺️ Record</button>
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
  </div>
</div>
__APP_FOOTER__
<script>
var recordingMode=false;
var streamVisible=true;
var streamUrl='http://'+window.location.hostname+':81/stream?t='+encodeURIComponent('__ROUTE_TOKEN__');
function id(n){return document.getElementById(n);}
function chk(el){return el.checked?1:0;}
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
function setRecordingState(isRecording,statusText){
  var btn=id('rec_btn');
  var status=id('rec_status');
  recordingMode=!!isRecording;
  btn.classList.toggle('recording',recordingMode);
  btn.textContent=recordingMode?'⏹️ Stop':'⏺️ Record';
  if(statusText!==undefined){status.textContent=statusText;}
}
function bindFrameSizeControl(){
  var el=id('framesize');
  if(!el)return;
  el.addEventListener('change',function(){
    var shouldResumeStream=streamVisible;
    setRecordingState(recordingMode,'Applying resolution change...');
    if(shouldResumeStream){releaseStream(true);}
    ctrl('framesize',el.value,true).then(function(result){
      return loadStatus().catch(function(){}).then(function(){
        if(!result.ok){setRecordingState(recordingMode,result.text||'Failed to change resolution');}
        else if(recordingMode){setRecordingState(true,'Recording...');}
        else{id('rec_status').textContent='';}
        if(shouldResumeStream){setTimeout(function(){setStreamVisibility(true);},150);}
      });
    }).catch(function(){
      if(shouldResumeStream){setStreamVisibility(true);}
      setRecordingState(recordingMode,'Failed to change resolution');
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
  setStreamVisibility(s.stream_visible!==undefined?!!s.stream_visible:true);
  if(s.recording_active!==undefined){setRecordingState(!!s.recording_active,s.recording_active?'Recording...':'');}
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
['awb','aec','hmirror','vflip','lenc'].forEach(bindCheckboxControl);
id('stream_toggle_btn').addEventListener('click',function(){
  var nextVisible=!streamVisible;
  setStreamVisibility(nextVisible);
  ctrl('stream_visible',nextVisible?1:0,true);
});
id('cap_btn').addEventListener('click',function(){fetch('/capture').then(function(r){if(r.ok)id('rec_status').textContent='Capture saved to /capture folder';});});
id('rec_btn').addEventListener('click',function(){
  if(recordingMode){
    fetch('/record/stop',{method:'POST'}).then(function(r){
      return r.text().then(function(msg){
        if(r.ok){setRecordingState(false,msg||'Recording saved.');}
      });
    });
  }else{
    fetch('/record/start',{method:'POST'}).then(function(r){
      return r.text().then(function(msg){
        if(r.ok){setRecordingState(true,msg||'Recording...');}
      });
    });
  }
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
id('ip_label').innerText=h;
window.addEventListener('pagehide',function(){releaseStream(true);});
window.addEventListener('beforeunload',function(){releaseStream(true);});
setStreamVisibility(false);
loadStatus().catch(function(){
  setStreamVisibility(true);
});
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char ADMIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Admin</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
.panel{flex:1 1 100%;background:#16213e;border-radius:8px;padding:14px;margin:12px;max-width:600px}
.panel h3{color:#e94560;margin-bottom:12px;font-size:1em}
.form{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;align-items:end}
.form label{display:block;font-size:.82em;margin-bottom:3px;color:#bbb}
.form input,.form select{width:100%;padding:10px;box-sizing:border-box;background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px}
.form button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:8px 12px;cursor:pointer}
.form button:hover{background:#c73652}
.status{min-height:20px;font-size:.85em;margin-top:10px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.list{display:flex;flex-direction:column;gap:10px;margin-top:12px}
.item{display:flex;justify-content:space-between;align-items:center;gap:10px;background:#0f3460;border-radius:6px;padding:10px 12px}
.item button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;font-size:.8em}
.item button:hover{background:#c73652}
.empty{font-size:.85em;color:#bbb}
@media (max-width:640px){.panel{margin:12px 0}.item{flex-direction:column;align-items:flex-start}}
</style>
</head>
<body>
__APP_NAV__
<header>
  <h1>⚙️ Administration</h1>
</header>
<div style="padding:12px;display:flex;flex-wrap:wrap">
  <div class="panel">
    <h3>WiFi Priority</h3>
    <form class="form" id="wifi_form">
      <div>
        <label>WiFi SSID</label>
        <input id="wifi_ssid" type="text" maxlength="32" required>
      </div>
      <div>
        <label>WiFi Password</label>
        <input id="wifi_wpass" type="password" maxlength="64" placeholder="Leave blank if open">
      </div>
      <button type="submit">Add / Update</button>
    </form>
    <div style="display:grid;grid-template-columns:1fr auto;gap:8px;margin:10px 0 12px">
      <select id="wifi_scan_list"><option value="">Scan & select</option></select>
      <button onclick="scanWiFi()">Scan</button>
    </div>
    <div id="wifi_status" class="status"></div>
    <div class="list" id="wifi_list"></div>
  </div>
  <div class="panel">
    <h3>Admin Password</h3>
    <form class="form" id="admin_form">
      <div>
        <label>Current Password</label>
        <input id="admin_current" type="password" maxlength="32" required>
      </div>
      <div>
        <label>New Password</label>
        <input id="admin_new" type="password" minlength="8" maxlength="32" required>
      </div>
      <div>
        <label>Confirm New Password</label>
        <input id="admin_confirm" type="password" minlength="8" maxlength="32" required>
      </div>
      <button type="submit">Change Password</button>
    </form>
    <div id="admin_status" class="status"></div>
  </div>
  <div class="panel">
    <h3>Device Name</h3>
    <form class="form" id="name_form">
      <div>
        <label>New Device Name</label>
        <input id="device_name" type="text" maxlength="32" required>
      </div>
      <button type="submit">Change Name</button>
    </form>
    <div id="name_status" class="status"></div>
  </div>
  <div class="panel">
    <h3>Time</h3>
    <div class="status" id="time_now"></div>
    <form class="form" id="time_form">
      <div>
        <label>Manual Local Time</label>
        <input id="manual_time" type="datetime-local" step="1" required>
      </div>
      <button type="submit">Set Time</button>
      <button type="button" id="ntp_sync_btn">Sync NTP</button>
    </form>
    <div id="time_status" class="status"></div>
  </div>
  <div class="panel">
    <h3>LED Control</h3>
    <form class="form" id="led_form">
      <div style="display:flex;align-items:center;gap:10px">
        <label for="led_access_blink" style="margin:0">Blink on URL access</label>
        <input id="led_access_blink" type="checkbox" style="width:auto">
      </div>
    </form>
    <div id="led_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">When enabled, LED blinks briefly on each URL request. Boot sequences are unaffected.</div>
  </div>
  <div class="panel">
    <h3>Logging</h3>
    <form class="form" id="logging_form">
      <div style="display:flex;align-items:center;gap:10px">
        <label for="logging_enabled" style="margin:0">Enable serial + file logging</label>
        <input id="logging_enabled" type="checkbox" style="width:auto">
      </div>
    </form>
    <div id="logging_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Disables all firmware logs globally, including serial output and /log.txt writes.</div>
  </div>
  <div class="panel">
    <h3>TX Power</h3>
    <form class="form" id="txpower_form">
      <div>
        <label>STA Mode (dBm)</label>
        <select id="txpower_sta">
          <option value="-4">-1 dBm</option>
          <option value="8">2 dBm</option>
          <option value="20">5 dBm</option>
          <option value="28">7 dBm</option>
          <option value="34">8.5 dBm</option>
          <option value="44">11 dBm</option>
          <option value="52">13 dBm</option>
          <option value="60">15 dBm</option>
          <option value="68">17 dBm</option>
          <option value="72">18 dBm</option>
          <option value="76">19 dBm</option>
          <option value="78">19.5 dBm (max)</option>
        </select>
      </div>
      <div>
        <label>AP Fallback Mode (dBm)</label>
        <select id="txpower_ap">
          <option value="-4">-1 dBm</option>
          <option value="8">2 dBm</option>
          <option value="20">5 dBm</option>
          <option value="28">7 dBm</option>
          <option value="34">8.5 dBm</option>
          <option value="44">11 dBm</option>
          <option value="52">13 dBm</option>
          <option value="60">15 dBm</option>
          <option value="68">17 dBm</option>
          <option value="72">18 dBm</option>
          <option value="76">19 dBm</option>
          <option value="78">19.5 dBm (max)</option>
        </select>
      </div>
    </form>
    <div id="txpower_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Lower TX power reduces consumption. AP fallback clients are nearby so 8.5 dBm is a reasonable default. Changes apply immediately.</div>
  </div>
  <div class="panel">
    <h3>Firmware Update</h3>
    <div class="status" style="color:#bbb;margin-bottom:10px">Current version: <strong style="color:#eee">__FIRMWARE_VERSION__</strong><br>Build: __FIRMWARE_BUILD__</div>
    <form class="form" id="firmware_form">
      <div>
        <label>Firmware Binary (.bin)</label>
        <input id="firmware_file" type="file" accept=".bin,application/octet-stream" required>
      </div>
      <button type="submit">Upload Firmware</button>
    </form>
    <div id="firmware_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Upload the compiled firmware binary. The device will reboot automatically after a successful update.</div>
  </div>
  <div class="panel">
    <h3>System Reset</h3>
    <form class="form" id="reset_form">
      <button type="submit">Restart Device</button>
    </form>
    <div id="reset_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Restarts the ESP32-CAM without changing saved settings.</div>
    <form class="form" id="factory_reset_form" style="margin-top:10px">
      <button type="submit">Factory Reset (Delete Config)</button>
    </form>
    <div id="factory_reset_status" class="status"></div>
  </div>
</div>
__APP_FOOTER__
<script>
function id(n){return document.getElementById(n);}
var transferBase='http://'+window.location.hostname+':82';
var transferToken=encodeURIComponent('__ROUTE_TOKEN__');
function setWiFiStatus(msg,err){var e=id('wifi_status');e.textContent=msg;e.className=err?'status error':'status';}
function setAdminStatus(msg,err){var e=id('admin_status');e.textContent=msg;e.className=err?'status error':'status';}
function setNameStatus(msg,err){var e=id('name_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTimeStatus(msg,err){var e=id('time_status');e.textContent=msg;e.className=err?'status error':'status';}
function setLedStatus(msg,err){var e=id('led_status');e.textContent=msg;e.className=err?'status error':'status';}
function setLoggingStatus(msg,err){var e=id('logging_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTxPowerStatus(msg,err){var e=id('txpower_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFirmwareStatus(msg,err){var e=id('firmware_status');e.textContent=msg;e.className=err?'status error':'status';}
function setResetStatus(msg,err){var e=id('reset_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFactoryResetStatus(msg,err){var e=id('factory_reset_status');e.textContent=msg;e.className=err?'status error':'status';}
function formData(obj){return Object.keys(obj).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(obj[k]);}).join('&');}
function toDateTimeLocalValue(epoch){
  var d=new Date((Number(epoch)||0)*1000);
  if(isNaN(d.getTime())) return '';
  var pad=function(n){return n<10?'0'+n:String(n);};
  return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'T'+pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
}
function refreshTimeStatus(){
  fetch('/admin/time').then(function(r){
    if(!r.ok){throw new Error('Failed to load time status');}
    return r.json();
  }).then(function(d){
    id('time_now').textContent='Current: '+(d.local||'unknown')+' • '+(d.sane?'Clock synced':'Clock not synced')+' • '+(d.wifiConnected?'WiFi connected':'WiFi offline');
    if(d.epoch){id('manual_time').value=toDateTimeLocalValue(d.epoch);}
  }).catch(function(e){
    id('time_now').textContent='Current: unavailable';
    setTimeStatus(e.message,true);
  });
}
function syncNtpTime(){
  setTimeStatus('Syncing NTP...',false);
  fetch('/admin/time/sync',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setTimeStatus(msg||'NTP sync request finished',!r.ok);
      refreshTimeStatus();
    });
  }).catch(function(e){setTimeStatus(e.message,true);});
}
function refreshDeviceName(){
  var input=id('device_name');
  fetch('/admin/name').then(function(r){
    if(!r.ok){throw new Error('Failed to load device name');}
    return r.json();
  }).then(function(d){
    if(d && typeof d.deviceName==='string' && d.deviceName.length){input.value=d.deviceName;}
  }).catch(function(){});
}
function scanWiFi(){
  setWiFiStatus('Scanning...',false);
  fetch('/wifi/scan').then(function(r){return r.json();}).then(function(data){
    var sel=id('wifi_scan_list');sel.innerHTML='<option value="">Select a network</option>';
    (data.networks||[]).forEach(function(n){var opt=document.createElement('option');opt.value=n.ssid;opt.textContent=n.ssid+' ('+n.rssi+' dBm)';sel.appendChild(opt);});
    setWiFiStatus(data.networks.length+' networks found',false);
  }).catch(function(e){setWiFiStatus(e.message,true);});
}
function renderWiFiList(items){
  var list=id('wifi_list');
  if(!items.length){list.innerHTML='<div class="empty">No networks saved.</div>';return;}
  list.innerHTML=items.map(function(item,i){
    return '<div class="item"><div><strong>'+(i+1)+'. '+item.ssid+'</strong><span style="font-size:.8em;color:#bbb">'+(item.hasPassword?'Protected':'Open')+'</span></div><div style="display:flex;gap:6px"><button onclick="moveWiFi('+i+',\'up\')"'+(i===0?' disabled':'')+'>↑</button><button onclick="moveWiFi('+i+',\'down\')"'+(i===items.length-1?' disabled':'')+'>↓</button><button onclick="deleteWiFi('+i+')">✕</button></div></div>';
  }).join('');
}
function moveWiFi(i,dir){
  fetch('/wifi/move',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i,dir:dir})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Updated':'Failed',!r.ok);});
}
function deleteWiFi(i){
  fetch('/wifi/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Deleted':'Failed',!r.ok);});
}
function refreshWiFiList(){fetch('/wifi/list').then(function(r){return r.json();}).then(function(d){renderWiFiList(d.networks||[]);});}
id('wifi_form').addEventListener('submit',function(e){e.preventDefault();fetch('/wifi/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({ssid:id('wifi_ssid').value,wpass:id('wifi_wpass').value})}).then(function(r){r.text().then(function(msg){setWiFiStatus(msg,!r.ok);if(r.ok){id('wifi_form').reset();refreshWiFiList();id('wifi_scan_list').value='';}});});});
id('admin_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({current:id('admin_current').value,next:id('admin_new').value,confirm:id('admin_confirm').value})}).then(function(r){r.text().then(function(msg){setAdminStatus(msg,!r.ok);if(r.ok)id('admin_form').reset();});});});
id('name_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({name:id('device_name').value})}).then(function(r){r.text().then(function(msg){setNameStatus(msg,!r.ok);if(r.ok)refreshDeviceName();});});});
id('time_form').addEventListener('submit',function(e){
  e.preventDefault();
  var raw=id('manual_time').value;
  if(!raw){setTimeStatus('Choose a date and time first',true);return;}
  var dt=new Date(raw);
  if(isNaN(dt.getTime())){setTimeStatus('Invalid date/time value',true);return;}
  var epoch=Math.floor(dt.getTime()/1000);
  fetch('/admin/time/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({epoch:epoch})}).then(function(r){
    return r.text().then(function(msg){
      setTimeStatus(msg||'Time updated',!r.ok);
      refreshTimeStatus();
    });
  }).catch(function(err){setTimeStatus(err.message,true);});
});
id('ntp_sync_btn').addEventListener('click',syncNtpTime);
id('wifi_scan_list').addEventListener('change',function(){if(this.value)id('wifi_ssid').value=this.value;});
function refreshLedStatus(){
  fetch('/admin/led').then(function(r){
    if(!r.ok){throw new Error('Failed to load LED settings');}
    return r.json();
  }).then(function(d){
    id('led_access_blink').checked=d.ledAccessBlink||false;
  }).catch(function(){});
}
function saveLedSettings(){
  setLedStatus('Saving...',false);
  fetch('/admin/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({ledAccessBlink:id('led_access_blink').checked?'1':'0'})}).then(function(r){r.text().then(function(msg){setLedStatus(msg||'Saved',!r.ok);refreshLedStatus();});}).catch(function(err){setLedStatus(err.message||'Failed to save LED settings',true);});
}
id('led_form').addEventListener('submit',function(e){
  e.preventDefault();
  saveLedSettings();
});
id('led_access_blink').addEventListener('change',saveLedSettings);
function refreshLoggingStatus(){
  fetch('/admin/logging').then(function(r){
    if(!r.ok){throw new Error('Failed to load logging settings');}
    return r.json();
  }).then(function(d){
    id('logging_enabled').checked=d.loggingEnabled!==false;
  }).catch(function(){});
}
function saveLoggingSettings(){
  setLoggingStatus('Saving...',false);
  fetch('/admin/logging',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({loggingEnabled:id('logging_enabled').checked?'1':'0'})}).then(function(r){r.text().then(function(msg){setLoggingStatus(msg||'Saved',!r.ok);refreshLoggingStatus();});}).catch(function(err){setLoggingStatus(err.message||'Failed to save logging settings',true);});
}
id('logging_form').addEventListener('submit',function(e){
  e.preventDefault();
  saveLoggingSettings();
});
id('logging_enabled').addEventListener('change',saveLoggingSettings);
function refreshTxPower(){
  fetch('/admin/txpower').then(function(r){
    if(!r.ok){throw new Error('Failed to load TX power settings');}
    return r.json();
  }).then(function(d){
    var sSel=id('txpower_sta');var aSel=id('txpower_ap');
    for(var i=0;i<sSel.options.length;i++){if(parseInt(sSel.options[i].value)===d.txPowerSta){sSel.selectedIndex=i;break;}}
    for(var i=0;i<aSel.options.length;i++){if(parseInt(aSel.options[i].value)===d.txPowerAp){aSel.selectedIndex=i;break;}}
  }).catch(function(){});
}
function saveTxPowerSettings(){
  setTxPowerStatus('Saving...',false);
  fetch('/admin/txpower',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({txPowerSta:id('txpower_sta').value,txPowerAp:id('txpower_ap').value})}).then(function(r){r.text().then(function(msg){setTxPowerStatus(msg||'Saved',!r.ok);});}).catch(function(err){setTxPowerStatus(err.message||'Failed to save TX power settings',true);});
}
id('txpower_form').addEventListener('submit',function(e){
  e.preventDefault();
  saveTxPowerSettings();
});
id('txpower_sta').addEventListener('change',saveTxPowerSettings);
id('txpower_ap').addEventListener('change',saveTxPowerSettings);
id('firmware_form').addEventListener('submit',function(e){
  e.preventDefault();
  var input=id('firmware_file');
  if(!input.files.length){setFirmwareStatus('Choose a firmware .bin file first',true);return;}
  var file=input.files[0];
  setFirmwareStatus('Uploading '+file.name+'...',false);
  var fd=new FormData();
  fd.append('firmware',file);
  fetch(transferBase+'/admin/update?t='+transferToken,{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(msg){
      setFirmwareStatus(msg||'Firmware upload finished',!r.ok);
      if(r.ok){input.value='';}
    });
  }).catch(function(err){setFirmwareStatus(err.message||'Firmware upload failed',true);});
});
id('reset_form').addEventListener('submit',function(e){
  e.preventDefault();
  if(!confirm('Restart device now?')){return;}
  setResetStatus('Scheduling restart...',false);
  fetch('/admin/reset',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setResetStatus(msg||'Restart requested',!r.ok);
    });
  }).catch(function(err){setResetStatus(err.message||'Restart failed',true);});
});
id('factory_reset_form').addEventListener('submit',function(e){
  e.preventDefault();
  if(!confirm('Delete stored configuration and restart to setup mode?')){return;}
  setFactoryResetStatus('Deleting configuration and scheduling restart...',false);
  fetch('/admin/factory-reset',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setFactoryResetStatus(msg||'Factory reset requested',!r.ok);
    });
  }).catch(function(err){setFactoryResetStatus(err.message||'Factory reset failed',true);});
});
refreshWiFiList();
refreshDeviceName();
refreshTimeStatus();
refreshLedStatus();
refreshLoggingStatus();
refreshTxPower();
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char SD_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - SD Browser</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
.container{max-width:1080px;margin:0 auto;padding:12px}
.actions{display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap}
.actions label,.actions button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.actions button:hover{background:#234573}
.actions button:disabled{opacity:.55;cursor:not-allowed}
.actions input[type=file]{display:none}
.folder-create{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.folder-create input{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px;min-width:200px}
.folder-create button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.folder-create button:hover{background:#234573}
.dropzone{border:2px dashed #234573;border-radius:8px;padding:14px 12px;margin-bottom:10px;text-align:center;color:#9ec5ff;background:#111c38;transition:background .15s,border-color .15s,color .15s}
.dropzone strong{color:#dbeafe}
.dropzone.active{border-color:#7dd3fc;background:#0d2f47;color:#dbeafe}
.pathbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.pathbar button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.85em}
.pathbar button:hover{background:#234573}
.crumbs{font-size:.88em;color:#bbb;word-break:break-all}
.crumbs a{color:#7dd3fc;text-decoration:none}
.crumbs a:hover{text-decoration:underline}
.sortbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.sortbar label{font-size:.85em;color:#bbb}
.sortbar select{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px}
.status{min-height:20px;margin-bottom:12px;color:#7dd3fc;font-size:.9em}
.status.error{color:#ff8a8a}
.file-list{display:flex;flex-direction:column;gap:12px}
.group{background:#16213e;border:1px solid #234573;border-radius:8px;padding:10px}
.group h3{color:#7dd3fc;font-size:.95em;margin-bottom:8px}
.selection-summary{font-size:.82em;color:#bbb;margin:-2px 0 10px}
.file-item{display:flex;align-items:center;gap:8px;background:#0f3460;padding:10px;border-radius:6px;border:1px solid #234573;margin-bottom:8px}
.file-item.selected{border-color:#7dd3fc;box-shadow:0 0 0 1px rgba(125,211,252,.35)}
.item-select{display:flex;align-items:center;justify-content:center;flex:0 0 auto;margin-right:2px}
.item-select input{width:18px;height:18px;cursor:pointer}
.file-left{display:flex;align-items:center;gap:10px;min-width:0;flex:1 1 auto}
.thumb{width:72px;height:54px;border-radius:4px;object-fit:cover;background:#111;border:1px solid #234573;display:block}
.meta{display:flex;flex-direction:column;min-width:0}
.meta strong{color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:360px}
.meta span{font-size:.82em;color:#bbb}
.file-actions{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.file-actions{margin-left:auto}
.file-actions a,.file-actions button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;text-decoration:none;font-size:.8em}
.file-actions button:hover,.file-actions a:hover{background:#c73652}
.empty{text-align:center;padding:40px;color:#bbb}
@media (max-width:700px){.file-item{flex-direction:column;align-items:flex-start}.meta strong{max-width:100%}.thumb{width:100px;height:75px}.file-actions{justify-content:flex-start}}
</style>
</head>
<body>
__APP_NAV__
<header>
  <h1>💾 SD Card Browser</h1>
</header>
<div class="container">
  <div class="actions">
    <button onclick="loadFiles()">Refresh</button>
    <button id="select_all" onclick="toggleSelectAllFiles()" disabled>Select All</button>
    <button id="download_selected" onclick="downloadSelectedFiles()" disabled>Download Selected</button>
    <button id="delete_selected" onclick="deleteSelectedFiles()" disabled>Delete Selected</button>
    <label>Upload: <input id="upload_file" type="file" onchange="uploadFile(this)"></label>
  </div>
  <div class="folder-create">
    <input id="new_folder_name" type="text" placeholder="New folder name" maxlength="64">
    <button onclick="createFolder()">Create Folder</button>
  </div>
  <div id="dropzone" class="dropzone"><strong>Drag and drop files here</strong> to upload into the current folder</div>
  <div class="pathbar">
    <button onclick="goUp()">Up</button>
    <div id="crumbs" class="crumbs"></div>
  </div>
  <div class="sortbar">
    <label for="sort_by">Sort by</label>
    <select id="sort_by" onchange="onSortChanged()">
      <option value="name">Name</option>
      <option value="size">Size</option>
      <option value="type">Type</option>
    </select>
    <label for="sort_dir">Direction</label>
    <select id="sort_dir" onchange="onSortChanged()">
      <option value="asc">Ascending</option>
      <option value="desc">Descending</option>
    </select>
  </div>
  <div id="status" class="status"></div>
  <div id="file_list" class="file-list"></div>
</div>
__APP_FOOTER__
<script>
var allItems=[];
var currentDir='/';
var transferBase='http://'+window.location.hostname+':82';
var transferToken=encodeURIComponent('__ROUTE_TOKEN__');
var selectedFiles={};
var visibleFileOrder=[];
var lastSelectionAnchor='';
function esc(s){return String(s).replace(/[&<>\"']/g,function(ch){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;','\'':'&#39;'}[ch];});}
function setStatus(msg,isError){
  var el=document.getElementById('status');
  el.textContent=msg||'';
  el.className=isError?'status error':'status';
}
function normalizePath(p){
  var path=String(p||'/').replace(/\\/g,'/');
  if(!path.startsWith('/')) path='/'+path;
  if(path.length>1 && path.endsWith('/')) path=path.slice(0,-1);
  return path;
}
function validFolderName(name){
  var n=String(name||'').trim();
  if(!n||n==='.'||n==='..')return false;
  if(n.indexOf('/')!==-1||n.indexOf('\\')!==-1||n.indexOf('..')!==-1)return false;
  return true;
}
function baseName(path){
  var clean=normalizePath(path);
  if(clean==='/') return '/';
  var idx=clean.lastIndexOf('/');
  return idx>=0?clean.slice(idx+1):clean;
}
function isImage(path){
  var p=String(path).toLowerCase();
  return p.endsWith('.jpg')||p.endsWith('.jpeg')||p.endsWith('.png')||p.endsWith('.gif')||p.endsWith('.webp')||p.endsWith('.bmp');
}
function formatSize(bytes){
  var n=Number(bytes)||0;
  if(n<1024)return n+' B';
  if(n<1024*1024)return (n/1024).toFixed(1)+' KB';
  return (n/(1024*1024)).toFixed(2)+' MB';
}
function quotedName(path){
  return '"'+baseName(path)+'"';
}
function getSelectedFiles(){
  return Object.keys(selectedFiles).filter(function(path){return !!selectedFiles[path];});
}
function getVisibleFilePaths(){
  return visibleFileOrder.slice();
}
function syncSelectedFiles(){
  var valid={};
  allItems.forEach(function(item){
    if(!item.isDir && selectedFiles[item.path]){
      valid[item.path]=true;
    }
  });
  selectedFiles=valid;
}
function updateBulkDeleteButton(){
  var count=getSelectedFiles().length;
  var visiblePaths=getVisibleFilePaths();
  var visibleCount=visiblePaths.length;
  var selectedVisibleCount=visiblePaths.filter(function(path){return !!selectedFiles[path];}).length;
  var allVisibleSelected=visibleCount>0&&selectedVisibleCount===visibleCount;
  var deleteButton=document.getElementById('delete_selected');
  var downloadButton=document.getElementById('download_selected');
  var selectAllButton=document.getElementById('select_all');
  var hasSelection=count>0;
  deleteButton.disabled=!hasSelection;
  downloadButton.disabled=!hasSelection;
  selectAllButton.disabled=visibleCount===0;
  selectAllButton.textContent=allVisibleSelected?'Clear All':'Select All';
  deleteButton.textContent=hasSelection?'Delete Selected ('+count+')':'Delete Selected';
  downloadButton.textContent=hasSelection?'Download Selected ('+count+')':'Download Selected';
}
function toggleFileSelection(path,checked){
  var normalized=String(path||'');
  if(!normalized)return;
  if(checked) selectedFiles[normalized]=true;
  else delete selectedFiles[normalized];
  lastSelectionAnchor=normalized;
  applySortAndRender();
}
function applyRangeSelection(anchorPath,targetPath,checked){
  var paths=getVisibleFilePaths();
  var start=paths.indexOf(anchorPath);
  var end=paths.indexOf(targetPath);
  if(start<0||end<0){
    toggleFileSelection(targetPath,checked);
    return;
  }
  var low=Math.min(start,end);
  var high=Math.max(start,end);
  for(var i=low;i<=high;i++){
    if(checked) selectedFiles[paths[i]]=true;
    else delete selectedFiles[paths[i]];
  }
  lastSelectionAnchor=targetPath;
  applySortAndRender();
}
function onFileCheckboxClick(path,input,event){
  var checked=!!(input&&input.checked);
  var useRange=!!(event&&event.shiftKey&&lastSelectionAnchor);
  if(useRange){
    applyRangeSelection(lastSelectionAnchor,path,checked);
    return;
  }
  toggleFileSelection(path,checked);
}
function toggleSelectAllFiles(){
  var paths=getVisibleFilePaths();
  if(!paths.length){
    setStatus('No files to select in this folder.',true);
    return;
  }
  var allSelected=paths.every(function(path){return !!selectedFiles[path];});
  if(allSelected){
    paths.forEach(function(path){delete selectedFiles[path];});
    setStatus('Selection cleared.',false);
  }else{
    paths.forEach(function(path){selectedFiles[path]=true;});
    setStatus(paths.length+' file(s) selected.',false);
  }
  lastSelectionAnchor='';
  applySortAndRender();
}
function buildDeleteBody(paths){
  return paths.map(function(path){return 'file='+encodeURIComponent(path);}).join('&');
}
function deleteFiles(paths){
  var items=(paths||[]).filter(function(path){return !!path;});
  if(!items.length){
    setStatus('Select at least one file to delete.',true);
    return Promise.resolve(false);
  }
  var label=items.length===1?quotedName(items[0]):String(items.length)+' files';
  setStatus('Deleting '+label+'...',false);
  return fetch('/sd/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:buildDeleteBody(items)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Delete failed');}
      items.forEach(function(path){delete selectedFiles[path];});
      setStatus(t||'File deleted',false);
      loadFiles();
      return true;
    });
  }).catch(function(e){
    setStatus(e.message||'Delete failed',true);
    return false;
  });
}
function downloadFile(path){
  var anchor=document.createElement('a');
  anchor.href=transferBase+'/sd/download?file='+encodeURIComponent(path)+'&t='+transferToken;
  anchor.download=baseName(path);
  anchor.style.display='none';
  document.body.appendChild(anchor);
  anchor.click();
  document.body.removeChild(anchor);
}
function downloadSelectedFiles(){
  var files=getSelectedFiles();
  if(!files.length){
    setStatus('Select at least one file to download.',true);
    return;
  }
  for(var i=0;i<files.length;i++){
    (function(path,delayMs){
      setTimeout(function(){downloadFile(path);},delayMs);
    })(files[i],i*120);
  }
  setStatus('Started '+files.length+' download(s). Your browser may ask for permission.',false);
}
function renderCrumbs(){
  var el=document.getElementById('crumbs');
  var clean=normalizePath(currentDir);
  if(clean==='/'){
    el.innerHTML='<strong>/</strong>';
    return;
  }
  var parts=clean.slice(1).split('/');
  var acc='';
  var links=['<a href="#" onclick="openDir(\'/\');return false;">/</a>'];
  for(var i=0;i<parts.length;i++){
    acc+='/'+parts[i];
    if(i===parts.length-1){
      links.push('<strong>'+esc(parts[i])+'</strong>');
    }else{
      links.push('<a href="#" onclick="openDir(\''+acc.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">'+esc(parts[i])+'</a>');
    }
  }
  el.innerHTML=links.join(' / ');
}
function sortItems(items){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value==='desc'?-1:1;
  function splitNumericPrefix(name){
    var text=String(name||'');
    var m=text.match(/^(\d+)(.*)$/);
    if(!m) return {hasPrefix:false,num:0,rest:text};
    return {hasPrefix:true,num:parseInt(m[1],10)||0,rest:m[2]};
  }
  function compareSmartName(aName,bName){
    var aParts=splitNumericPrefix(aName);
    var bParts=splitNumericPrefix(bName);
    if(aParts.hasPrefix&&bParts.hasPrefix){
      if(aParts.num!==bParts.num) return aParts.num-bParts.num;
      return aParts.rest.localeCompare(bParts.rest);
    }
    if(aParts.hasPrefix!==bParts.hasPrefix) return aParts.hasPrefix?-1:1;
    return String(aName||'').localeCompare(String(bName||''));
  }
  return items.slice().sort(function(a,b){
    if(a.isDir!==b.isDir) return a.isDir?-1:1;
    var ba=String(a.name||'');
    var bb=String(b.name||'');
    var ta=ba.indexOf('.')>=0?ba.slice(ba.lastIndexOf('.')+1).toLowerCase():'';
    var tb=bb.indexOf('.')>=0?bb.slice(bb.lastIndexOf('.')+1).toLowerCase():'';

    var cmp=0;
    if(by==='size') cmp=(Number(a.size)||0)-(Number(b.size)||0);
    else if(by==='type') cmp=ta.localeCompare(tb)||compareSmartName(ba,bb);
    else cmp=compareSmartName(ba,bb);

    return cmp*dir;
  });
}
function renderItems(items){
  var list=document.getElementById('file_list');
  syncSelectedFiles();
  visibleFileOrder=items.filter(function(item){return !item.isDir;}).map(function(item){return item.path;});
  if(lastSelectionAnchor&&visibleFileOrder.indexOf(lastSelectionAnchor)<0){
    lastSelectionAnchor='';
  }
  updateBulkDeleteButton();
  if(!items.length){
    list.innerHTML='<div class="empty">This folder is empty</div>';
    setStatus('Folder '+currentDir+' is empty.',false);
    return;
  }

  var totalSize=0;
  items.forEach(function(f){if(!f.isDir)totalSize+=Number(f.size)||0;});

  var rows=items.map(function(item){
    var isSelected=!!selectedFiles[item.path];
    var selectionClass=isSelected?' selected':'';
    var selectionCell=item.isDir
      ?'<div class="item-select"></div>'
      :'<label class="item-select" aria-label="Select '+esc(item.name)+'"><input type="checkbox" '+(isSelected?'checked ':'')+'onclick="onFileCheckboxClick(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\',this,event)"></label>';
    if(item.isDir){
      return '<div class="file-item'+selectionClass+'">'
        +selectionCell
        +'<div class="file-left" ondblclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')"><div class="thumb"></div><div class="meta"><strong>'+esc(item.name)+'</strong><span>Folder • '+esc(item.path)+'</span></div></div>'
        +'<div class="file-actions"><button onclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Open</button><button onclick="deleteFolder(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
        +'</div>';
    }

    var preview=isImage(item.path)
      ?'<img class="thumb" loading="lazy" src="'+transferBase+'/sd/view?file='+encodeURIComponent(item.path)+'&t='+transferToken+'" alt="preview">'
      :'<div class="thumb"></div>';
    return '<div class="file-item'+selectionClass+'">'
      +selectionCell
      +'<div class="file-left" ondblclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">'+preview+'<div class="meta"><strong>'+esc(item.name)+'</strong><span>'+formatSize(item.size)+' • '+esc(item.path)+'</span></div></div>'
      +'<div class="file-actions"><a href="'+transferBase+'/sd/download?file='+encodeURIComponent(item.path)+'&t='+transferToken+'">Download</a><a href="#" onclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">Open</a><button onclick="deleteFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
      +'</div>';
  }).join('');

  var selectedCount=getSelectedFiles().length;
  var summary=selectedCount>0?'<div class="selection-summary">'+selectedCount+' file(s) selected</div>':'';
  list.innerHTML='<section class="group"><h3>'+esc(currentDir)+'</h3>'+summary+rows+'</section>';
  var fileCount=items.filter(function(i){return !i.isDir;}).length;
  var dirCount=items.filter(function(i){return i.isDir;}).length;
  setStatus(dirCount+' folder(s), '+fileCount+' file(s), '+formatSize(totalSize),false);
}
function applySortAndRender(){
  renderCrumbs();
  renderItems(sortItems(allItems));
}
function openDir(path){
  currentDir=normalizePath(path);
  loadFiles();
}
function openFile(path){
  var p=String(path||'').toLowerCase();
  var isVideo=p.endsWith('.avi')||p.endsWith('.mp4')||p.endsWith('.mjpg')||p.endsWith('.mov')||p.endsWith('.webm');
  if(isVideo){
    window.location.assign('/sd/player?file='+encodeURIComponent(path));
    return;
  }
  window.location.assign(transferBase+'/sd/view?file='+encodeURIComponent(path)+'&t='+transferToken);
}
function goUp(){
  if(currentDir==='/') return;
  var parent=currentDir.substring(0,currentDir.lastIndexOf('/'));
  if(!parent) parent='/';
  openDir(parent);
}
function loadFiles(){
  setStatus('Loading files...',false);
  fetch('/sd/list?dir='+encodeURIComponent(currentDir)).then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load files');}
      return d;
    });
  }).then(function(d){
    currentDir=normalizePath(d.dir||currentDir);
    allItems=d.items||[];
    syncSelectedFiles();
    applySortAndRender();
  }).catch(function(e){
    document.getElementById('file_list').innerHTML='<div class="empty">Error loading files</div>';
    visibleFileOrder=[];
    lastSelectionAnchor='';
    updateBulkDeleteButton();
    setStatus(e.message||'Failed to load files',true);
  });
}
function deleteFile(name){
  if(!confirm('Delete '+quotedName(name)+'?'))return;
  deleteFiles([name]);
}
function deleteSelectedFiles(){
  var files=getSelectedFiles();
  if(!files.length){
    setStatus('Select at least one file to delete.',true);
    return;
  }
  if(!confirm('Delete '+files.length+' selected file(s)?'))return;
  deleteFiles(files);
}
function createFolder(){
  var input=document.getElementById('new_folder_name');
  var name=String(input.value||'').trim();
  if(!validFolderName(name)){
    setStatus('Invalid folder name',true);
    return;
  }
  setStatus('Creating folder '+name+'...',false);
  var body='dir='+encodeURIComponent(currentDir)+'&name='+encodeURIComponent(name);
  fetch('/sd/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to create folder');}
      setStatus(t||'Folder created',false);
      input.value='';
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to create folder',true);});
}
function deleteFolder(path){
  if(path==='/'||!path){
    setStatus('Cannot delete root folder',true);
    return;
  }
  if(!confirm('Delete folder '+path+' and all contents?'))return;
  setStatus('Deleting folder '+path+'...',false);
  fetch('/sd/rmdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'dir='+encodeURIComponent(path)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to delete folder');}
      setStatus(t||'Folder deleted',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to delete folder',true);});
}
function uploadFileObject(file){
  if(!file)return;
  setStatus('Uploading '+file.name+'...',false);
  var fd=new FormData();fd.append('file',file);
  fetch(transferBase+'/sd/upload?t='+transferToken+'&dir='+encodeURIComponent(currentDir),{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Upload failed');}
      setStatus(t||'Upload complete',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Upload failed',true);});
}
function uploadFile(input){
  if(!input.files.length)return;
  uploadFileObject(input.files[0]);
  input.value='';
}
function isValidSortBy(v){
  return v==='name'||v==='size'||v==='type';
}
function isValidSortDir(v){
  return v==='asc'||v==='desc';
}
function saveSortPreference(){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value;
  fetch('/sd/sort',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'by='+encodeURIComponent(by)+'&dir='+encodeURIComponent(dir)}).catch(function(){});
}
function loadSortPreference(){
  return fetch('/sd/sort').then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load sort preference');}
      return d;
    });
  }).then(function(d){
    document.getElementById('sort_by').value=isValidSortBy(d.by)?d.by:'name';
    document.getElementById('sort_dir').value=isValidSortDir(d.dir)?d.dir:'asc';
  }).catch(function(){
    document.getElementById('sort_by').value='name';
    document.getElementById('sort_dir').value='asc';
  });
}
function onSortChanged(){
  applySortAndRender();
  saveSortPreference();
}
function setupDropzone(){
  var zone=document.getElementById('dropzone');
  if(!zone)return;
  function over(e){
    e.preventDefault();
    zone.classList.add('active');
  }
  function leave(e){
    e.preventDefault();
    zone.classList.remove('active');
  }
  zone.addEventListener('dragenter',over);
  zone.addEventListener('dragover',over);
  zone.addEventListener('dragleave',leave);
  zone.addEventListener('drop',function(e){
    e.preventDefault();
    zone.classList.remove('active');
    var files=e.dataTransfer&&e.dataTransfer.files;
    if(!files||!files.length){
      setStatus('No file dropped',true);
      return;
    }
    uploadFileObject(files[0]);
  });
}
setupDropzone();
loadSortPreference().then(function(){loadFiles();});
</script>
</body>
</html>)html";

// ─── Motion pages (stored in flash) ───────────────────────────────────────────

static const char MOTION_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Motion</title>
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
    <div class="cg"><label><input id="wake_on_motion" type="checkbox"> Wake up on motion</label></div>
    <div class="cg"><label><input id="auto_standby" type="checkbox"> Automatic stand-by</label></div>
    <div class="small">When the device enters stand-by, the next motion wake is handled immediately.</div>

    <div class="cg row">
      <label for="standby_after_sec">No activity before stand-by (seconds)</label>
      <input id="standby_after_sec" type="number" min="5" max="120" step="1">
    </div>

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
    <div class="small">This cooldown applies only while the device is awake. A motion wake from stand-by ignores the cooldown once, then the cooldown resumes after that event completes.</div>

    <div class="cg row">
      <label for="notify_url">Notify URL (GET on motion detect)</label>
      <input id="notify_url" type="url" placeholder="http://example.local/motion">
    </div>

    <div class="small">Changes are saved automatically when you modify a setting.</div>
    <div style="margin-top:10px">
      <button class="btn" id="standby_btn" type="button">Go To Standby</button>
    </div>
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
    wakeOnMotion:id('wake_on_motion').checked?1:0,
    autoStandby:id('auto_standby').checked?1:0,
    standbyAfterSec:asInt(id('standby_after_sec').value,30),
    captureImage:id('capture_image').checked?1:0,
    imageCount:asInt(id('image_count').value,1),
    imageDelayDs:Math.round((parseFloat(id('image_delay_ds').value)||0.1)*10),
    captureVideo:id('capture_video').checked?1:0,
    videoDurationSec:asInt(id('video_duration_sec').value,5),
    detectionIntervalSec:asInt(id('detection_interval_sec').value,0),
    notifyUrl:id('notify_url').value||''
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

function wakeSourcesForDialog(){
  var events=['Button press'];
  if(id('wake_on_motion').checked){
    events.push('Motion (PIR sensor)');
  }
  return events;
}

function loadConfig(){
  fetch('/motion/config').then(function(r){
    if(!r.ok){throw new Error('Failed to load motion config');}
    return r.json();
  }).then(function(c){
    id('enabled').checked=!!c.enabled;
    id('wake_on_motion').checked=!!c.wakeOnMotion;
    id('auto_standby').checked=!!c.autoStandby;
    id('standby_after_sec').value=c.standbyAfterSec;
    id('capture_image').checked=!!c.captureImage;
    id('image_count').value=c.imageCount;
    id('image_delay_ds').value=((c.imageDelayDs||1)/10).toFixed(1);
    id('capture_video').checked=!!c.captureVideo;
    id('video_duration_sec').value=c.videoDurationSec;
    id('detection_interval_sec').value=String(c.detectionIntervalSec||0);
    id('notify_url').value=c.notifyUrl||'';
  }).catch(function(e){setStatus(e.message,true);});
}
id('standby_btn').addEventListener('click',function(){
  var events=wakeSourcesForDialog();
  var msg='Put device in standby now?\\n\\nActive wake-up events:\\n- '+events.join('\\n- ');
  if(!confirm(msg)){return;}
  setStatus('Entering standby...',false);
  fetch('/motion/standby',{method:'POST'})
    .then(function(r){
      return r.text().then(function(t){
        setStatus(t||'Standby requested',!r.ok);
      });
    })
    .catch(function(e){setStatus(e.message||'Failed to enter standby',true);});
});

bindAutoSave('enabled');
bindAutoSave('wake_on_motion');
bindAutoSave('auto_standby');
bindAutoSave('standby_after_sec');
bindAutoSave('capture_image');
bindAutoSave('image_count');
bindAutoSave('image_delay_ds');
bindAutoSave('capture_video');
bindAutoSave('video_duration_sec');
bindAutoSave('detection_interval_sec');
bindAutoSave('notify_url');
loadConfig();
</script>
</body>
</html>)html";

static const char MOTION_GRAPH_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Motion Graph</title>
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
