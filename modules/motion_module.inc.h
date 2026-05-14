#pragma once

// Motion, PIR, wake/sleep, and motion-route handlers.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

static bool isValidMotionIntervalSec(uint16_t seconds) {
  return seconds == 0 || seconds == 5 || seconds == 10 || seconds == 30 || seconds == 60 || seconds == 600;
}

static void clampMotionSettings(MotionSettings &settings) {
  if (settings.imageCount < 1) settings.imageCount = 1;
  if (settings.imageCount > 10) settings.imageCount = 10;
  if (settings.imageDelayDs < 1) settings.imageDelayDs = 1;
  if (settings.imageDelayDs > 20) settings.imageDelayDs = 20;
  if (settings.videoDurationSec < 1) settings.videoDurationSec = 1;
  if (settings.videoDurationSec > 30) settings.videoDurationSec = 30;
  if (settings.standbyAfterSec < 5) settings.standbyAfterSec = 5;
  if (settings.standbyAfterSec > 120) settings.standbyAfterSec = 120;
  if (!isValidMotionIntervalSec(settings.detectionIntervalSec)) {
    settings.detectionIntervalSec = 0;
  }

  settings.notifyUrl.trim();
  if (settings.notifyUrl.length() > 255) {
    settings.notifyUrl = settings.notifyUrl.substring(0, 255);
    settings.notifyUrl.trim();
  }
}

static void applyPirInputMode() {
  // Keep PIR input high-impedance; internal pull resistors can mask weak module outputs,
  // especially on shared GPIO12/SD lines.
  pinMode(PIR_GPIO, INPUT);
  Logger.Log("[GPIO] PIR mode applied on GPIO%d: INPUT\n", PIR_GPIO);
}

static void restoreInputPinsAfterSDInit() {
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  applyPirInputMode();
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
}

static void resetMotionDetectionState() {
  motionRawHigh = false;
  motionLatched = false;
  motionBootEventPending = false;
  motionEdgePending = false;
  motionHighSinceAt = 0;
  motionLastDetectedAt = 0;
  motionActionWindowActive = false;
  motionNotifyPending = false;
  motionNotifyLastAttemptAt = 0;
  motionIgnoreUntilAt = 0;
  motionPendingImages = 0;
  motionVideoManagedRecording = false;
  motionRecordingStopAt = 0;
}

static void updateSdLoggingState() {
  gLogSerialEnabled = runtimeConfig.logSerialEnabled;
  gLogFileEnabled = runtimeConfig.logFileEnabled;
}

static void IRAM_ATTR onPirEdgeInterrupt() {
  motionEdgePending = true;
  ++motionEdgeCount;
}

// ─── Motion route handlers ───────────────────────────────────────────────────

static void handleMotionPage() {
  if (!checkAuth()) {
    return;
  }

  sendAppHtmlWithToken(MOTION_HTML, AppPage::Motion);
}

static void handleMotionGraphPage() {
  if (!checkAuth()) {
    return;
  }

  sendAppHtmlWithToken(MOTION_GRAPH_HTML, AppPage::Motion);
}

