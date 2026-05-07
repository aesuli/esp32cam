#pragma once

// Still capture and MJPEG AVI recording implementation.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

static bool captureImageToSD(String &savedPath) {
  savedPath = "";

  {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked()) {
      return false;
    }

    if (!initSDCard() || !ensureCaptureDirectory()) {
      return false;
    }
  }

  if (!ensureCameraReady()) {
    return false;
  }

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return false;
  }

  OwnedJpegFrame frame;
  bool copied = copyCameraFrame(fb, frame);
  unlockCameraFrame(fb);
  if (!copied) {
    return false;
  }

  ensureClockBeforeTimestamp();

  uint32_t sequence = 0;
  {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked()) {
      return false;
    }

    if (!initSDCard() || !ensureCaptureDirectory() || !nextCaptureSequence(sequence)) {
      return false;
    }
  }

  savedPath = buildCapturePath(sequence, "jpg");

  bool ok = false;
  {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked()) {
      return false;
    }

    File file = SD_MMC.open(savedPath, FILE_WRITE);
    if (!file) {
      return false;
    }

    ok = file.write(frame.data, frame.len) == frame.len;
    file.close();

    if (!ok) {
      SD_MMC.remove(savedPath);
    }
  }

  return ok;
}

static void handleCaptureSD() {
  if (!checkAuth()) return;

  String photoPath;
  if (!captureImageToSD(photoPath)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to capture to SD");
    return;
  }

  server.send(HTTP_OK, "text/plain", String("Saved: ") + photoPath);
}

static bool writeAviHeader(File &file, uint32_t riffSize, uint32_t durationMs, uint32_t frameCount, uint32_t maxFrameSize, uint16_t width, uint16_t height, uint32_t moviListSize, bool hasIndex) {
  // ESP32-CAM can emit JPEG frames directly, so we keep recordings as MJPG in
  // an AVI container. This is the simplest standards-compliant output we can
  // generate here, but Android's documented native video support favors MP4/
  // WebM containers, so playback on phones may still depend on the app used.
  if (durationMs == 0U) {
    durationMs = frameCount == 0U ? 1U : (frameCount * RECORDING_FRAME_INTERVAL_MS);
  }

  uint64_t totalMicroseconds = (uint64_t)durationMs * 1000ULL;
  uint32_t microsecondsPerFrame = frameCount == 0U
    ? 0U
    : (uint32_t)((totalMicroseconds + (frameCount / 2ULL)) / (uint64_t)frameCount);
  if (frameCount != 0U && microsecondsPerFrame == 0U) {
    microsecondsPerFrame = 1U;
  }

  uint32_t moviPayloadSize = moviListSize >= 4U ? (moviListSize - 4U) : 0U;
  uint32_t bytesPerSecond = durationMs == 0U
    ? 0U
    : (uint32_t)((((uint64_t)moviPayloadSize * 1000ULL) + (durationMs / 2ULL)) / (uint64_t)durationMs);
  uint32_t imageSize = maxFrameSize == 0U
    ? (uint32_t)width * (uint32_t)height * 3UL
    : maxFrameSize;
  uint32_t scale = durationMs;
  uint64_t rawRate = (uint64_t)frameCount * 1000ULL;
  uint32_t rate = rawRate > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)rawRate;

  if (scale == 0U || rate == 0U) {
    scale = 1U;
    rate = 1U;
  } else {
    uint32_t divisor = gcdU32(scale, rate);
    scale /= divisor;
    rate /= divisor;
  }

  if (!file.seek(0)) {
    return false;
  }

  return
    writeFourCC(file, "RIFF") &&
    writeU32LE(file, riffSize) &&
    writeFourCC(file, "AVI ") &&
    writeFourCC(file, "LIST") &&
    writeU32LE(file, 192) &&
    writeFourCC(file, "hdrl") &&
    writeFourCC(file, "avih") &&
    writeU32LE(file, 56) &&
    writeU32LE(file, microsecondsPerFrame) &&
    writeU32LE(file, bytesPerSecond) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, hasIndex ? AVI_HAS_INDEX_FLAG : 0U) &&
    writeU32LE(file, frameCount) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 1) &&
    writeU32LE(file, maxFrameSize) &&
    writeU32LE(file, width) &&
    writeU32LE(file, height) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeFourCC(file, "LIST") &&
    writeU32LE(file, 116) &&
    writeFourCC(file, "strl") &&
    writeFourCC(file, "strh") &&
    writeU32LE(file, 56) &&
    writeFourCC(file, "vids") &&
    writeFourCC(file, "MJPG") &&
    writeU32LE(file, 0) &&
    writeU16LE(file, 0) &&
    writeU16LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, scale) &&
    writeU32LE(file, rate) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, frameCount) &&
    writeU32LE(file, maxFrameSize) &&
    writeU32LE(file, 0xFFFFFFFFUL) &&
    writeU32LE(file, 0) &&
    writeU16LE(file, 0) &&
    writeU16LE(file, 0) &&
    writeU16LE(file, width) &&
    writeU16LE(file, height) &&
    writeFourCC(file, "strf") &&
    writeU32LE(file, 40) &&
    writeU32LE(file, 40) &&
    writeU32LE(file, width) &&
    writeU32LE(file, height) &&
    writeU16LE(file, 1) &&
    writeU16LE(file, 24) &&
    writeFourCC(file, "MJPG") &&
    writeU32LE(file, imageSize) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeU32LE(file, 0) &&
    writeFourCC(file, "LIST") &&
    writeU32LE(file, moviListSize) &&
    writeFourCC(file, "movi");
}

