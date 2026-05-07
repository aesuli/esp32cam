#pragma once

// SD browser, file transfer, upload, playback, and firmware upload routes.
// Included directly by esp32cam.cpp so it can share existing static firmware state.

// ─── SD and Page handlers ──────────────────────────────────────────────────────
static bool normalizeAndValidateSDPath(const String &inputPath, String &outPath) {
  if (inputPath.isEmpty()) {
    return false;
  }

  String path = inputPath;
  path.replace('\\', '/');
  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  if (path.indexOf("..") != -1 || path.indexOf("//") != -1) {
    return false;
  }

  outPath = path;
  return true;
}

static bool isValidSortByValue(const String &value) {
  return value == "name" || value == "size" || value == "type";
}

static bool isValidSortDirValue(const String &value) {
  return value == "asc" || value == "desc";
}

static void loadSDSortPreferences(String &sortBy, String &sortDir) {
  sortBy = "name";
  sortDir = "asc";

  if (!SD_MMC.exists(SD_SORT_FILE_PATH)) {
    return;
  }

  File file = SD_MMC.open(SD_SORT_FILE_PATH, FILE_READ);
  if (!file) {
    return;
  }

  String content = file.readString();
  file.close();

  int byPos = content.indexOf("by=");
  if (byPos >= 0) {
    int byStart = byPos + 3;
    int byEnd = content.indexOf('\n', byStart);
    String byValue = (byEnd >= 0) ? content.substring(byStart, byEnd) : content.substring(byStart);
    byValue.trim();
    if (isValidSortByValue(byValue)) {
      sortBy = byValue;
    }
  }

  int dirPos = content.indexOf("dir=");
  if (dirPos >= 0) {
    int dirStart = dirPos + 4;
    int dirEnd = content.indexOf('\n', dirStart);
    String dirValue = (dirEnd >= 0) ? content.substring(dirStart, dirEnd) : content.substring(dirStart);
    dirValue.trim();
    if (isValidSortDirValue(dirValue)) {
      sortDir = dirValue;
    }
  }
}

static bool saveSDSortPreferences(const String &sortBy, const String &sortDir) {
  File file = SD_MMC.open(SD_SORT_FILE_PATH, FILE_WRITE);
  if (!file) {
    return false;
  }

  String content = "by=" + sortBy + "\ndir=" + sortDir + "\n";
  size_t written = file.print(content);
  file.close();
  return written == content.length();
}

static bool validateNewSDName(const String &name) {
  if (name.isEmpty() || name == "." || name == "..") {
    return false;
  }

  if (name.indexOf('/') != -1 || name.indexOf('\\') != -1 || name.indexOf("..") != -1) {
    return false;
  }

  return true;
}

static bool isProtectedSDPath(const String &path) {
  return path == CONFIG_FILE_PATH || path == CAPTURE_COUNTER_FILE_PATH || path == SD_SORT_FILE_PATH;
}

static bool isHiddenSDPath(const String &path) {
  return path == SD_SORT_FILE_PATH;
}

static bool removeSDDirectoryRecursive(const String &dirPath, bool &blockedProtectedPath) {
  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String rawName = String(entry.name());
    String itemPath = rawName;
    if (!itemPath.startsWith("/")) {
      itemPath = (dirPath == "/") ? ("/" + rawName) : (dirPath + "/" + rawName);
    }

    String normalizedPath;
    if (!normalizeAndValidateSDPath(itemPath, normalizedPath)) {
      entry.close();
      dir.close();
      return false;
    }

    if (isProtectedSDPath(normalizedPath)) {
      blockedProtectedPath = true;
      entry.close();
      dir.close();
      return false;
    }

    bool isDir = entry.isDirectory();
    entry.close();

    if (isDir) {
      if (!removeSDDirectoryRecursive(normalizedPath, blockedProtectedPath)) {
        dir.close();
        return false;
      }
      if (!SD_MMC.rmdir(normalizedPath)) {
        dir.close();
        return false;
      }
    } else {
      if (!SD_MMC.remove(normalizedPath)) {
        dir.close();
        return false;
      }
    }

    entry = dir.openNextFile();
  }

  dir.close();
  return true;
}

static void handleSDPage() {
  if (!checkAuth()) return;
  sendAppHtmlWithToken(SD_HTML, AppPage::Sd);
}