static void handleMotionReadings() {
  if (!checkAuth()) return;

  unsigned long now = millis();
  unsigned long highDurationMs = 0;
  if (motionRawHigh && motionHighSinceAt != 0) {
    highDurationMs = now - motionHighSinceAt;
  }

  int signal = motionRawHigh ? 100 : 0;

  unsigned long sinceLast = motionLastDetectedAt == 0 ? 0 : (now - motionLastDetectedAt);
  uint32_t edgeCountSnapshot = 0;
  noInterrupts();
  edgeCountSnapshot = motionEdgeCount;
  interrupts();

  String json = "{";
  json += "\"enabled\":" + String(runtimeConfig.motionSettings.enabled ? "true" : "false") + ",";
  json += "\"rawHigh\":" + String(motionRawHigh ? "true" : "false") + ",";
  json += "\"latched\":" + String(motionLatched ? "true" : "false") + ",";
  json += "\"edgeCount\":" + String(edgeCountSnapshot) + ",";
  json += "\"signal\":" + String(signal) + ",";
  json += "\"highDurationMs\":" + String(highDurationMs) + ",";
  json += "\"sinceLastDetectedMs\":" + String(sinceLast);
  json += "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleMotionConfigGet() {
  if (!checkAuth()) return;

  clampMotionSettings(runtimeConfig.motionSettings);
  const MotionSettings &m = runtimeConfig.motionSettings;
  String json = "{";
  String notifyUrlEscaped = m.notifyUrl;
  notifyUrlEscaped.replace("\\", "\\\\");
  notifyUrlEscaped.replace("\"", "\\\"");
  json += "\"enabled\":" + String(m.enabled ? "true" : "false") + ",";
  json += "\"captureImage\":" + String(m.captureImage ? "true" : "false") + ",";
  json += "\"imageCount\":" + String((int)m.imageCount) + ",";
  json += "\"imageDelayDs\":" + String((int)m.imageDelayDs) + ",";
  json += "\"captureVideo\":" + String(m.captureVideo ? "true" : "false") + ",";
  json += "\"videoDurationSec\":" + String((int)m.videoDurationSec) + ",";
  json += "\"standbyButtonEnabled\":" + String(m.standbyButtonEnabled ? "true" : "false") + ",";
  json += "\"wakeOnMotion\":" + String(m.wakeOnMotion ? "true" : "false") + ",";
  json += "\"autoStandby\":" + String(m.autoStandby ? "true" : "false") + ",";
  json += "\"standbyAfterSec\":" + String((int)m.standbyAfterSec) + ",";
  json += "\"detectionIntervalSec\":" + String((int)m.detectionIntervalSec) + ",";
  json += "\"notifyUrl\":\"" + notifyUrlEscaped + "\"";
  json += "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleMotionConfigSet() {
  if (!checkAuth()) return;

  MotionSettings updated = runtimeConfig.motionSettings;
  if (server.hasArg("enabled")) updated.enabled = server.arg("enabled") == "1" || server.arg("enabled") == "true";
  if (server.hasArg("captureImage")) updated.captureImage = server.arg("captureImage") == "1" || server.arg("captureImage") == "true";
  if (server.hasArg("imageCount")) updated.imageCount = (uint8_t)server.arg("imageCount").toInt();
  if (server.hasArg("imageDelayDs")) updated.imageDelayDs = (uint8_t)server.arg("imageDelayDs").toInt();
  if (server.hasArg("captureVideo")) updated.captureVideo = server.arg("captureVideo") == "1" || server.arg("captureVideo") == "true";
  if (server.hasArg("videoDurationSec")) updated.videoDurationSec = (uint8_t)server.arg("videoDurationSec").toInt();
  if (server.hasArg("standbyButtonEnabled")) updated.standbyButtonEnabled = server.arg("standbyButtonEnabled") == "1" || server.arg("standbyButtonEnabled") == "true";
  if (server.hasArg("wakeOnMotion")) updated.wakeOnMotion = server.arg("wakeOnMotion") == "1" || server.arg("wakeOnMotion") == "true";
  if (server.hasArg("autoStandby")) updated.autoStandby = server.arg("autoStandby") == "1" || server.arg("autoStandby") == "true";
  if (server.hasArg("standbyAfterSec")) updated.standbyAfterSec = (uint16_t)server.arg("standbyAfterSec").toInt();
  if (server.hasArg("detectionIntervalSec")) updated.detectionIntervalSec = (uint16_t)server.arg("detectionIntervalSec").toInt();
  if (server.hasArg("notifyUrl")) updated.notifyUrl = server.arg("notifyUrl");

  clampMotionSettings(updated);
  runtimeConfig.motionSettings = updated;

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save motion configuration");
    return;
  }

  resetMotionDetectionState();
  configureMotionWakeup(runtimeConfig.motionSettings.wakeOnMotion);
  applyPirInputMode();
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
  server.send(HTTP_OK, "text/plain", "Motion configuration saved");
}

static void handleMotionStandby() {
  if (!checkAuth()) return;

  server.send(HTTP_OK, "text/plain", "Standby requested. Going to deep sleep now...");
  delay(120);
  enterDeepSleepNow("Standby requested from motion page", 0, true);
}

static void registerMotionRoutes() {
  server.on("/motion", HTTP_GET, handleMotionPage);
  server.on("/motion/graph", HTTP_GET, handleMotionGraphPage);
  server.on("/motion/config", HTTP_GET, handleMotionConfigGet);
  server.on("/motion/config", HTTP_POST, handleMotionConfigSet);
  server.on("/motion/standby", HTTP_POST, handleMotionStandby);
  server.on("/motion/readings", HTTP_GET, handleMotionReadings);
}

// ─── Motion runtime and sleep handling ───────────────────────────────────────

