#pragma once

// Admin page and configuration-management routes.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

static void handleDeviceNameRename() {
  if (!checkAuth()) return;
  if (!server.hasArg("name")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Device name is required");
    return;
  }

  String newName = server.arg("name");
  if (newName.isEmpty() || newName.length() > 32) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Device name must be between 1 and 32 characters");
    return;
  }

  if (!isPrintableAscii(newName)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid characters in device name");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.deviceName = newName;
  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "Device name updated to: " + newName);
}

static void handleDeviceNameGet() {
  if (!checkAuth()) return;

  String currentName = cfgDeviceName;
  if (currentName.isEmpty()) {
    currentName = runtimeConfig.deviceName;
  }
  if (currentName.isEmpty()) {
    currentName = "ESP32-CAM";
  }

  String json = "{\"deviceName\":\"" + jsonEscape(currentName) + "\"}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(HTTP_OK, "application/json", json);
}

static void handleAdminTimeStatus() {
  if (!checkAuth()) return;

  time_t now = time(nullptr);
  String json = "{";
  json += "\"epoch\":" + String((unsigned long)now) + ",";
  json += "\"sane\":" + String(isClockSane() ? "true" : "false") + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"local\":\"" + jsonEscape(formatLocalTimeString()) + "\"";
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(HTTP_OK, "application/json", json);
}

static bool parseEpochArg(const String &raw, time_t &epochOut) {
  if (raw.isEmpty()) {
    return false;
  }

  uint64_t parsed = 0;
  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = (parsed * 10ULL) + (uint64_t)(c - '0');
    if (parsed > 0x7FFFFFFFULL) {
      return false;
    }
  }

  if (parsed < 946684800ULL) {  // 2000-01-01
    return false;
  }

  epochOut = (time_t)parsed;
  return true;
}

static void handleAdminTimeSet() {
  if (!checkAuth()) return;
  if (!server.hasArg("epoch")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "epoch is required");
    return;
  }

  time_t epoch = 0;
  if (!parseEpochArg(server.arg("epoch"), epoch)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid epoch value");
    return;
  }

  applyLocalTimeZone();

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to set system time");
    return;
  }

  server.send(HTTP_OK, "text/plain", "Time set to: " + formatLocalTimeString());
}

static void handleAdminTimeSync() {
  if (!checkAuth()) return;

  if (WiFi.status() != WL_CONNECTED) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "WiFi is not connected");
    return;
  }

  bool ok = syncClockWithNtp();
  if (!ok) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "NTP sync failed");
    return;
  }

  server.send(HTTP_OK, "text/plain", "NTP synced: " + formatLocalTimeString());
}

static void handleAdminLedGet() {
  if (!checkAuth()) return;

  String json = "{\"ledAccessBlink\":" + String(ledAccessBlinkEnabled ? "true" : "false") + "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleAdminLedSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("ledAccessBlink")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Missing ledAccessBlink parameter");
    return;
  }

  String value = server.arg("ledAccessBlink");
  bool newValue = (value == "1" || value == "true");

  // Update runtime config and save to SD
  runtimeConfig.ledAccessBlink = newValue;
  ledAccessBlinkEnabled = newValue;

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save LED configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "LED configuration saved");
}

static void handleAdminLoggingGet() {
  if (!checkAuth()) return;

  String json = "{\"loggingEnabled\":" + String(gLoggingEnabled ? "true" : "false") + "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleAdminLoggingSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("loggingEnabled")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Missing loggingEnabled parameter");
    return;
  }

  bool newValue = (server.arg("loggingEnabled") == "1" || server.arg("loggingEnabled") == "true");

  runtimeConfig.loggingEnabled = newValue;
  gLoggingEnabled = newValue;
  updateSdLoggingState();

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save logging configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "Logging configuration saved");
}