static void handleSDList() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }

  String dirPath = "/";
  if (server.hasArg("dir")) {
    if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
      server.send(HTTP_BAD_REQUEST, "application/json", "{\"error\":\"Invalid directory path\"}");
      return;
    }
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "application/json", String("{\"error\":\"") + ERR_SD_CARD_BUSY + "\"}");
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "application/json", String("{\"error\":\"") + ERR_SD_CARD_NOT_AVAILABLE + "\"}");
    return;
  }

  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    server.send(HTTP_NOT_FOUND, "application/json", "{\"error\":\"Directory not found\"}");
    return;
  }

  String json = "{\"dir\":\"" + jsonEscape(dirPath) + "\",\"items\":[";
  bool first = true;

  File entry = dir.openNextFile();
  while (entry) {
    String rawName = String(entry.name());
    String itemPath = rawName;
    if (!itemPath.startsWith("/")) {
      if (dirPath == "/") {
        itemPath = "/" + rawName;
      } else {
        itemPath = dirPath + "/" + rawName;
      }
    }

    String normalizedPath;
    if (normalizeAndValidateSDPath(itemPath, normalizedPath)) {
      if (isProtectedSDPath(normalizedPath) || isHiddenSDPath(normalizedPath)) {
        entry.close();
        entry = dir.openNextFile();
        continue;
      }

      String itemName = normalizedPath;
      int slash = itemName.lastIndexOf('/');
      if (slash >= 0) {
        itemName = itemName.substring(slash + 1);
      }

      if (!first) {
        json += ",";
      }
      json += "{\"name\":\"" + jsonEscape(itemName) + "\",";
      json += "\"path\":\"" + jsonEscape(normalizedPath) + "\",";
      json += "\"isDir\":" + String(entry.isDirectory() ? "true" : "false") + ",";
      json += "\"size\":" + String((unsigned int)entry.size()) + "}";
      first = false;
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
  json += "]}";
  sdLock.release();
  server.send(HTTP_OK, "application/json", json);
}

static void sendTransferError(WebServer &srv, int statusCode, const char *message) {
  srv.sendHeader("Access-Control-Allow-Origin", "*");
  srv.send(statusCode, "text/plain", message);
}

static void handleSDDownloadWorker() {
  if (!checkAuth(transferServer, true)) {
    sendTransferError(transferServer, HTTP_UNAUTHORIZED, ERR_UNAUTHORIZED);
    return;
  }

  if (!transferServer.hasArg(PARAM_FILE)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg(PARAM_FILE), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_NOT_FOUND, "text/plain", ERR_FILE_NOT_FOUND);
    return;
  }

  String downloadName = filePath;
  int slash = downloadName.lastIndexOf('/');
  if (slash >= 0) {
    downloadName = downloadName.substring(slash + 1);
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  transferServer.streamFile(file, "application/octet-stream");
  file.close();
}

static void handleSDDownloadMain() {
  if (!checkAuth(server)) {
    return;
  }

  if (!server.hasArg(PARAM_FILE)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg(PARAM_FILE), filePath)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/download?file=") + urlEncode(filePath), true));
  server.send(HTTP_FOUND, "text/plain", "Redirecting to transfer server");
}

static String sdMimeTypeForPath(const String &filePath) {
  String lower = filePath;
  lower.toLowerCase();

  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".bmp")) return "image/bmp";
  if (lower.endsWith(".avi")) return "video/x-msvideo";
  if (lower.endsWith(".mp4")) return "video/mp4";
  if (lower.endsWith(".mjpg")) return "application/octet-stream";
  if (lower.endsWith(".txt") || lower.endsWith(".log") || lower.endsWith(".csv")) return "text/plain";
  if (lower.endsWith(".json")) return "application/json";
  return "application/octet-stream";
}

static bool sdIsVideoPath(const String &filePath) {
  String lower = filePath;
  lower.toLowerCase();
  return lower.endsWith(".avi")
      || lower.endsWith(".mp4")
      || lower.endsWith(".mjpg")
      || lower.endsWith(".mov")
      || lower.endsWith(".webm");
}

