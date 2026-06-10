#pragma once

// Intervalometer/timelapse routes and runtime cycle.
// Included directly by esp32cam.cpp so it can share static firmware state.

static bool parseUint32Arg(const String &raw, uint32_t &valueOut) {
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
    if (parsed > 0xFFFFFFFFULL) {
      return false;
    }
  }

  valueOut = (uint32_t)parsed;
  return true;
}

static uint32_t intervalometerUnitSecondsMultiplier(uint8_t unit) {
  switch (unit) {
    case 0: return 1U;
    case 1: return 60U;
    case 2: return 3600U;
    case 3: return 86400U;
    default: return 1U;
  }
}

static uint64_t intervalometerConfigToIntervalUs(const IntervalometerSettings &settings) {
  uint64_t seconds = (uint64_t)settings.intervalValue * (uint64_t)intervalometerUnitSecondsMultiplier(settings.intervalUnit);
  if (seconds == 0ULL) {
    seconds = 1ULL;
  }
  return seconds * 1000000ULL;
}

static bool ensureTimelapseDirectory(const String &path) {
  if (path.isEmpty() || path[0] != '/') {
    return false;
  }

  if (path == "/") {
    return true;
  }

  int pos = 1;
  while (pos <= (int)path.length()) {
    int slash = path.indexOf('/', pos);
    String part = (slash < 0) ? path : path.substring(0, slash);
    if (!part.isEmpty() && !SD_MMC.exists(part)) {
      if (!SD_MMC.mkdir(part)) {
        return false;
      }
    }

    if (slash < 0) {
      break;
    }
    pos = slash + 1;
  }

  return true;
}

static bool loadTimelapseSequence(uint32_t &valueOut) {
  valueOut = 0;
  if (!SD_MMC.exists(TIMELAPSE_COUNTER_FILE_PATH)) {
    return true;
  }

  File file = SD_MMC.open(TIMELAPSE_COUNTER_FILE_PATH, FILE_READ);
  if (!file) {
    return false;
  }

  String raw = file.readString();
  file.close();
  raw.trim();
  if (raw.isEmpty()) {
    return true;
  }

  return parseUint32Arg(raw, valueOut);
}

static bool saveTimelapseSequence(uint32_t value) {
  if (SD_MMC.exists(TIMELAPSE_COUNTER_FILE_PATH) && !SD_MMC.remove(TIMELAPSE_COUNTER_FILE_PATH)) {
    return false;
  }

  File file = SD_MMC.open(TIMELAPSE_COUNTER_FILE_PATH, FILE_WRITE);
  if (!file) {
    return false;
  }

  bool ok = file.print(value) > 0;
  file.close();
  return ok;
}

static bool nextTimelapseSequence(uint32_t &nextValue) {
  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    return false;
  }

  if (!initSDCard()) {
    return false;
  }

  uint32_t current = 0;
  if (!loadTimelapseSequence(current)) {
    return false;
  }

  nextValue = current + 1U;
  return saveTimelapseSequence(nextValue);
}

static String buildIntervalometerImagePath(uint32_t timelapseId, uint32_t imageNumber, uint8_t burstNumber) {
  String runDir = String(TIMELAPSE_DIRECTORY) + "/t-" + String(timelapseId);
  String stamp = buildTimestampFilenameToken();

  char filename[80];
  snprintf(filename, sizeof(filename), "i-%lu-%u-%s.jpg",
    (unsigned long)imageNumber,
    (unsigned int)burstNumber,
    stamp.c_str());

  return runDir + "/" + String(filename);
}