// Valid wifi_power_t raw values accepted from the UI
static bool isValidTxPowerValue(int v) {
  switch (v) {
    case WIFI_POWER_MINUS_1dBm:
    case WIFI_POWER_2dBm:
    case WIFI_POWER_5dBm:
    case WIFI_POWER_7dBm:
    case WIFI_POWER_8_5dBm:
    case WIFI_POWER_11dBm:
    case WIFI_POWER_13dBm:
    case WIFI_POWER_15dBm:
    case WIFI_POWER_17dBm:
    case WIFI_POWER_18_5dBm:
    case WIFI_POWER_19dBm:
    case WIFI_POWER_19_5dBm:
      return true;
    default:
      return false;
  }
}

static void handleAdminTxPowerGet() {
  if (!checkAuth()) return;

  String json = "{\"txPowerSta\":" + String((int)runtimeConfig.txPowerSta) +
                ",\"txPowerAp\":"  + String((int)runtimeConfig.txPowerAp) + "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleAdminTxPowerSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("txPowerSta") || !server.hasArg("txPowerAp")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Missing txPowerSta or txPowerAp");
    return;
  }

  int newSta = server.arg("txPowerSta").toInt();
  int newAp  = server.arg("txPowerAp").toInt();

  if (!isValidTxPowerValue(newSta) || !isValidTxPowerValue(newAp)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid TX power value");
    return;
  }

  runtimeConfig.txPowerSta = (int8_t)newSta;
  runtimeConfig.txPowerAp  = (int8_t)newAp;

  // Apply immediately to the active interface
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_STA || mode == WIFI_AP_STA) {
    WiFi.setTxPower((wifi_power_t)newSta);
  } else if (mode == WIFI_AP) {
    WiFi.setTxPower((wifi_power_t)newAp);
  }

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save TX power configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "TX power saved");
}

static void handleAdminReset() {
  if (!checkAuth()) {
    return;
  }

  handleUrlAccess();
  adminRestartPending = true;
  adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
  server.send(HTTP_OK, "text/plain", "Restart requested. Device will reboot shortly.");
}

static void handleAdminFactoryReset() {
  if (!checkAuth()) {
    return;
  }

  handleUrlAccess();

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", ERR_SD_CARD_BUSY);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  if (SD_MMC.exists(CONFIG_FILE_PATH) && !SD_MMC.remove(CONFIG_FILE_PATH)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to delete configuration file");
    return;
  }

  sdLock.release();
  adminRestartPending = true;
  adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
  server.send(HTTP_OK, "text/plain", "Configuration deleted. Rebooting to setup mode shortly.");
}

static void handleAdminPage() {
  if (!checkAuth()) return;
  String page(ADMIN_HTML);
  page.replace("__FIRMWARE_VERSION__", FIRMWARE_VERSION_TEXT);
  page.replace("__FIRMWARE_BUILD__", FIRMWARE_BUILD_TEXT);
  sendAppHtmlWithToken(page, AppPage::Admin);
}

static void registerAdminRoutes() {
  server.on("/admin", HTTP_GET, handleAdminPage);
  server.on("/admin/password", HTTP_POST, handleAdminPasswordChange);
  server.on("/admin/rename", HTTP_POST, handleDeviceNameRename);
  server.on("/admin/name", HTTP_GET, handleDeviceNameGet);
  server.on("/admin/time", HTTP_GET, handleAdminTimeStatus);
  server.on("/admin/time/set", HTTP_POST, handleAdminTimeSet);
  server.on("/admin/time/sync", HTTP_POST, handleAdminTimeSync);
  server.on("/admin/led", HTTP_GET, handleAdminLedGet);
  server.on("/admin/led", HTTP_POST, handleAdminLedSet);
  server.on("/admin/logging", HTTP_GET, handleAdminLoggingGet);
  server.on("/admin/logging", HTTP_POST, handleAdminLoggingSet);
  server.on("/admin/txpower", HTTP_GET, handleAdminTxPowerGet);
  server.on("/admin/txpower", HTTP_POST, handleAdminTxPowerSet);
  server.on("/admin/reset", HTTP_POST, handleAdminReset);
  server.on("/admin/factory-reset", HTTP_POST, handleAdminFactoryReset);
  server.on("/wifi/list", HTTP_GET, handleWifiList);
  server.on("/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/wifi/delete", HTTP_POST, handleWifiDelete);
  server.on("/wifi/move", HTTP_POST, handleWifiMove);
}