static bool readU32LE(File &file, uint32_t &value) {
  uint8_t bytes[4];
  if (file.read(bytes, sizeof(bytes)) != (int)sizeof(bytes)) {
    return false;
  }

  value = (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
  return true;
}

static bool skipChunkData(File &file, uint32_t chunkSize) {
  uint32_t skip = chunkSize + (chunkSize & 1U);
  uint32_t nextPos = (uint32_t)file.position() + skip;
  return file.seek(nextPos);
}

static void handleSDPlaybackWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!transferServer.hasArg(PARAM_FILE)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg(PARAM_FILE), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  String lower = filePath;
  lower.toLowerCase();
  if (!lower.endsWith(".avi")) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_UNSUPPORTED_MEDIA_TYPE, "text/plain", "Playback stream currently supports AVI MJPEG files only");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_NOT_FOUND, "text/plain", ERR_FILE_NOT_FOUND);
    return;
  }

  uint32_t fileSize = (uint32_t)file.size();
  if (fileSize < 16U || !file.seek(12U)) {
    file.close();
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", "Invalid AVI file");
    return;
  }

  bool moviFound = false;
  uint32_t moviStart = 0;
  uint32_t moviEnd = 0;

  while ((uint32_t)file.position() + 8U <= fileSize) {
    char chunkId[4];
    if (file.read((uint8_t *)chunkId, 4) != 4) {
      break;
    }

    uint32_t chunkSize = 0;
    if (!readU32LE(file, chunkSize)) {
      break;
    }

    if (memcmp(chunkId, "LIST", 4) == 0) {
      char listType[4];
      if (file.read((uint8_t *)listType, 4) != 4) {
        break;
      }

      if (memcmp(listType, "movi", 4) == 0) {
        uint32_t payloadSize = chunkSize >= 4U ? (chunkSize - 4U) : 0U;
        moviStart = (uint32_t)file.position();
        moviEnd = moviStart + payloadSize;
        if (moviEnd > fileSize) {
          moviEnd = fileSize;
        }
        moviFound = true;
        break;
      }

      if (chunkSize < 4U) {
        break;
      }

      uint32_t remaining = chunkSize - 4U;
      uint32_t skip = remaining + (chunkSize & 1U);
      if (!file.seek((uint32_t)file.position() + skip)) {
        break;
      }
    } else {
      if (!skipChunkData(file, chunkSize)) {
        break;
      }
    }
  }

  if (!moviFound || moviStart >= moviEnd || !file.seek(moviStart)) {
    file.close();
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", "Could not locate AVI movi data");
    return;
  }

  WiFiClient client = transferServer.client();
  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=jpgbound\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n"
  );

  uint8_t frameBuf[1024];
  while (client.connected() && (uint32_t)file.position() + 8U <= moviEnd) {
    char chunkId[4];
    if (file.read((uint8_t *)chunkId, 4) != 4) {
      break;
    }

    uint32_t chunkSize = 0;
    if (!readU32LE(file, chunkSize)) {
      break;
    }

    uint32_t dataStart = (uint32_t)file.position();
    uint32_t dataEnd = dataStart + chunkSize;
    if (dataEnd > moviEnd) {
      break;
    }

    bool isVideoChunk = memcmp(chunkId, AVI_VIDEO_CHUNK_ID, 4) == 0;
    if (isVideoChunk && chunkSize > 0U) {
      char partHeader[128];
      int hlen = snprintf(partHeader, sizeof(partHeader),
        "--jpgbound\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %u\r\n"
        "\r\n",
        (unsigned int)chunkSize);

      bool ok = client.write((const uint8_t *)partHeader, (size_t)hlen) == (size_t)hlen;
      uint32_t remaining = chunkSize;
      while (ok && remaining > 0U) {
        size_t toRead = remaining > sizeof(frameBuf) ? sizeof(frameBuf) : (size_t)remaining;
        int readNow = file.read(frameBuf, toRead);
        if (readNow <= 0) {
          ok = false;
          break;
        }
        if (client.write(frameBuf, (size_t)readNow) != (size_t)readNow) {
          ok = false;
          break;
        }
        remaining -= (uint32_t)readNow;
      }
      if (ok) {
        ok = client.print("\r\n") > 0;
      }

      if (!ok) {
        break;
      }

      if ((chunkSize & 1U) != 0U) {
        file.read();
      }
      delay(RECORDING_FRAME_INTERVAL_MS);
      continue;
    }

    if (!file.seek(dataEnd + (chunkSize & 1U))) {
      break;
    }
  }

  file.close();
  client.stop();
}

