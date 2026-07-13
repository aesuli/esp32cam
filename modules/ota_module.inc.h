#pragma once

// OTA firmware upload routes and transfer-server workers.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

static void logOtaUpdateError(const char *context) {
  Logger.Log("[OTA] %s (Update error code=%u)\n",
             context ? context : "Update operation failed",
             (unsigned int)Update.getError());
}

static void handleFirmwareUploadDataWorker() {
  if (!cfgAccessPass.isEmpty()) {
    bool authorized = hasSharedAccessToken(transferServer)
      || transferServer.authenticate("admin", cfgAccessPass.c_str());
    if (!authorized) {
      firmwareUploadFailed = true;
      return;
    }
  }

  HTTPUpload &upload = transferServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    firmwareUploadFailed = false;
    firmwareUploadSuccess = false;
    firmwareRestartAt = 0;

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      logOtaUpdateError("Update.begin failed");
      firmwareUploadFailed = true;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareUploadFailed) {
      return;
    }

    size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      logOtaUpdateError("Update.write failed");
      firmwareUploadFailed = true;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (firmwareUploadFailed) {
      Update.abort();
      return;
    }

    if (!Update.end(true) || !Update.isFinished()) {
      logOtaUpdateError("Update.end failed");
      firmwareUploadFailed = true;
      return;
    }

    firmwareUploadSuccess = true;
    firmwareRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
    Logger.Log("[OTA] Firmware upload complete (%u bytes). Restart scheduled.\n", (unsigned int)upload.totalSize);
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareUploadFailed = true;
    firmwareUploadSuccess = false;
    firmwareRestartAt = 0;
    Logger.LogLine("[OTA] Firmware upload aborted");
  }
}

static void handleFirmwareUploadWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  transferServer.sendHeader("Connection", "close");

  if (firmwareUploadSuccess) {
    transferServer.send(HTTP_OK, "text/plain", "Firmware uploaded successfully. Device will reboot in a moment.");
    return;
  }

  if (firmwareUploadFailed) {
    transferServer.send(HTTP_INTERNAL_ERROR, "text/plain", "Firmware update failed. Check /log.txt for details.");
    firmwareUploadFailed = false;
    return;
  }

  transferServer.send(HTTP_BAD_REQUEST, "text/plain", "No firmware file provided");
}

static void handleFirmwareUploadMain() {
  if (!checkAuth(server)) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, "/admin/update", true));
  server.send(HTTP_TEMPORARY_REDIRECT, "text/plain", "Redirecting to transfer server");
}

static void registerOtaRoutes() {
  server.on("/admin/update", HTTP_POST, handleFirmwareUploadMain);
}

static void registerOtaTransferRoutes() {
  transferServer.on("/admin/update", HTTP_POST, handleFirmwareUploadWorker, handleFirmwareUploadDataWorker);
}