[[noreturn]] static void enterIntervalometerDeepSleep(uint64_t sleepUs) {
  if (sleepUs < 100000ULL) {
    sleepUs = 100000ULL;
  }

  Logger.Log("[TLM] Entering deep sleep for %lu ms\n", (unsigned long)(sleepUs / 1000ULL));

  streamClientAbortRequested = true;
  detachInterrupt(digitalPinToInterrupt(PIR_GPIO));

  if (recordingActive) {
    String stopMessage;
    (void)stopRecordingSessionInternal(stopMessage);
  }

  if (cameraInitialized) {
    esp_err_t camErr = esp_camera_deinit();
    if (camErr == ESP_OK) {
      cameraInitialized = false;
      powerDownCameraHardware();
    } else {
      Logger.Log("[TLM] Camera deinit failed: 0x%x\n", camErr);
    }
  } else {
    powerDownCameraHardware();
  }

  serviceLogFileFlush();

  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t wakeErr = esp_sleep_enable_timer_wakeup(sleepUs);
  if (wakeErr != ESP_OK) {
    Logger.Log("[TLM] Failed to configure timer wakeup: 0x%x\n", wakeErr);
    delay(200);
    ESP.restart();
  }

  delay(50);
  esp_deep_sleep_start();
  for (;;) {
    delay(1000);
  }
}

static bool runIntervalometerCaptureCycle() {
  if (!intervalometerRtcActive || intervalometerRtcTimelapseId == 0 || intervalometerRtcBurstCount < 1 || intervalometerRtcBurstCount > 10) {
    return false;
  }

  unsigned long cycleStartedAt = millis();
  uint32_t imageNumber = intervalometerRtcImageIndex + 1U;

  String runDir = String(TIMELAPSE_DIRECTORY) + "/t-" + String(intervalometerRtcTimelapseId);
  {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked() || !initSDCard() || !ensureTimelapseDirectory(runDir)) {
      Logger.LogLine("[TLM] Failed to prepare timelapse directory");
      return false;
    }
  }

    if (!ensureCameraReady()) {
      Logger.LogLine("[TLM] Failed to activate camera for timelapse capture");
      return false;
    }

    // Allow the camera AE to settle after the sensor is activated.
    delay(1000);

  bool allOk = true;
  for (uint8_t burst = 1; burst <= intervalometerRtcBurstCount; ++burst) {
    String path = buildIntervalometerImagePath(intervalometerRtcTimelapseId, imageNumber, burst);
    if (captureImageToPath(path)) {
      Logger.Log("[TLM] Saved: %s\n", path.c_str());
    } else {
      allOk = false;
      Logger.Log("[TLM] Capture failed: %s\n", path.c_str());
    }

    if (burst < intervalometerRtcBurstCount) {
      delay((unsigned long)intervalometerRtcBurstDelaySec * 1000UL);
    }
  }

  intervalometerRtcImageIndex = imageNumber;

  unsigned long elapsedMs = millis() - cycleStartedAt;
  uint64_t elapsedUs = (uint64_t)elapsedMs * 1000ULL;
  uint64_t sleepUs = (intervalometerRtcIntervalUs > elapsedUs)
    ? (intervalometerRtcIntervalUs - elapsedUs)
    : 100000ULL;

  if (!allOk) {
    Logger.LogLine("[TLM] One or more shots failed in this cycle");
  }

  enterIntervalometerDeepSleep(sleepUs);
}

static bool serviceIntervalometerStartupIfNeeded() {
  if (!intervalometerRtcActive) {
    return false;
  }

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause != ESP_SLEEP_WAKEUP_TIMER) {
    Logger.LogLine("[TLM] Clearing intervalometer state (not a timer wake)");
    intervalometerRtcActive = false;
    intervalometerRtcTimelapseId = 0;
    intervalometerRtcImageIndex = 0;
    intervalometerRtcIntervalUs = 60000000ULL;
    intervalometerRtcBurstCount = 1;
    intervalometerRtcBurstDelaySec = 1;
    return false;
  }

  Logger.Log("[TLM] Resume timelapse t-%lu image #%lu\n",
    (unsigned long)intervalometerRtcTimelapseId,
    (unsigned long)(intervalometerRtcImageIndex + 1U));

  (void)runIntervalometerCaptureCycle();
  return true;
}

static void handleIntervalometerPage() {
  if (!checkAuth()) return;
  sendAppHtmlWithToken(INTERVALOMETER_HTML, AppPage::Intervalometer, "Timelapse");
}