static void handleSDPlayerMain() {
  if (!checkAuth(server)) {
    return;
  }

  if (!server.hasArg(PARAM_FILE)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg(PARAM_FILE), filePath)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(HTTP_NOT_FOUND, "text/plain", ERR_FILE_NOT_FOUND);
    return;
  }
  file.close();

  if (!sdIsVideoPath(filePath)) {
    server.sendHeader("Location", String("/sd/view?file=") + urlEncode(filePath));
    server.send(HTTP_FOUND, "text/plain", "Redirecting to file view");
    return;
  }

  String mediaUrl = buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/view?file=") + urlEncode(filePath), true);
  String playbackUrl = buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/playback?file=") + urlEncode(filePath), true);
  String downloadUrl = buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/download?file=") + urlEncode(filePath), true);
  String mime = sdMimeTypeForPath(filePath);
  String lower = filePath;
  lower.toLowerCase();
  bool useImagePlayback = lower.endsWith(".avi");

  String filename = filePath;
  int slash = filename.lastIndexOf('/');
  if (slash >= 0) {
    filename = filename.substring(slash + 1);
  }

  String page = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - SD Video</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px}
header h1{color:#e94560;font-size:1.3em}
.wrap{max-width:1080px;margin:0 auto;padding:16px}
.panel{background:#16213e;border:1px solid #234573;border-radius:10px;padding:14px}
.title{color:#e94560;margin-bottom:10px}
.meta{font-size:.9em;color:#bbb;margin-bottom:10px}
video,img{width:100%;max-height:75vh;background:#000;border:1px solid #234573;border-radius:8px;display:block;object-fit:contain}
.hint{font-size:.85em;color:#bbb;margin-top:10px;line-height:1.4}
.actions{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
.actions a{background:#e94560;color:#fff;text-decoration:none;padding:8px 12px;border-radius:4px}
.actions a:hover{background:#c73652}
</style>
</head>
<body>
__APP_NAV__
<header><h1>💾 SD Video Viewer</h1></header>
<div class="wrap">
  <div class="panel">
    <h2 class="title">__FILENAME__</h2>
    <div class="meta">File: __FILENAME__</div>
    __PLAYER_MEDIA__
    <div class="actions">
      <a href="__MEDIA_URL__">Open Raw</a>
      <a href="__DOWNLOAD_URL__">Download</a>
    </div>
    <div class="hint">__PLAYER_HINT__</div>
  </div>
</div>
__APP_FOOTER__
</body>
</html>)html";

  page.replace("__FILENAME__", filename);
  page.replace("__MEDIA_URL__", mediaUrl);
  page.replace("__DOWNLOAD_URL__", downloadUrl);
  if (useImagePlayback) {
    page.replace("__PLAYER_MEDIA__", "<img src=\"" + playbackUrl + "\" alt=\"AVI playback\">");
    page.replace("__PLAYER_HINT__", "AVI playback is rendered as MJPEG frames (stream style), similar to the live camera view.");
  } else {
    page.replace("__PLAYER_MEDIA__", "<video controls playsinline preload=\"metadata\" src=\"" + mediaUrl + "\" type=\"" + mime + "\"></video>");
    page.replace("__PLAYER_HINT__", "If playback does not start, your browser likely does not support this container/codec and may require download instead.");
  }
  applyAppChrome(page, AppPage::Sd);
  server.send(HTTP_OK, "text/html", page);
}

static void handleSDViewWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!transferServer.hasArg(PARAM_FILE)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg(PARAM_FILE), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_NOT_FOUND, "text/plain", ERR_FILE_NOT_FOUND);
    return;
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  transferServer.streamFile(file, sdMimeTypeForPath(filePath));
  file.close();
}

static void handleSDViewMain() {
  if (!checkAuth(server)) {
    return;
  }

  if (!server.hasArg(PARAM_FILE)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg(PARAM_FILE), filePath)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
    return;
  }

  if (isProtectedSDPath(filePath)) {
    server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/view?file=") + urlEncode(filePath), true));
  server.send(HTTP_FOUND, "text/plain", "Redirecting to transfer server");
}

static void handleSDDelete() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!server.hasArg(PARAM_FILE)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  std::vector<String> filePaths;
  filePaths.reserve(server.args());
  for (int index = 0; index < server.args(); ++index) {
    if (server.argName(index) != PARAM_FILE) {
      continue;
    }

    String filePath;
    if (!normalizeAndValidateSDPath(server.arg(index), filePath)) {
      server.send(HTTP_BAD_REQUEST, "text/plain", ERR_INVALID_PATH);
      return;
    }

    if (isProtectedSDPath(filePath)) {
      server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
      return;
    }

    if (std::find(filePaths.begin(), filePaths.end(), filePath) == filePaths.end()) {
      filePaths.push_back(filePath);
    }
  }

  if (filePaths.empty()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", ERR_FILE_REQUIRED);
    return;
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", ERR_SD_CARD_BUSY);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  for (const String &filePath : filePaths) {
    if (!SD_MMC.remove(filePath)) {
      server.send(HTTP_INTERNAL_ERROR, "text/plain", filePaths.size() > 1 ? "Failed to delete one or more files" : "Failed to delete file");
      return;
    }
  }

  sdLock.release();
  if (filePaths.size() == 1) {
    server.send(HTTP_OK, "text/plain", "File deleted");
  } else {
    server.send(HTTP_OK, "text/plain", String(filePaths.size()) + " files deleted");
  }
}

static void handleSDSortGet() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "application/json", String("{\"error\":\"") + ERR_SD_CARD_BUSY + "\"}");
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "application/json", String("{\"error\":\"") + ERR_SD_CARD_NOT_AVAILABLE + "\"}");
    return;
  }

  String sortBy;
  String sortDir;
  loadSDSortPreferences(sortBy, sortDir);
  sdLock.release();

  String json = "{\"by\":\"" + jsonEscape(sortBy) + "\",\"dir\":\"" + jsonEscape(sortDir) + "\"}";
  server.send(HTTP_OK, "application/json", json);
}

