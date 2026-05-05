#pragma once

// OTA firmware upload routes and transfer-server workers.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

static void handleFirmwareUploadDataWorker() {
  if (!cfgAccessPass.isEmpty()) {
    bool authorized = hasSharedAccessToken(transferServer)
      || transferServer.authenticate("admin", cfgAccessPass.c_str());
    if (!authorized) {
      firmwareUploadFailed = true;
      return;
    }
    noteAuthenticatedWebActivity();
  }

  HTTPUpload &upload = transferServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    firmwareUploadFailed = false;
    firmwareUploadSuccess = false;
    firmwareRestartAt = 0;

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
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
      Update.printError(Serial);
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
      Update.printError(Serial);
      firmwareUploadFailed = true;
      return;
    }

    firmwareUploadSuccess = true;
    firmwareRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
    Serial.printf("[OTA] Firmware upload complete (%u bytes). Restart scheduled.\n", (unsigned int)upload.totalSize);
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareUploadFailed = true;
    firmwareUploadSuccess = false;
    firmwareRestartAt = 0;
    Serial.println("[OTA] Firmware upload aborted");
  }
}

static void handleFirmwareUploadWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(401, "text/plain", "Unauthorized");
    return;
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  transferServer.sendHeader("Connection", "close");

  if (firmwareUploadSuccess) {
    transferServer.send(200, "text/plain", "Firmware uploaded successfully. Device will reboot in a moment.");
    return;
  }

  if (firmwareUploadFailed) {
    transferServer.send(500, "text/plain", "Firmware update failed. Check serial log for details.");
    firmwareUploadFailed = false;
    return;
  }

  transferServer.send(400, "text/plain", "No firmware file provided");
}

static void handleFirmwareUploadMain() {
  if (!checkAuth(server)) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, "/admin/update", true));
  server.send(307, "text/plain", "Redirecting to transfer server");
}

static void registerOtaRoutes() {
  server.on("/admin/update", HTTP_POST, handleFirmwareUploadMain);
}

static void registerOtaTransferRoutes() {
  transferServer.on("/admin/update", HTTP_POST, handleFirmwareUploadWorker, handleFirmwareUploadDataWorker);
}