static void configureButtonWakeup() {
  esp_err_t err = esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_GPIO, 0);
  if (err != ESP_OK) {
    Logger.Log("[SLEEP] Failed to enable EXT0 wakeup on GPIO%d (err=0x%x)\n", BUTTON_GPIO, err);
    return;
  }

  rtc_gpio_pullup_en((gpio_num_t)BUTTON_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_GPIO);
  Logger.Log("[SLEEP] Wakeup source configured: button GPIO%d LOW\n", BUTTON_GPIO);
}

static void configureMotionWakeup(bool enabled) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
  if (!enabled) {
    Logger.LogLine("[SLEEP] Motion wakeup disabled");
    return;
  }

  uint64_t mask = (1ULL << PIR_GPIO);
  esp_err_t err = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (err != ESP_OK) {
    Logger.Log("[SLEEP] Failed to enable EXT1 wakeup on GPIO%d (err=0x%x)\n", PIR_GPIO, err);
    return;
  }

  rtc_gpio_pullup_dis((gpio_num_t)PIR_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)PIR_GPIO);
  Logger.Log("[SLEEP] Wakeup source configured: motion GPIO%d HIGH\n", PIR_GPIO);
}

static void handleWakeupIndicator() {
  if (bootWakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    Logger.Log("[BOOT] Wakeup from deep sleep via button GPIO%d\n", BUTTON_GPIO);
    ledBlinkCount(2, BUTTON_BLINK_ON_MS, BUTTON_BLINK_OFF_MS);
    return;
  }

  if (bootWakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    if ((mask & (1ULL << PIR_GPIO)) != 0ULL) {
      Logger.Log("[BOOT] Wakeup from deep sleep via motion GPIO%d\n", PIR_GPIO);
      motionBootEventPending = true;
      ledQuickBlink();
      return;
    }
  }

  ledBootSequence();
}

static void prepareDeviceForDeepSleep() {
  streamClientAbortRequested = true;

  SemaphoreLock recordingLock(recordingMutex, pdMS_TO_TICKS(RECORDING_LONG_LOCK_TIMEOUT_MS));
  if (recordingLock.locked()) {
    if (recordingActive) {
      Logger.LogLine("[SLEEP] Stopping active recording before deep sleep");
      ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
      if (sdLock.locked()) {
        stopRecordingSession(true);
      } else {
        Logger.LogLine("[SLEEP] SD card busy; recording may not be finalized");
      }
    }
  }

  digitalWrite(LED_FLASH_GPIO_NUM, LOW);
  flashEnabled = false;

  SemaphoreLock cameraLock(cameraMutex, pdMS_TO_TICKS(1500));
  if (cameraLock.locked()) {
    if (cameraInitialized) {
      esp_err_t err = esp_camera_deinit();
      if (err != ESP_OK) {
        Logger.Log("[SLEEP] Camera deinit failed: 0x%x\n", err);
      } else {
        cameraInitialized = false;
      }
    }
  }

  powerDownCameraHardware();
  setWifiModemSleep(false, "deep sleep");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
}

[[noreturn]] static void enterDeepSleepNow(const char *reason, int blinkCount, bool allowMotionWake) {
  Logger.Log("[SLEEP] %s\n", reason ? reason : "Entering deep sleep");
  prepareDeviceForDeepSleep();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
  configureButtonWakeup();
  if (allowMotionWake && runtimeConfig.motionSettings.wakeOnMotion) {
    configureMotionWakeup(true);
  }

  if (blinkCount > 0) {
    ledBlinkCount(blinkCount, BUTTON_BLINK_ON_MS, BUTTON_BLINK_OFF_MS);
  }
  delay(20);
  esp_deep_sleep_start();

  for (;;) {
    delay(1000);
  }
}

[[noreturn]] static void enterDeepSleepFromButton() {
  // Manual button sleep can also wake on motion when enabled in settings.
  enterDeepSleepNow("Button requested deep sleep", 3, true);
}

static void closeMotionActionWindow() {
  motionActionWindowActive = false;
  motionIgnoreUntilAt = millis() + ((unsigned long)runtimeConfig.motionSettings.detectionIntervalSec * 1000UL);

  if (motionNotifyPending) {
    if (sendMotionNotifyRequest(runtimeConfig.motionSettings.notifyUrl)) {
      motionNotifyPending = false;
      motionNotifyLastAttemptAt = 0;
    } else {
      motionNotifyLastAttemptAt = millis();
    }
  }
}