static void handleIntervalometerConfigGet() {
  if (!checkAuth()) return;

  clampIntervalometerSettings(runtimeConfig.intervalometerSettings);
  String json = "{";
  json += "\"intervalValue\":" + String(runtimeConfig.intervalometerSettings.intervalValue) + ",";
  json += "\"intervalUnit\":" + String(runtimeConfig.intervalometerSettings.intervalUnit) + ",";
  json += "\"burstCount\":" + String(runtimeConfig.intervalometerSettings.burstCount) + ",";
  json += "\"burstDelaySec\":" + String(runtimeConfig.intervalometerSettings.burstDelaySec) + ",";
  json += "\"active\":" + String(intervalometerRtcActive ? "true" : "false");
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(HTTP_OK, "application/json", json);
}

static bool parseIntervalometerArgs(IntervalometerSettings &settings, String &errorOut) {
  if (server.hasArg("intervalValue")) {
    uint32_t value = 0;
    if (!parseUint32Arg(server.arg("intervalValue"), value)) {
      errorOut = "Invalid interval value";
      return false;
    }
    settings.intervalValue = value;
  }

  if (server.hasArg("intervalUnit")) {
    uint32_t unit = 0;
    if (!parseUint32Arg(server.arg("intervalUnit"), unit) || unit > 255U) {
      errorOut = "Invalid interval unit";
      return false;
    }
    settings.intervalUnit = (uint8_t)unit;
  }

  if (server.hasArg("burstCount")) {
    uint32_t burst = 0;
    if (!parseUint32Arg(server.arg("burstCount"), burst) || burst > 255U) {
      errorOut = "Invalid burst count";
      return false;
    }
    settings.burstCount = (uint8_t)burst;
  }

  if (server.hasArg("burstDelaySec")) {
    uint32_t burstDelay = 0;
    if (!parseUint32Arg(server.arg("burstDelaySec"), burstDelay) || burstDelay > 255U) {
      errorOut = "Invalid burst delay";
      return false;
    }
    settings.burstDelaySec = (uint8_t)burstDelay;
  }

  clampIntervalometerSettings(settings);
  return true;
}

static void handleIntervalometerConfigSet() {
  if (!checkAuth()) return;

  IntervalometerSettings updated = runtimeConfig.intervalometerSettings;
  String parseError;
  if (!parseIntervalometerArgs(updated, parseError)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", parseError);
    return;
  }

  runtimeConfig.intervalometerSettings = updated;
  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save intervalometer configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "Intervalometer configuration saved");
}

static void handleIntervalometerStart() {
  if (!checkAuth()) return;

  if (recordingActive) {
    server.send(HTTP_CONFLICT, "text/plain", "Stop recording before starting intervalometer");
    return;
  }

  IntervalometerSettings updated = runtimeConfig.intervalometerSettings;
  String parseError;
  if (!parseIntervalometerArgs(updated, parseError)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", parseError);
    return;
  }

  runtimeConfig.intervalometerSettings = updated;
  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save intervalometer configuration");
    return;
  }

  uint32_t timelapseId = 0;
  if (!nextTimelapseSequence(timelapseId)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to reserve new timelapse sequence");
    return;
  }

  intervalometerRtcActive = true;
  intervalometerRtcTimelapseId = timelapseId;
  intervalometerRtcImageIndex = 0;
  intervalometerRtcIntervalUs = intervalometerConfigToIntervalUs(updated);
  intervalometerRtcBurstCount = updated.burstCount;
  intervalometerRtcBurstDelaySec = updated.burstDelaySec;

  server.send(HTTP_OK, "text/plain", "Intervalometer started. First burst is being captured now.");
  delay(120);

  (void)runIntervalometerCaptureCycle();
}

static void registerIntervalometerRoutes() {
  server.on("/intervalometer", HTTP_GET, handleIntervalometerPage);
  server.on("/intervalometer/config", HTTP_GET, handleIntervalometerConfigGet);
  server.on("/intervalometer/config", HTTP_POST, handleIntervalometerConfigSet);
  server.on("/intervalometer/start", HTTP_POST, handleIntervalometerStart);
}
