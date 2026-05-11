#pragma once

// First-setup and saved-confirmation page templates.

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
<form method="POST" action="/save" id="setup_form">
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
    <label>IP Mode</label>
    <select id="setup_netmode" name="netmode">
      <option value="dhcp">DHCP</option>
      <option value="static">Static</option>
    </select>
  </div>
  <div class="fg" id="setup_static_ip_group" style="display:none">
    <label>Static IP</label>
    <input type="text" id="setup_ip" name="ip" inputmode="decimal" placeholder="e.g. 192.168.1.70">
  </div>
  <div class="fg" id="setup_static_gw_group" style="display:none">
    <label>Gateway</label>
    <input type="text" id="setup_gw" name="gw" inputmode="decimal" placeholder="e.g. 192.168.1.1">
  </div>
  <div class="fg" id="setup_static_mask_group" style="display:none">
    <label>Subnet Mask</label>
    <input type="text" id="setup_mask" name="mask" inputmode="decimal" placeholder="e.g. 255.255.255.0">
  </div>
  <div class="fg">
    <label>DNS 1 (optional)</label>
    <input type="text" id="setup_dns1" name="dns1" inputmode="decimal" placeholder="e.g. 1.1.1.1">
  </div>
  <div class="fg">
    <label>DNS 2 (optional)</label>
    <input type="text" id="setup_dns2" name="dns2" inputmode="decimal" placeholder="e.g. 8.8.8.8">
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
function setupIsValidIpv4(value){
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
function setupValidateIpv4Field(field,label,required){
  var value=(field.value||'').trim();
  field.setCustomValidity('');
  if(!value){
    if(required){
      var requiredMsg=label+' is required';
      field.setCustomValidity(requiredMsg);
      field.reportValidity();
      setupSetScanStatus(requiredMsg,true);
      return false;
    }
    return true;
  }
  if(!setupIsValidIpv4(value)){
    var invalidMsg=label+' must be a valid IPv4 address';
    field.setCustomValidity(invalidMsg);
    field.reportValidity();
    setupSetScanStatus(invalidMsg,true);
    return false;
  }
  field.value=value;
  return true;
}
function setupValidateNetworkFields(){
  var staticMode=sid('setup_netmode').value==='static';
  if(!setupValidateIpv4Field(sid('setup_ip'),'Static IP',staticMode)){return false;}
  if(!setupValidateIpv4Field(sid('setup_gw'),'Gateway',staticMode)){return false;}
  if(!setupValidateIpv4Field(sid('setup_mask'),'Subnet mask',staticMode)){return false;}
  if(!setupValidateIpv4Field(sid('setup_dns1'),'DNS 1',false)){return false;}
  if(!setupValidateIpv4Field(sid('setup_dns2'),'DNS 2',false)){return false;}
  return true;
}
sid('setup_scan_btn').addEventListener('click',setupScanWifi);
sid('setup_scan_list').addEventListener('change',function(){
  if(this.value){sid('setup_ssid').value=this.value;}
});
sid('setup_form').addEventListener('submit',function(e){
  if(!setupValidateNetworkFields()){
    e.preventDefault();
  }
});
function setupUpdateStaticFieldVisibility(){
  var staticMode=sid('setup_netmode').value==='static';
  sid('setup_static_ip_group').style.display=staticMode?'':'none';
  sid('setup_static_gw_group').style.display=staticMode?'':'none';
  sid('setup_static_mask_group').style.display=staticMode?'':'none';
}
sid('setup_netmode').addEventListener('change',setupUpdateStaticFieldVisibility);
setupUpdateStaticFieldVisibility();
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