static bool finalizeRecordingFile() {
  if (!recordingFile || recordingFrameCount == 0 || recordingWidth == 0 || recordingHeight == 0) {
    return false;
  }

  uint32_t durationMs = recordingDurationMs;
  if (durationMs == 0U) {
    unsigned long elapsedMs = millis() - recordingStartTime;
    durationMs = elapsedMs == 0UL ? 1U : (uint32_t)elapsedMs;
  }

  bool writeIndex = recordingIndexEnabled && recordingIndex.size() == recordingFrameCount;
  if (writeIndex) {
    uint32_t indexSize = (uint32_t)recordingIndex.size() * 16UL;
    if (!recordingFile.seek(recordingFile.size())) {
      return false;
    }

    if (!writeAviChunkHeader(recordingFile, "idx1", indexSize)) {
      return false;
    }

    for (size_t i = 0; i < recordingIndex.size(); ++i) {
      if (!writeFourCC(recordingFile, AVI_VIDEO_CHUNK_ID) ||
          !writeU32LE(recordingFile, AVI_KEYFRAME_FLAG) ||
          !writeU32LE(recordingFile, recordingIndex[i].offset) ||
          !writeU32LE(recordingFile, recordingIndex[i].size)) {
        return false;
      }
    }
  } else if (recordingIndexEnabled) {
    disableRecordingIndex("index/frame mismatch");
  }

  recordingFile.flush();

  uint32_t riffSize = (uint32_t)recordingFile.size() - 8UL;
  if (!writeAviHeader(recordingFile, riffSize, durationMs, recordingFrameCount, recordingMaxFrameSize, recordingWidth, recordingHeight, recordingMoviListSize, writeIndex)) {
    return false;
  }

  recordingFile.flush();
  return true;
}

static void stopRecordingSession(bool keepFile) {
  bool finalized = true;
  if (keepFile) {
    finalized = finalizeRecordingFile();
  }

  if (recordingFile) {
    recordingFile.close();
  }

  if ((!keepFile || !finalized) && !recordingPath.isEmpty()) {
    SD_MMC.remove(recordingPath);
  }

  resetRecordingState();
}

static bool appendRecordingFrame(const OwnedJpegFrame &frame) {
  if (!recordingFile || !frame.data || frame.len == 0U) {
    return false;
  }

  if (recordingWidth == 0 || recordingHeight == 0) {
    recordingWidth = frame.width;
    recordingHeight = frame.height;
  }
  if (frame.len > 0xFFFFFFFFUL) {
    return false;
  }
  recordingMaxFrameSize = std::max(recordingMaxFrameSize, (uint32_t)frame.len);

  AviIndexEntry entry;
  // idx1 offsets are relative to the start of the movi list payload, whose
  // first four bytes are the literal "movi" tag.
  entry.offset = recordingMoviListSize;
  entry.size = (uint32_t)frame.len;

  size_t padding = frame.len & 1U;
  uint32_t chunkSpan = 8U + (uint32_t)frame.len + (uint32_t)padding;
  if (0xFFFFFFFFUL - recordingMoviListSize < chunkSpan) {
    Serial.println("[REC] AVI size limit reached; stopping recording");
    return false;
  }

  if (!writeAviChunkHeader(recordingFile, AVI_VIDEO_CHUNK_ID, entry.size)) {
    return false;
  }
  if (!writeMjpegFramePayload(recordingFile, frame.data, frame.len)) {
    return false;
  }

  if (padding != 0U) {
    uint8_t zero = 0;
    if (recordingFile.write(&zero, 1) != 1) {
      return false;
    }
  }

  if (canStoreRecordingIndexEntry()) {
    recordingIndex.push_back(entry);
  }
  recordingMoviListSize += chunkSpan;

  return true;
}

static bool isDeviceBusy() {
  return recordingActive || motionPendingImages > 0 || motionVideoManagedRecording || streamClientConnected;
}