static void handleSDSortSet() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!server.hasArg("by") || !server.hasArg("dir")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "by and dir parameters are required");
    return;
  }

  String sortBy = server.arg("by");
  String sortDir = server.arg("dir");
  sortBy.trim();
  sortDir.trim();

  if (!isValidSortByValue(sortBy) || !isValidSortDirValue(sortDir)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid sort values");
    return;
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", ERR_SD_CARD_BUSY);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  if (!saveSDSortPreferences(sortBy, sortDir)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save sort preferences");
    return;
  }

  sdLock.release();
  server.send(HTTP_OK, "text/plain", "Sort preferences saved");
}

static void handleSDMakeDir() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!server.hasArg("dir") || !server.hasArg("name")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "dir and name parameters are required");
    return;
  }

  String dirPath;
  if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid directory path");
    return;
  }

  String folderName = server.arg("name");
  folderName.trim();
  if (!validateNewSDName(folderName)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid folder name");
    return;
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", ERR_SD_CARD_BUSY);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  File parent = SD_MMC.open(dirPath, FILE_READ);
  if (!parent || !parent.isDirectory()) {
    server.send(HTTP_NOT_FOUND, "text/plain", "Parent directory not found");
    return;
  }
  parent.close();

  String targetPath = (dirPath == "/") ? ("/" + folderName) : (dirPath + "/" + folderName);
  String normalizedTarget;
  if (!normalizeAndValidateSDPath(targetPath, normalizedTarget)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid target path");
    return;
  }

  if (isProtectedSDPath(normalizedTarget)) {
    server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  if (SD_MMC.exists(normalizedTarget)) {
    server.send(HTTP_CONFLICT, "text/plain", "A file or folder with this name already exists");
    return;
  }

  if (!SD_MMC.mkdir(normalizedTarget)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to create folder");
    return;
  }

  sdLock.release();
  server.send(HTTP_OK, "text/plain", "Folder created");
}

static void handleSDRemoveDir() {
  if (!checkAuth()) {
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (!server.hasArg("dir")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "dir parameter required");
    return;
  }

  String dirPath;
  if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid directory path");
    return;
  }

  if (dirPath == "/") {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Cannot delete root folder");
    return;
  }

  if (isProtectedSDPath(dirPath)) {
    server.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
    return;
  }

  ScopedSdLock sdLock(pdMS_TO_TICKS(SD_LONG_LOCK_TIMEOUT_MS));
  if (!sdLock.locked()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", ERR_SD_CARD_BUSY);
    return;
  }

  if (!initSDCard()) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    server.send(HTTP_NOT_FOUND, "text/plain", "Folder not found");
    return;
  }
  dir.close();

  bool blockedProtectedPath = false;
  if (!removeSDDirectoryRecursive(dirPath, blockedProtectedPath)) {
    if (blockedProtectedPath) {
      server.send(HTTP_FORBIDDEN, "text/plain", "Folder contains protected content");
    } else {
      server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to delete folder contents");
    }
    return;
  }

  if (!SD_MMC.rmdir(dirPath)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to delete folder");
    return;
  }

  sdLock.release();
  server.send(HTTP_OK, "text/plain", "Folder deleted");
}

