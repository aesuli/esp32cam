#pragma once

// Motion, PIR, and motion-route handlers.
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
  // Keep PIR line biased low while awake; deep sleep also applies RTC pulldown.
  pinMode(PIR_GPIO, INPUT_PULLDOWN);
  Logger.Log("[GPIO] PIR mode applied on GPIO%d: INPUT\n", PIR_GPIO); // PIR now on GPIO13
}

static void restoreInputPinsAfterSDInit() {
  applyPirInputMode();
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
}

static void resetMotionDetectionState() {
  motionRawHigh = false;
  motionLatched = false;
  motionEdgePending = false;
  motionHighSinceAt = 0;
  motionLastDetectedAt = 0;
  motionActionWindowActive = false;
  motionNotifyPending = false;
  motionNotifyLastAttemptAt = 0;
  motionIgnoreUntilAt = 0;
  motionEnableActivationAt = 0;
  motionPendingImages = 0;
  motionVideoManagedRecording = false;
  motionRecordingStopAt = 0;
}

static void scheduleMotionActivationDelay(const char *reason) {
  if (!runtimeConfig.motionSettings.enabled) {
    motionEnableActivationAt = 0;
    return;
  }

  motionEnableActivationAt = millis() + MOTION_ENABLE_ACTIVATION_DELAY_MS;
  motionLatched = false;
  motionHighSinceAt = 0;
  noInterrupts();
  motionEdgePending = false;
  interrupts();

  Logger.Log("[MOTION] Activation delay started (%lus) after %s\n",
             (unsigned long)(MOTION_ENABLE_ACTIVATION_DELAY_MS / 1000UL),
             reason ? reason : "enable");
}

static void updateSdLoggingState() {
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

  sendAppHtmlWithToken(MOTION_HTML, AppPage::Motion, "Motion");
}

static void handleMotionGraphPage() {
  if (!checkAuth()) {
    return;
  }

  sendAppHtmlWithToken(MOTION_GRAPH_HTML, AppPage::Motion, "Motion Graph");
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

static void handleMotionStandbyNow() {
  if (!checkAuth()) return;

  server.send(HTTP_OK, "text/plain", "Standby requested. Entering deep sleep...");
  requestDeepStandby("manual", true);
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
  json += "\"detectionIntervalSec\":" + String((int)m.detectionIntervalSec) + ",";
  json += "\"notifyUrl\":\"" + notifyUrlEscaped + "\",";
  json += "\"notifyEnabled\":" + String(m.notifyEnabled ? "true" : "false") + ",";
  json += "\"standbyAfterInactivity\":" + String(m.standbyAfterInactivity ? "true" : "false");
  json += "}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleMotionConfigSet() {
  if (!checkAuth()) return;

  bool wasEnabled = runtimeConfig.motionSettings.enabled;
  MotionSettings updated = runtimeConfig.motionSettings;
  if (server.hasArg("enabled")) updated.enabled = server.arg("enabled") == "1" || server.arg("enabled") == "true";
  if (server.hasArg("captureImage")) updated.captureImage = server.arg("captureImage") == "1" || server.arg("captureImage") == "true";
  if (server.hasArg("imageCount")) updated.imageCount = (uint8_t)server.arg("imageCount").toInt();
  if (server.hasArg("imageDelayDs")) updated.imageDelayDs = (uint8_t)server.arg("imageDelayDs").toInt();
  if (server.hasArg("captureVideo")) updated.captureVideo = server.arg("captureVideo") == "1" || server.arg("captureVideo") == "true";
  if (server.hasArg("videoDurationSec")) updated.videoDurationSec = (uint8_t)server.arg("videoDurationSec").toInt();
  if (server.hasArg("detectionIntervalSec")) updated.detectionIntervalSec = (uint16_t)server.arg("detectionIntervalSec").toInt();
  if (server.hasArg("notifyUrl")) updated.notifyUrl = server.arg("notifyUrl");
  if (server.hasArg("notifyEnabled")) updated.notifyEnabled = server.arg("notifyEnabled") == "1" || server.arg("notifyEnabled") == "true";
  if (server.hasArg("standbyAfterInactivity")) {
    updated.standbyAfterInactivity = server.arg("standbyAfterInactivity") == "1" || server.arg("standbyAfterInactivity") == "true";
  }

  clampMotionSettings(updated);
  runtimeConfig.motionSettings = updated;

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save motion configuration");
    return;
  }

  resetMotionDetectionState();
  applyPirInputMode();
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
  if (!wasEnabled && runtimeConfig.motionSettings.enabled) {
    scheduleMotionActivationDelay("web-config");
  }
  server.send(HTTP_OK, "text/plain", "Motion configuration saved");
}