static bool recordFrameIfDue(const OwnedJpegFrame &frame, unsigned long now) {
  if (!frame.data || frame.len == 0U || !recordingMutex || !sdMutex) {
    return false;
  }

  bool ok = false;
  SemaphoreLock recordingLock(recordingMutex, pdMS_TO_TICKS(RECORDING_SHORT_LOCK_TIMEOUT_MS));
  if (!recordingLock.locked()) {
    return false;
  }

  if (recordingActive &&
      (recordingLastFrameAt == 0 || (now - recordingLastFrameAt) >= RECORDING_FRAME_INTERVAL_MS)) {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_SHORT_LOCK_TIMEOUT_MS));
    if (sdLock.locked()) {
      ok = appendRecordingFrame(frame);
      if (ok) {
        recordingLastFrameAt = now;
        ++recordingFrameCount;
        if ((recordingFrameCount % 10U) == 0U) {
          recordingFile.flush();
        }
      } else {
        Serial.println("[REC] Failed to write frame; aborting recording");
        stopRecordingSession(false);
      }
    }
  }

  return ok;
}

static void serviceRecording() {
  if (!recordingActive || streamClientConnected) {
    return;
  }

  unsigned long now = millis();
  if (!isRecordingFrameDue(now)) {
    return;
  }

  if (!ensureCameraReady(pdMS_TO_TICKS(1000))) {
    return;
  }

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return;
  }

  OwnedJpegFrame frame;
  bool copied = copyCameraFrame(fb, frame);
  unlockCameraFrame(fb);
  if (!copied) {
    return;
  }

  recordFrameIfDue(frame, now);
}

static bool startRecordingSessionInternal(String &message) {
  {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked()) {
      message = "SD card busy";
      return false;
    }

    if (!initSDCard()) {
      message = ERR_SD_CARD_NOT_AVAILABLE;
      return false;
    }
  }

  if (!ensureCameraReady()) {
    message = "Camera unavailable";
    return false;
  }

  ensureClockBeforeTimestamp();

  SemaphoreLock recordingLock(recordingMutex, pdMS_TO_TICKS(RECORDING_LONG_LOCK_TIMEOUT_MS));
  if (!recordingLock.locked()) {
    message = "Recording lock unavailable";
    return false;
  }

  bool ok = false;
  if (recordingActive) {
    message = "Recording already in progress";
  } else {
    ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
    if (!sdLock.locked()) {
      message = "SD card busy";
    } else if (!initSDCard()) {
      message = ERR_SD_CARD_NOT_AVAILABLE;
    } else if (!ensureCaptureDirectory()) {
      message = "Failed to create capture directory";
    } else {
      uint32_t sequence = 0;
      if (!nextCaptureSequence(sequence)) {
        message = "Failed to update capture sequence";
      } else {
        String path = buildCapturePath(sequence, "avi");
        if (!beginRecordingFile(path)) {
          message = "Failed to initialize AVI recording file";
        } else {
          message = String("Recording started: ") + recordingPath;
          ok = true;
        }
      }
    }
  }

  return ok;
}

static bool stopRecordingSessionInternal(String &message) {
  SemaphoreLock recordingLock(recordingMutex, pdMS_TO_TICKS(RECORDING_LONG_LOCK_TIMEOUT_MS));
  if (!recordingLock.locked()) {
    message = "Recording lock unavailable";
    return false;
  }

  bool ok = false;
  if (!recordingActive) {
    message = "No recording in progress";
  } else {
    unsigned long duration = millis() - recordingStartTime;
    recordingDurationMs = duration == 0UL ? 1U : (uint32_t)duration;

    bool keepFile = recordingFrameCount > 0;
    String savedPath = recordingPath;
    uint32_t savedFrameCount = recordingFrameCount;
    {
      ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
      if (!sdLock.locked()) {
        message = "SD card busy";
        return false;
      }

      stopRecordingSession(keepFile);
      if (!keepFile) {
        savedPath = "";
      }
    }

    message = "Recording stopped. Duration: " + String(duration / 1000) + "s, Frames: " + String(savedFrameCount);
    if (keepFile) {
      message += ", Saved: " + savedPath;
    } else {
      message += ". No frames captured.";
    }
    ok = true;
  }

  return ok;
}

static void handleRecordStart() {
  if (!checkAuth()) return;

  String message;
  bool ok = startRecordingSessionInternal(message);
  server.send(ok ? 200 : 400, "text/plain", message);
}

static void handleRecordStop() {
  if (!checkAuth()) return;

  String message;
  bool ok = stopRecordingSessionInternal(message);
  server.send(ok ? 200 : 400, "text/plain", message);
}

static void registerRecordingRoutes() {
  server.on("/capture", HTTP_GET, handleCaptureSD);
  server.on("/record/start", HTTP_POST, handleRecordStart);
  server.on("/record/stop", HTTP_POST, handleRecordStop);
}