static void handleSDUploadData() {
  if (!initSDCard()) {
    sdUploadFailed = true;
    return;
  }

  if (!cfgAccessPass.isEmpty()) {
    if (!server.authenticate("admin", cfgAccessPass.c_str())) {
      sdUploadFailed = true;
      return;
    }
    noteAuthenticatedWebActivity();
  }

  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    sdUploadFailed = false;
    sdUploadBlocked = false;
    sdUploadPath = "";

    String targetDir = "/";
    if (server.hasArg("dir")) {
      if (!normalizeAndValidateSDPath(server.arg("dir"), targetDir)) {
        sdUploadFailed = true;
        return;
      }
    }

    File targetDirFile = SD_MMC.open(targetDir, FILE_READ);
    if (!targetDirFile || !targetDirFile.isDirectory()) {
      sdUploadFailed = true;
      return;
    }
    targetDirFile.close();

    String filename = upload.filename;
    filename.replace('\\', '/');
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) {
      filename = filename.substring(slash + 1);
    }

    if (filename.isEmpty() || filename.indexOf("..") != -1 || filename.indexOf('/') != -1) {
      sdUploadFailed = true;
      return;
    }

    sdUploadPath = (targetDir == "/") ? ("/" + filename) : (targetDir + "/" + filename);
    if (!normalizeAndValidateSDPath(sdUploadPath, sdUploadPath)) {
      sdUploadFailed = true;
      return;
    }

    if (isProtectedSDPath(sdUploadPath)) {
      sdUploadBlocked = true;
      return;
    }

    File existing = SD_MMC.open(sdUploadPath, FILE_READ);
    if (existing) {
      bool isDir = existing.isDirectory();
      existing.close();
      if (isDir) {
        sdUploadFailed = true;
        return;
      }
    }

    SD_MMC.remove(sdUploadPath);
    sdUploadFile = SD_MMC.open(sdUploadPath, FILE_WRITE);
    if (!sdUploadFile) {
      sdUploadFailed = true;
      return;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (sdUploadFailed || !sdUploadFile) {
      sdUploadFailed = true;
      return;
    }
    size_t written = sdUploadFile.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      sdUploadFailed = true;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (sdUploadFailed && !sdUploadPath.isEmpty()) {
      SD_MMC.remove(sdUploadPath);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (!sdUploadPath.isEmpty()) {
      SD_MMC.remove(sdUploadPath);
    }
    sdUploadFailed = true;
  }
}

