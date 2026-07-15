#pragma once

// Admin page template.

static const char ADMIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
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
.panel{flex:1 1 100%;background:#16213e;border-radius:8px;padding:14px;margin:12px;max-width:600px}
.panel h3{color:#e94560;margin-bottom:12px;font-size:1em}
.form{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;align-items:end}
.form label{display:block;font-size:.82em;margin-bottom:3px;color:#bbb}
.form input,.form select{width:100%;padding:10px;box-sizing:border-box;background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px}
.form button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:8px 12px;cursor:pointer}
.form button:hover{background:#c73652}
.btn{background:#e94560;color:#fff;border:none;border-radius:4px;padding:8px 12px;cursor:pointer}
.btn:hover{background:#c73652}
.btn:disabled{opacity:.55;cursor:not-allowed}
.status{min-height:20px;font-size:.85em;margin-top:10px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.scan-row{display:grid;grid-template-columns:1fr auto;gap:8px;margin:10px 0 12px}
.list{display:flex;flex-direction:column;gap:10px;margin-top:12px}
.item{display:flex;justify-content:space-between;align-items:center;gap:10px;background:#0f3460;border-radius:6px;padding:10px 12px}
.item button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;font-size:.8em}
.item button:hover{background:#c73652}
.item-main{display:flex;flex-direction:column;gap:4px;min-width:0}
.wifi-meta{font-size:.8em;color:#bbb}
.wifi-actions{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.wifi-toggle{display:flex;align-items:center;gap:5px;font-size:.82em;color:#bbb}
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
      <div>
        <label>IP Mode</label>
        <select id="wifi_netmode">
          <option value="dhcp">DHCP</option>
          <option value="static">Static</option>
        </select>
      </div>
      <div id="wifi_static_ip_group" style="display:none">
        <label>Static IP</label>
        <input id="wifi_ip" type="text" inputmode="decimal" placeholder="e.g. 192.168.1.70">
      </div>
      <div id="wifi_static_gw_group" style="display:none">
        <label>Gateway</label>
        <input id="wifi_gw" type="text" inputmode="decimal" placeholder="e.g. 192.168.1.1">
      </div>
      <div id="wifi_static_mask_group" style="display:none">
        <label>Subnet Mask</label>
        <input id="wifi_mask" type="text" inputmode="decimal" placeholder="e.g. 255.255.255.0">
      </div>
      <div>
        <label>DNS 1 (optional)</label>
        <input id="wifi_dns1" type="text" inputmode="decimal" placeholder="e.g. 1.1.1.1">
      </div>
      <div>
        <label>DNS 2 (optional)</label>
        <input id="wifi_dns2" type="text" inputmode="decimal" placeholder="e.g. 8.8.8.8">
      </div>
      <button type="submit">Add / Update</button>
    </form>
    <div class="scan-row">
      <select id="wifi_scan_list"><option value="">Scan & select</option></select>
      <button class="btn" onclick="scanWiFi()">Scan</button>
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
var wifiCache=[];
function setWiFiStatus(msg,err){var e=id('wifi_status');e.textContent=msg;e.className=err?'status error':'status';}
function setAdminStatus(msg,err){var e=id('admin_status');e.textContent=msg;e.className=err?'status error':'status';}
function setNameStatus(msg,err){var e=id('name_status');e.textContent=msg;e.className=err?'status error':'status';}
function setLedStatus(msg,err){var e=id('led_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTxPowerStatus(msg,err){var e=id('txpower_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFirmwareStatus(msg,err){var e=id('firmware_status');e.textContent=msg;e.className=err?'status error':'status';}
function setResetStatus(msg,err){var e=id('reset_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFactoryResetStatus(msg,err){var e=id('factory_reset_status');e.textContent=msg;e.className=err?'status error':'status';}
function formData(obj){return Object.keys(obj).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(obj[k]);}).join('&');}
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
function isValidIpv4(value){
  var trimmed=(value||'').trim();
  if(!trimmed){return false;}
  var parts=trimmed.split('.');
  if(parts.length!==4){return false;}
  return parts.every(function(part){
    if(!/^\d{1,3}$/.test(part)){return false;}
    var num=Number(part);
    return num>=0&&num<=255&&String(num)===String(parseInt(part,10));
  });
}
function validateIpv4Field(field,label,required,setStatus){
  var value=(field.value||'').trim();
  field.setCustomValidity('');
  if(!value){
    if(required){
      var requiredMsg=label+' is required';
      field.setCustomValidity(requiredMsg);
      field.reportValidity();
      setStatus(requiredMsg,true);
      return false;
    }
    return true;
  }
  if(!isValidIpv4(value)){
    var invalidMsg=label+' must be a valid IPv4 address';
    field.setCustomValidity(invalidMsg);
    field.reportValidity();
    setStatus(invalidMsg,true);
    return false;
  }
  field.value=value;
  return true;
}
function validateWiFiForm(){
  var staticMode=id('wifi_netmode').value==='static';
  if(!validateIpv4Field(id('wifi_ip'),'Static IP',staticMode,setWiFiStatus)){return false;}
  if(!validateIpv4Field(id('wifi_gw'),'Gateway',staticMode,setWiFiStatus)){return false;}
  if(!validateIpv4Field(id('wifi_mask'),'Subnet mask',staticMode,setWiFiStatus)){return false;}
  if(!validateIpv4Field(id('wifi_dns1'),'DNS 1',false,setWiFiStatus)){return false;}
  if(!validateIpv4Field(id('wifi_dns2'),'DNS 2',false,setWiFiStatus)){return false;}
  return true;
}
function updateWiFiStaticFieldVisibility(){
  var staticMode=id('wifi_netmode').value==='static';
  id('wifi_static_ip_group').style.display=staticMode?'':'none';
  id('wifi_static_gw_group').style.display=staticMode?'':'none';
  id('wifi_static_mask_group').style.display=staticMode?'':'none';
}
function setWiFiFormFromItem(item){
  id('wifi_ssid').value=item.ssid||'';
  id('wifi_wpass').value='';
  id('wifi_netmode').value=(item.netmode==='static')?'static':'dhcp';
  id('wifi_ip').value=item.ip||'';
  id('wifi_gw').value=item.gw||'';
  id('wifi_mask').value=item.mask||'';
  id('wifi_dns1').value=item.dns1||'';
  id('wifi_dns2').value=item.dns2||'';
  updateWiFiStaticFieldVisibility();
}
function renderWiFiList(items){
  var list=id('wifi_list');
  if(!items.length){list.innerHTML='<div class="empty">No networks saved.</div>';return;}
  list.innerHTML=items.map(function(item,i){
    var modeLabel=item.netmode==='static'?'Static':'DHCP';
    var staticSummary='';
    if(item.netmode==='static'&&item.ip){
      staticSummary=' • IP: '+item.ip;
    }
    var enabledChecked=item.enabled===false?'':' checked';
    return '<div class="item"><div class="item-main"><strong>'+(i+1)+'. '+item.ssid+'</strong>\n<span class="wifi-meta">'+(item.hasPassword?'Protected':'Open')+' • '+modeLabel+staticSummary+'</span></div><div class="wifi-actions"><button class="btn" onclick="editWiFi('+i+')">Edit</button><button class="btn" onclick="moveWiFi('+i+',\'up\')"'+(i===0?' disabled':'')+'>↑</button><button class="btn" onclick="moveWiFi('+i+',\'down\')"'+(i===items.length-1?' disabled':'')+'>↓</button><button class="btn" onclick="deleteWiFi('+i+')">✕</button>\n<label class="wifi-toggle"><input type="checkbox" onchange="setWiFiEnabled('+i+',this.checked)"'+enabledChecked+'> Enabled</label></div></div>';
  }).join('');
}
function setWiFiEnabled(i,enabled){
  fetch('/wifi/enabled',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i,enabled:enabled?'1':'0'})}).then(function(r){
    return r.text().then(function(msg){
      setWiFiStatus(msg||'Saved',!r.ok);
      if(r.ok){refreshWiFiList();}
    });
  }).catch(function(e){setWiFiStatus(e.message||'Failed to update network state',true);});
}
function editWiFi(i){
  var item=wifiCache[i];
  if(!item){return;}
  setWiFiFormFromItem(item);
  setWiFiStatus('Editing '+item.ssid+'. Submit to update.',false);
}
function moveWiFi(i,dir){
  fetch('/wifi/move',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i,dir:dir})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Updated':'Failed',!r.ok);});
}
function deleteWiFi(i){
  fetch('/wifi/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Deleted':'Failed',!r.ok);});
}
function resetWiFiForm(){
  id('wifi_form').reset();
  id('wifi_netmode').value='dhcp';
  updateWiFiStaticFieldVisibility();
}
function refreshWiFiList(){fetch('/wifi/list').then(function(r){return r.json();}).then(function(d){wifiCache=d.networks||[];renderWiFiList(wifiCache);});}
id('wifi_form').addEventListener('submit',function(e){
  e.preventDefault();
  if(!validateWiFiForm()){return;}
  var payload={
    ssid:id('wifi_ssid').value,
    wpass:id('wifi_wpass').value,
    netmode:id('wifi_netmode').value,
    ip:id('wifi_ip').value,
    gw:id('wifi_gw').value,
    mask:id('wifi_mask').value,
    dns1:id('wifi_dns1').value,
    dns2:id('wifi_dns2').value
  };
  fetch('/wifi/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData(payload)}).then(function(r){
    r.text().then(function(msg){
      setWiFiStatus(msg,!r.ok);
      if(r.ok){
        resetWiFiForm();
        refreshWiFiList();
        id('wifi_scan_list').value='';
      }
    });
  });
});
id('admin_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({current:id('admin_current').value,next:id('admin_new').value,confirm:id('admin_confirm').value})}).then(function(r){r.text().then(function(msg){setAdminStatus(msg,!r.ok);if(r.ok)id('admin_form').reset();});});});
id('name_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({name:id('device_name').value})}).then(function(r){r.text().then(function(msg){setNameStatus(msg,!r.ok);if(r.ok)refreshDeviceName();});});});
id('wifi_scan_list').addEventListener('change',function(){if(this.value)id('wifi_ssid').value=this.value;});
id('wifi_netmode').addEventListener('change',updateWiFiStaticFieldVisibility);
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
resetWiFiForm();
refreshDeviceName();
refreshLedStatus();
refreshLoggingStatus();
refreshTxPower();
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────