static void registerMotionRoutes() {
  server.on("/motion", HTTP_GET, handleMotionPage);
  server.on("/motion/graph", HTTP_GET, handleMotionGraphPage);
  server.on("/motion/config", HTTP_GET, handleMotionConfigGet);
  server.on("/motion/config", HTTP_POST, handleMotionConfigSet);
  server.on("/motion/readings", HTTP_GET, handleMotionReadings);
  server.on("/motion/standby", HTTP_POST, handleMotionStandbyNow);
}

// ─── Motion runtime handling ────────────────────────────────────────────────

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

static void extendMotionRecording(unsigned long now, const char *source) {
  motionVideoManagedRecording = true;
  motionRecordingStopAt = now + ((unsigned long)runtimeConfig.motionSettings.videoDurationSec * 1000UL);
  motionLastDetectedAt = now;
  motionLastActivityAt = now;
  Logger.Log("[MOTION] Extended recording stop deadline by %s to %lus\n",
             source ? source : "motion",
             (unsigned long)runtimeConfig.motionSettings.videoDurationSec);
}

static void triggerMotionEvent(const char *source) {
  if (!runtimeConfig.motionSettings.enabled) {
    return;
  }

  unsigned long now = millis();
  if (recordingActive) {
    if (motionVideoManagedRecording && runtimeConfig.motionSettings.captureVideo) {
      extendMotionRecording(now, source);
    }
    return;
  }

  motionActionWindowActive = true;
  motionLastDetectedAt = now;
  motionLastActivityAt = now;
  motionPendingImages = 0;
  motionVideoManagedRecording = false;
  motionRecordingStopAt = 0;
  motionNotifyPending = runtimeConfig.motionSettings.notifyEnabled && !runtimeConfig.motionSettings.notifyUrl.isEmpty();
  motionNotifyLastAttemptAt = 0;
  Logger.Log("[MOTION] Triggered (%s)\n", source ? source : "runtime");

  if (runtimeConfig.motionSettings.captureImage) {
    motionPendingImages = runtimeConfig.motionSettings.imageCount;
    motionNextImageAt = now;
  }

  if (runtimeConfig.motionSettings.captureVideo) {
    String message;
    if (startRecordingSessionInternal(message)) {
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
  if (recordingActive && !motionVideoManagedRecording) {
    motionRawHigh = (digitalRead(PIR_GPIO) == HIGH);
    if (motionRawHigh) {
      if (motionHighSinceAt == 0) {
        motionHighSinceAt = now;
      }
    } else {
      motionHighSinceAt = 0;
    }

    noInterrupts();
    motionEdgePending = false;
    interrupts();
    return;
  }

  if (motionEnableActivationAt != 0) {
    if ((long)(now - motionEnableActivationAt) < 0) {
      motionRawHigh = (digitalRead(PIR_GPIO) == HIGH);
      motionLatched = false;
      noInterrupts();
      motionEdgePending = false;
      interrupts();
      return;
    }

    motionEnableActivationAt = 0;
    Logger.LogLine("[MOTION] Activation delay elapsed; detection armed");
  }

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

  if (edgeTriggered && recordingActive && motionVideoManagedRecording &&
      runtimeConfig.motionSettings.captureVideo) {
    motionLatched = true;
    motionHighSinceAt = now;
    triggerMotionEvent("pir-edge");
    return;
  }

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

static void serviceDeferredNetworkStartup() {
  if (!deferredNetworkStartupPending) {
    return;
  }

  if (!runtimeConfig.wifiEnabled) {
    deferredNetworkStartupPending = false;
    Logger.LogLine("[BOOT] Deferred network startup skipped: WiFi disabled");
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

  if (!runtimeConfig.motionSettings.notifyEnabled) {
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

static void serviceAutoStandby() {
  if (!runtimeConfig.motionSettings.standbyAfterInactivity || standbyPending) {
    return;
  }

  if (motionActionWindowActive || motionPendingImages > 0 || motionVideoManagedRecording || recordingActive || streamClientConnected) {
    return;
  }

  unsigned long now = millis();
  if ((now - motionLastActivityAt) >= STANDBY_INACTIVITY_TIMEOUT_MS) {
    requestDeepStandby("inactivity", false);
  }
}