static bool sendMotionNotifyRequest(const String &url) {
  if (url.isEmpty()) {
    return true;
  }

  if (!staLinkUp || WiFi.status() != WL_CONNECTED) {
    Logger.LogLine("[MOTION] Notify deferred (WiFi not connected)");
    return false;
  }

  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    Logger.Log("[MOTION] Notify URL ignored (must start with http:// or https://): %s\n", url.c_str());
    return true;
  }

  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(url)) {
    Logger.Log("[MOTION] Notify request failed to begin: %s\n", url.c_str());
    return false;
  }

  int status = http.GET();
  if (status > 0) {
    Logger.Log("[MOTION] Notify GET status %d for %s\n", status, url.c_str());
  } else {
    Logger.Log("[MOTION] Notify GET error %d for %s\n", status, url.c_str());
  }
  http.end();
  return status > 0;
}

static void triggerMotionEvent(const char *source) {
  if (!runtimeConfig.motionSettings.enabled) {
    return;
  }

  unsigned long now = millis();
  motionActionWindowActive = true;
  motionLastDetectedAt = now;
  motionLastActivityAt = now;
  motionPendingImages = 0;
  motionVideoManagedRecording = false;
  motionRecordingStopAt = 0;
  motionNotifyPending = !runtimeConfig.motionSettings.notifyUrl.isEmpty();
  motionNotifyLastAttemptAt = 0;
  Logger.Log("[MOTION] Triggered (%s)\n", source ? source : "runtime");

  if (runtimeConfig.motionSettings.captureImage) {
    motionPendingImages = runtimeConfig.motionSettings.imageCount;
    motionNextImageAt = now;
  }

  if (runtimeConfig.motionSettings.captureVideo) {
    String message;
    if (recordingActive) {
      motionVideoManagedRecording = true;
      motionRecordingStopAt = now + ((unsigned long)runtimeConfig.motionSettings.videoDurationSec * 1000UL);
      Logger.Log("[MOTION] Extended recording stop deadline by motion to %lus\n", (unsigned long)runtimeConfig.motionSettings.videoDurationSec);
    } else if (startRecordingSessionInternal(message)) {
      motionVideoManagedRecording = true;
      motionRecordingStopAt = now + ((unsigned long)runtimeConfig.motionSettings.videoDurationSec * 1000UL);
      Logger.Log("[MOTION] %s\n", message.c_str());
    } else {
      Logger.Log("[MOTION] Failed to start recording: %s\n", message.c_str());
    }
  }

  if (motionPendingImages == 0 && !motionVideoManagedRecording) {
    closeMotionActionWindow();
  }
}

static void serviceButtonSleepRequest() {
  if (!runtimeConfig.motionSettings.standbyButtonEnabled) {
    buttonSleepRequestPending = false;
    return;
  }

  unsigned long now = millis();
  bool rawPressed = (digitalRead(BUTTON_GPIO) == LOW);

  if (rawPressed != buttonLastRawPressed) {
    buttonLastRawPressed = rawPressed;
    buttonLastChangeAt = now;
  }

  if ((now - buttonLastChangeAt) >= BUTTON_DEBOUNCE_MS && rawPressed != buttonStablePressed) {
    buttonStablePressed = rawPressed;

    if (!buttonStablePressed) {
      buttonSleepArmed = true;
      return;
    }
  }

  if ((now - buttonLastChangeAt) >= BUTTON_DEBOUNCE_MS &&
      buttonStablePressed && buttonSleepArmed && !buttonSleepRequestPending) {
    buttonSleepRequestPending = true;
    buttonSleepRequestAt = now;
    buttonSleepArmed = false;
    Logger.Log("[BUTTON] Sleep requested, entering deep sleep in %lu ms\n", BUTTON_SLEEP_DELAY_MS);
  }

  if (buttonSleepRequestPending && (now - buttonSleepRequestAt) >= BUTTON_SLEEP_DELAY_MS) {
    buttonSleepRequestPending = false;
    enterDeepSleepFromButton();
  }
}