static void handleSDUploadDataWorker() {
  if (!initSDCard()) {
    sdUploadFailed = true;
    return;
  }

  if (!cfgAccessPass.isEmpty()) {
    bool authorized = hasSharedAccessToken(transferServer)
      || transferServer.authenticate("admin", cfgAccessPass.c_str());
    if (!authorized) {
      sdUploadFailed = true;
      return;
    }
    noteAuthenticatedWebActivity();
  }

  HTTPUpload &upload = transferServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    sdUploadFailed = false;
    sdUploadBlocked = false;
    sdUploadPath = "";

    String targetDir = "/";
    if (transferServer.hasArg("dir")) {
      if (!normalizeAndValidateSDPath(transferServer.arg("dir"), targetDir)) {
        sdUploadFailed = true;
        return;
      }
    }

    File targetDirFile = SD_MMC.open(targetDir, FILE_READ);
    if (!targetDirFile || !targetDirFile.isDirectory()) {
      sdUploadFailed = true;
      return;
    }
    targetDirFile.close();

    String filename = upload.filename;
    filename.replace('\\', '/');
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) {
      filename = filename.substring(slash + 1);
    }

    if (filename.isEmpty() || filename.indexOf("..") != -1 || filename.indexOf('/') != -1) {
      sdUploadFailed = true;
      return;
    }

    sdUploadPath = (targetDir == "/") ? ("/" + filename) : (targetDir + "/" + filename);
    if (!normalizeAndValidateSDPath(sdUploadPath, sdUploadPath)) {
      sdUploadFailed = true;
      return;
    }

    if (isProtectedSDPath(sdUploadPath)) {
      sdUploadBlocked = true;
      return;
    }

    File existing = SD_MMC.open(sdUploadPath, FILE_READ);
    if (existing) {
      bool isDir = existing.isDirectory();
      existing.close();
      if (isDir) {
        sdUploadFailed = true;
        return;
      }
    }

    SD_MMC.remove(sdUploadPath);
    sdUploadFile = SD_MMC.open(sdUploadPath, FILE_WRITE);
    if (!sdUploadFile) {
      sdUploadFailed = true;
      return;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (sdUploadFailed || !sdUploadFile) {
      sdUploadFailed = true;
      return;
    }
    size_t written = sdUploadFile.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      sdUploadFailed = true;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (sdUploadFailed && !sdUploadPath.isEmpty()) {
      SD_MMC.remove(sdUploadPath);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    if (sdUploadFile) {
      sdUploadFile.close();
    }
    if (!sdUploadPath.isEmpty()) {
      SD_MMC.remove(sdUploadPath);
    }
    sdUploadFailed = true;
  }
}

static void handleSDUploadWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  if (sdUploadFile) {
    sdUploadFile.close();
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");

  if (sdUploadBlocked) {
    transferServer.send(HTTP_FORBIDDEN, "text/plain", ERR_ACCESS_DENIED);
  } else if (sdUploadFailed) {
    transferServer.send(HTTP_INTERNAL_ERROR, "text/plain", "Upload failed");
  } else if (sdUploadPath.isEmpty()) {
    transferServer.send(HTTP_BAD_REQUEST, "text/plain", "No file provided");
  } else {
    transferServer.send(HTTP_OK, "text/plain", "Uploaded: " + sdUploadPath);
  }

  sdUploadPath = "";
  sdUploadFailed = false;
  sdUploadBlocked = false;
}

static void handleSDUploadMain() {
  if (!checkAuth(server)) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(HTTP_UNAUTHORIZED, "text/plain", ERR_UNAUTHORIZED);
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  String uploadPath = "/sd/upload";
  if (server.hasArg("dir")) {
    String dirPath;
    if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
      server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid directory path");
      return;
    }
    uploadPath += "?dir=" + urlEncode(dirPath);
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, uploadPath, true));
  server.send(HTTP_TEMPORARY_REDIRECT, "text/plain", "Redirecting to transfer server");
}

static void registerSdRoutes() {
  server.on("/sd", HTTP_GET, handleSDPage);
  server.on("/sd/list", HTTP_GET, handleSDList);
  server.on("/sd/download", HTTP_GET, handleSDDownloadMain);
  server.on("/sd/view", HTTP_GET, handleSDViewMain);
  server.on("/sd/player", HTTP_GET, handleSDPlayerMain);
  server.on("/sd/delete", HTTP_POST, handleSDDelete);
  server.on("/sd/sort", HTTP_GET, handleSDSortGet);
  server.on("/sd/sort", HTTP_POST, handleSDSortSet);
  server.on("/sd/mkdir", HTTP_POST, handleSDMakeDir);
  server.on("/sd/rmdir", HTTP_POST, handleSDRemoveDir);
  server.on("/sd/upload", HTTP_POST, handleSDUploadMain);
}

static void registerSdTransferRoutes() {
  transferServer.on("/sd/download", HTTP_GET, handleSDDownloadWorker);
  transferServer.on("/sd/view", HTTP_GET, handleSDViewWorker);
  transferServer.on("/sd/playback", HTTP_GET, handleSDPlaybackWorker);
  transferServer.on("/sd/upload", HTTP_POST, handleSDUploadWorker, handleSDUploadDataWorker);
}