static void serviceMotionDetection() {
  if (!runtimeConfig.motionSettings.enabled) {
    unsigned long now = millis();
    motionRawHigh = (digitalRead(PIR_GPIO) == HIGH);
    motionLatched = false;
    if (motionRawHigh) {
      if (motionHighSinceAt == 0) {
        motionHighSinceAt = now;
      }
    } else {
      motionHighSinceAt = 0;
    }
    closeMotionActionWindow();
    noInterrupts();
    motionEdgePending = false;
    interrupts();
    return;
  }

  unsigned long now = millis();
  if (motionIgnoreUntilAt != 0 && (long)(now - motionIgnoreUntilAt) >= 0) {
    motionIgnoreUntilAt = 0;
  }

  bool edgeTriggered = false;
  noInterrupts();
  if (motionEdgePending) {
    motionEdgePending = false;
    edgeTriggered = true;
  }
  interrupts();

  if (edgeTriggered && !motionActionWindowActive && motionIgnoreUntilAt == 0) {
    motionLatched = true;
    motionHighSinceAt = now;
    triggerMotionEvent("pir-edge");
    return;
  }

  bool rawHigh = (digitalRead(PIR_GPIO) == HIGH);
  motionRawHigh = rawHigh;

  if (rawHigh) {
    if (motionHighSinceAt == 0) {
      motionHighSinceAt = now;
    }

    // Ignore further detections while actions are running and during post-action cooldown.
    if (motionActionWindowActive || motionIgnoreUntilAt != 0) {
      return;
    }

    if (!motionLatched) {
      motionLatched = true;
      triggerMotionEvent("pir");
    }
  } else {
    motionHighSinceAt = 0;
    motionLatched = false;
  }
}

static void serviceMotionActions() {
  unsigned long now = millis();

  if (motionBootEventPending) {
    motionBootEventPending = false;
    triggerMotionEvent("wake");
  }

  if (motionPendingImages > 0 && now >= motionNextImageAt) {
    String path;
    if (captureImageToSD(path)) {
      Logger.Log("[MOTION] Image captured: %s\n", path.c_str());
    } else {
      Logger.LogLine("[MOTION] Failed to capture image");
    }

    --motionPendingImages;
    motionNextImageAt = now + ((unsigned long)runtimeConfig.motionSettings.imageDelayDs * 100UL);
  }

  if (motionVideoManagedRecording && motionRecordingStopAt != 0 && now >= motionRecordingStopAt) {
    String message;
    if (stopRecordingSessionInternal(message)) {
      Logger.Log("[MOTION] %s\n", message.c_str());
    }
    motionVideoManagedRecording = false;
    motionRecordingStopAt = 0;
  }

  if (motionActionWindowActive && motionPendingImages == 0 && !motionVideoManagedRecording) {
    closeMotionActionWindow();
  }
}

static void serviceMotionAutoStandby() {
  if (!runtimeConfig.motionSettings.enabled || !runtimeConfig.motionSettings.autoStandby) {
    return;
  }

  if (isDeviceBusy()) {
    return;
  }

  unsigned long now = millis();
  if ((now - motionLastActivityAt) >= ((unsigned long)runtimeConfig.motionSettings.standbyAfterSec * 1000UL)) {
    enterDeepSleepNow("Auto stand-by timeout", 0, true);
  }
}

static void serviceDeferredNetworkStartup() {
  if (!deferredNetworkStartupPending) {
    return;
  }

  if (motionActionWindowActive || motionPendingImages > 0 || motionVideoManagedRecording || recordingActive) {
    return;
  }

  deferredNetworkStartupPending = false;
  Logger.LogLine("[BOOT] Starting deferred network services after motion wake actions");

  if (isConfigured) {
    startSTAMode();
  } else {
    startSetupAPMode();
  }
}

static void serviceMotionNotifyRetry() {
  if (!motionNotifyPending) {
    return;
  }

  if (motionActionWindowActive || motionPendingImages > 0 || motionVideoManagedRecording) {
    return;
  }

  if (runtimeConfig.motionSettings.notifyUrl.isEmpty()) {
    motionNotifyPending = false;
    motionNotifyLastAttemptAt = 0;
    return;
  }

  if (!staLinkUp || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (motionNotifyLastAttemptAt != 0
      && (now - motionNotifyLastAttemptAt) < MOTION_NOTIFY_RETRY_INTERVAL_MS) {
    return;
  }

  motionNotifyLastAttemptAt = now;
  if (sendMotionNotifyRequest(runtimeConfig.motionSettings.notifyUrl)) {
    motionNotifyPending = false;
    motionNotifyLastAttemptAt = 0;
    Logger.LogLine("[MOTION] Deferred notify delivered");
  }
}
