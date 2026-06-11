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
  return path == CONFIG_FILE_PATH
      || path == CAPTURE_COUNTER_FILE_PATH
      || path == TIMELAPSE_COUNTER_FILE_PATH
      || path == SD_SORT_FILE_PATH;
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
  sendAppHtmlWithToken(SD_HTML, AppPage::Sd, "SD Browser");
}

static inline bool sdIsAsciiDigit(char ch) {
  return ch >= '0' && ch <= '9';
}

static inline char sdToLowerAscii(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}

static int sdCompareNaturalName(const String &a, const String &b) {
  const size_t aLen = a.length();
  const size_t bLen = b.length();
  size_t ai = 0;
  size_t bi = 0;

  while (ai < aLen && bi < bLen) {
    const char ac = a.charAt(ai);
    const char bc = b.charAt(bi);
    const bool aDigit = sdIsAsciiDigit(ac);
    const bool bDigit = sdIsAsciiDigit(bc);

    if (aDigit && bDigit) {
      size_t aRunEnd = ai;
      size_t bRunEnd = bi;
      while (aRunEnd < aLen && sdIsAsciiDigit(a.charAt(aRunEnd))) {
        ++aRunEnd;
      }
      while (bRunEnd < bLen && sdIsAsciiDigit(b.charAt(bRunEnd))) {
        ++bRunEnd;
      }

      size_t aTrim = ai;
      size_t bTrim = bi;
      while (aTrim < aRunEnd && a.charAt(aTrim) == '0') {
        ++aTrim;
      }
      while (bTrim < bRunEnd && b.charAt(bTrim) == '0') {
        ++bTrim;
      }

      const size_t aDigits = aRunEnd - aTrim;
      const size_t bDigits = bRunEnd - bTrim;
      if (aDigits != bDigits) {
        return aDigits < bDigits ? -1 : 1;
      }

      if (aDigits > 0U) {
        for (size_t i = 0; i < aDigits; ++i) {
          const char da = a.charAt(aTrim + i);
          const char db = b.charAt(bTrim + i);
          if (da != db) {
            return da < db ? -1 : 1;
          }
        }
      }

      const size_t aRunLen = aRunEnd - ai;
      const size_t bRunLen = bRunEnd - bi;
      if (aRunLen != bRunLen) {
        return aRunLen < bRunLen ? -1 : 1;
      }

      ai = aRunEnd;
      bi = bRunEnd;
      continue;
    }

    if (aDigit != bDigit) {
      return aDigit ? -1 : 1;
    }

    const char al = sdToLowerAscii(ac);
    const char bl = sdToLowerAscii(bc);
    if (al != bl) {
      return al < bl ? -1 : 1;
    }
    if (ac != bc) {
      return ac < bc ? -1 : 1;
    }

    ++ai;
    ++bi;
  }

  if (ai < aLen) {
    return 1;
  }
  if (bi < bLen) {
    return -1;
  }
  return 0;
}

static String sdGetLowerExtension(const String &name) {
  int dot = name.lastIndexOf('.');
  if (dot < 0 || dot + 1 >= (int)name.length()) {
    return String();
  }
  String ext = name.substring(dot + 1);
  ext.toLowerCase();
  return ext;
}

struct SDListItem {
  String name;
  String path;
  bool isDir;
  uint32_t size;
};

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

  String sortBy;
  String sortDir;
  loadSDSortPreferences(sortBy, sortDir);
  const bool descending = (sortDir == "desc");

  std::vector<SDListItem> items;
  items.reserve(32);

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

      SDListItem item;
      item.name = itemName;
      item.path = normalizedPath;
      item.isDir = entry.isDirectory();
      item.size = (uint32_t)entry.size();
      items.push_back(item);
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();

  std::sort(items.begin(), items.end(), [&](const SDListItem &a, const SDListItem &b) {
    if (a.isDir != b.isDir) {
      return a.isDir;
    }

    int cmp = 0;
    if (sortBy == "size") {
      if (a.size < b.size) {
        cmp = -1;
      } else if (a.size > b.size) {
        cmp = 1;
      }
    } else if (sortBy == "type") {
      String aExt = sdGetLowerExtension(a.name);
      String bExt = sdGetLowerExtension(b.name);
      cmp = aExt.compareTo(bExt);
      if (cmp == 0) {
        cmp = sdCompareNaturalName(a.name, b.name);
      }
    } else {
      cmp = sdCompareNaturalName(a.name, b.name);
    }

    if (cmp == 0) {
      cmp = sdCompareNaturalName(a.name, b.name);
    }
    return descending ? (cmp > 0) : (cmp < 0);
  });

  String json = "{\"dir\":\"" + jsonEscape(dirPath) + "\",\"items\":[";
  bool first = true;

  for (const SDListItem &item : items) {
    if (!first) {
      json += ",";
    }
    json += "{\"name\":\"" + jsonEscape(item.name) + "\",";
    json += "\"path\":\"" + jsonEscape(item.path) + "\",";
    json += "\"isDir\":" + String(item.isDir ? "true" : "false") + ",";
    json += "\"size\":" + String((unsigned int)item.size) + "}";
    first = false;
  }

  json += "]}";
  sdLock.release();
  server.send(HTTP_OK, "application/json", json);
}

static void sendTransferError(WebServer &srv, int statusCode, const char *message) {
  srv.sendHeader("Access-Control-Allow-Origin", "*");
  srv.send(statusCode, "text/plain", message);
}

static bool sdIsJpegPath(const String &filePath) {
  String lower = filePath;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

static bool sdReadU16BE(File &file, uint16_t &value) {
  int hi = file.read();
  int lo = file.read();
  if (hi < 0 || lo < 0) {
    return false;
  }
  value = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
  return true;
}

static bool sdIsSofMarker(uint8_t marker) {
  switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
      return true;
    default:
      return false;
  }
}

static bool sdReadJpegDimensions(File &file, uint16_t &width, uint16_t &height) {
  width = 0;
  height = 0;

  if (!file.seek(0)) {
    return false;
  }

  int b0 = file.read();
  int b1 = file.read();
  if (b0 != 0xFF || b1 != 0xD8) {
    return false;
  }

  while (file.available()) {
    int prefix = file.read();
    if (prefix < 0) {
      return false;
    }
    if (prefix != 0xFF) {
      continue;
    }

    int marker = file.read();
    while (marker == 0xFF) {
      marker = file.read();
    }
    if (marker < 0) {
      return false;
    }

    if (marker == 0xD9 || marker == 0xDA) {
      break;
    }

    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }

    uint16_t segmentLength = 0;
    if (!sdReadU16BE(file, segmentLength) || segmentLength < 2U) {
      return false;
    }

    if (sdIsSofMarker((uint8_t)marker)) {
      if (segmentLength < 7U) {
        return false;
      }

      if (file.read() < 0) {
        return false;
      }

      uint16_t parsedHeight = 0;
      uint16_t parsedWidth = 0;
      if (!sdReadU16BE(file, parsedHeight) || !sdReadU16BE(file, parsedWidth)) {
        return false;
      }

      width = parsedWidth;
      height = parsedHeight;
      return width > 0U && height > 0U;
    }

    uint32_t skipBytes = (uint32_t)segmentLength - 2U;
    uint32_t nextPos = (uint32_t)file.position() + skipBytes;
    if (!file.seek(nextPos)) {
      return false;
    }
  }

  return false;
}

static bool sdWriteClientAll(WiFiClient &client, const uint8_t *data, size_t length) {
  size_t writtenTotal = 0;
  while (writtenTotal < length) {
    size_t written = client.write(data + writtenTotal, length - writtenTotal);
    if (written == 0) {
      return false;
    }
    writtenTotal += written;
  }
  return true;
}

static bool sdWriteClientU16LE(WiFiClient &client, uint16_t value) {
  uint8_t bytes[2] = {
    (uint8_t)(value & 0xFFU),
    (uint8_t)((value >> 8) & 0xFFU)
  };
  return sdWriteClientAll(client, bytes, sizeof(bytes));
}

static bool sdWriteClientU32LE(WiFiClient &client, uint32_t value) {
  uint8_t bytes[4] = {
    (uint8_t)(value & 0xFFU),
    (uint8_t)((value >> 8) & 0xFFU),
    (uint8_t)((value >> 16) & 0xFFU),
    (uint8_t)((value >> 24) & 0xFFU)
  };
  return sdWriteClientAll(client, bytes, sizeof(bytes));
}

static bool sdWriteClientFourCC(WiFiClient &client, const char *fourcc) {
  return sdWriteClientAll(client, reinterpret_cast<const uint8_t *>(fourcc), 4U);
}

static uint32_t sdGcdU32(uint32_t a, uint32_t b) {
  while (b != 0U) {
    uint32_t temp = a % b;
    a = b;
    b = temp;
  }
  return a;
}

static bool sdWriteMjpgAviHeaderToClient(
    WiFiClient &client,
    uint32_t riffSize,
    uint32_t durationMs,
    uint32_t frameCount,
    uint32_t maxFrameSize,
    uint16_t width,
    uint16_t height,
    uint32_t moviListSize,
    bool hasIndex) {
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
    uint32_t divisor = sdGcdU32(scale, rate);
    scale /= divisor;
    rate /= divisor;
  }

  return
    sdWriteClientFourCC(client, "RIFF") &&
    sdWriteClientU32LE(client, riffSize) &&
    sdWriteClientFourCC(client, "AVI ") &&
    sdWriteClientFourCC(client, "LIST") &&
    sdWriteClientU32LE(client, 192U) &&
    sdWriteClientFourCC(client, "hdrl") &&
    sdWriteClientFourCC(client, "avih") &&
    sdWriteClientU32LE(client, 56U) &&
    sdWriteClientU32LE(client, microsecondsPerFrame) &&
    sdWriteClientU32LE(client, bytesPerSecond) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, hasIndex ? AVI_HAS_INDEX_FLAG : 0U) &&
    sdWriteClientU32LE(client, frameCount) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 1U) &&
    sdWriteClientU32LE(client, maxFrameSize) &&
    sdWriteClientU32LE(client, width) &&
    sdWriteClientU32LE(client, height) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientFourCC(client, "LIST") &&
    sdWriteClientU32LE(client, 116U) &&
    sdWriteClientFourCC(client, "strl") &&
    sdWriteClientFourCC(client, "strh") &&
    sdWriteClientU32LE(client, 56U) &&
    sdWriteClientFourCC(client, "vids") &&
    sdWriteClientFourCC(client, "MJPG") &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU16LE(client, 0U) &&
    sdWriteClientU16LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, scale) &&
    sdWriteClientU32LE(client, rate) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, frameCount) &&
    sdWriteClientU32LE(client, maxFrameSize) &&
    sdWriteClientU32LE(client, 0xFFFFFFFFUL) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU16LE(client, 0U) &&
    sdWriteClientU16LE(client, 0U) &&
    sdWriteClientU16LE(client, width) &&
    sdWriteClientU16LE(client, height) &&
    sdWriteClientFourCC(client, "strf") &&
    sdWriteClientU32LE(client, 40U) &&
    sdWriteClientU32LE(client, 40U) &&
    sdWriteClientU32LE(client, width) &&
    sdWriteClientU32LE(client, height) &&
    sdWriteClientU16LE(client, 1U) &&
    sdWriteClientU16LE(client, 24U) &&
    sdWriteClientFourCC(client, "MJPG") &&
    sdWriteClientU32LE(client, imageSize) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientU32LE(client, 0U) &&
    sdWriteClientFourCC(client, "LIST") &&
    sdWriteClientU32LE(client, moviListSize) &&
    sdWriteClientFourCC(client, "movi");
}

static bool collectMjpgAviFiles(WebServer &srv, std::vector<String> &filePaths, String &errorMessage) {
  if (!srv.hasArg(PARAM_FILE)) {
    errorMessage = ERR_FILE_REQUIRED;
    return false;
  }

  filePaths.clear();
  filePaths.reserve(srv.args());
  for (int index = 0; index < srv.args(); ++index) {
    if (srv.argName(index) != PARAM_FILE) {
      continue;
    }

    String filePath;
    if (!normalizeAndValidateSDPath(srv.arg(index), filePath)) {
      errorMessage = ERR_INVALID_PATH;
      return false;
    }

    if (isProtectedSDPath(filePath)) {
      errorMessage = ERR_ACCESS_DENIED;
      return false;
    }

    if (!sdIsJpegPath(filePath)) {
      errorMessage = "MJPG AVI download supports only .jpg/.jpeg files";
      return false;
    }

    if (std::find(filePaths.begin(), filePaths.end(), filePath) == filePaths.end()) {
      filePaths.push_back(filePath);
    }
  }

  if (filePaths.size() < 2U) {
    errorMessage = "Select at least two JPEG files";
    return false;
  }

  return true;
}

static bool collectMjpgAviFps(WebServer &srv, uint32_t &fps, String &errorMessage) {
  fps = 30U;
  if (!srv.hasArg("fps")) {
    return true;
  }

  String fpsRaw = srv.arg("fps");
  fpsRaw.trim();
  if (fpsRaw.isEmpty()) {
    errorMessage = "fps parameter is invalid";
    return false;
  }

  for (size_t i = 0; i < fpsRaw.length(); ++i) {
    char ch = fpsRaw.charAt(i);
    if (ch < '0' || ch > '9') {
      errorMessage = "fps parameter must be numeric";
      return false;
    }
  }

  uint32_t parsed = (uint32_t)fpsRaw.toInt();
  if (parsed < 1U || parsed > 60U) {
    errorMessage = "fps must be between 1 and 60";
    return false;
  }

  fps = parsed;
  return true;
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

static void handleSDDownloadMjpgAviWorker() {
  if (!checkAuth(transferServer, true)) {
    sendTransferError(transferServer, HTTP_UNAUTHORIZED, ERR_UNAUTHORIZED);
    return;
  }

  if (!initSDCard()) {
    sendTransferError(transferServer, HTTP_INTERNAL_ERROR, ERR_SD_CARD_NOT_AVAILABLE);
    return;
  }

  std::vector<String> filePaths;
  String selectionError;
  if (!collectMjpgAviFiles(transferServer, filePaths, selectionError)) {
    sendTransferError(transferServer, HTTP_BAD_REQUEST, selectionError.c_str());
    return;
  }

  uint32_t fps = 30U;
  if (!collectMjpgAviFps(transferServer, fps, selectionError)) {
    sendTransferError(transferServer, HTTP_BAD_REQUEST, selectionError.c_str());
    return;
  }

  std::vector<uint32_t> frameSizes;
  frameSizes.reserve(filePaths.size());
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t maxFrameSize = 0;
  uint64_t moviListSize64 = AVI_MOVI_LIST_HEADER_SIZE;

  for (const String &filePath : filePaths) {
    File frameFile = SD_MMC.open(filePath, FILE_READ);
    if (!frameFile || frameFile.isDirectory()) {
      sendTransferError(transferServer, HTTP_NOT_FOUND, ERR_FILE_NOT_FOUND);
      return;
    }

    uint16_t frameWidth = 0;
    uint16_t frameHeight = 0;
    bool dimOk = sdReadJpegDimensions(frameFile, frameWidth, frameHeight);
    uint32_t frameSize = (uint32_t)frameFile.size();
    frameFile.close();

    if (!dimOk || frameSize == 0U) {
      sendTransferError(transferServer, HTTP_BAD_REQUEST, "One or more files are not valid JPEG images");
      return;
    }

    if (width == 0U && height == 0U) {
      width = frameWidth;
      height = frameHeight;
    } else if (width != frameWidth || height != frameHeight) {
      sendTransferError(transferServer, HTTP_BAD_REQUEST, "All selected JPEG images must have the same resolution");
      return;
    }

    maxFrameSize = std::max(maxFrameSize, frameSize);
    frameSizes.push_back(frameSize);

    uint64_t chunkSpan = 8ULL + (uint64_t)frameSize + (uint64_t)(frameSize & 1U);
    moviListSize64 += chunkSpan;
    if (moviListSize64 > 0xFFFFFFFFULL) {
      sendTransferError(transferServer, HTTP_BAD_REQUEST, "Selection is too large for AVI output");
      return;
    }
  }

  uint32_t frameCount = (uint32_t)frameSizes.size();
  if (frameCount == 0U) {
    sendTransferError(transferServer, HTTP_BAD_REQUEST, "No JPEG frames selected");
    return;
  }

  uint64_t indexSize64 = (uint64_t)frameCount * 16ULL;
  if (indexSize64 > 0xFFFFFFFFULL) {
    sendTransferError(transferServer, HTTP_BAD_REQUEST, "Too many frames selected");
    return;
  }

  uint64_t riffSize64 = 4ULL + (8ULL + 192ULL) + (8ULL + moviListSize64) + (8ULL + indexSize64);
  if (riffSize64 > 0xFFFFFFFFULL) {
    sendTransferError(transferServer, HTTP_BAD_REQUEST, "Selection is too large for AVI output");
    return;
  }

  uint32_t moviListSize = (uint32_t)moviListSize64;
  uint32_t indexSize = (uint32_t)indexSize64;
  uint32_t riffSize = (uint32_t)riffSize64;
  uint32_t contentLength = riffSize + 8U;
  uint64_t durationNumerator = ((uint64_t)frameCount * 1000ULL) + ((uint64_t)fps / 2ULL);
  uint32_t durationMs = (uint32_t)(durationNumerator / (uint64_t)fps);
  if (durationMs == 0U) {
    durationMs = 1U;
  }

  String firstName = filePaths.front();
  int slash = firstName.lastIndexOf('/');
  if (slash >= 0) {
    firstName = firstName.substring(slash + 1);
  }
  int dot = firstName.lastIndexOf('.');
  if (dot > 0) {
    firstName = firstName.substring(0, dot);
  }
  if (firstName.isEmpty()) {
    firstName = "selection";
  }
  String downloadName = firstName + "_mjpg.avi";

  WiFiClient client = transferServer.client();
  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  char responseHeader[320];
  int hlen = snprintf(
      responseHeader,
      sizeof(responseHeader),
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: video/x-msvideo\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\n"
      "Content-Length: %lu\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n",
      downloadName.c_str(),
      (unsigned long)contentLength);
  if (hlen <= 0 || (size_t)hlen >= sizeof(responseHeader)) {
    client.stop();
    return;
  }
  if (!sdWriteClientAll(client, reinterpret_cast<const uint8_t *>(responseHeader), (size_t)hlen)) {
    client.stop();
    return;
  }

  if (!sdWriteMjpgAviHeaderToClient(client, riffSize, durationMs, frameCount, maxFrameSize, width, height, moviListSize, true)) {
    client.stop();
    return;
  }

  uint8_t frameBuffer[1024];
  const uint8_t zero = 0;
  for (const String &filePath : filePaths) {
    File frameFile = SD_MMC.open(filePath, FILE_READ);
    if (!frameFile || frameFile.isDirectory()) {
      client.stop();
      return;
    }

    uint32_t frameSize = (uint32_t)frameFile.size();
    if (!sdWriteClientFourCC(client, AVI_VIDEO_CHUNK_ID) || !sdWriteClientU32LE(client, frameSize)) {
      frameFile.close();
      client.stop();
      return;
    }

    uint32_t remaining = frameSize;
    while (remaining > 0U) {
      size_t toRead = remaining > sizeof(frameBuffer) ? sizeof(frameBuffer) : (size_t)remaining;
      int readNow = frameFile.read(frameBuffer, toRead);
      if (readNow <= 0 || !sdWriteClientAll(client, frameBuffer, (size_t)readNow)) {
        frameFile.close();
        client.stop();
        return;
      }
      remaining -= (uint32_t)readNow;
    }
    frameFile.close();

    if ((frameSize & 1U) != 0U && !sdWriteClientAll(client, &zero, 1U)) {
      client.stop();
      return;
    }

  }

  if (!sdWriteClientFourCC(client, "idx1") || !sdWriteClientU32LE(client, indexSize)) {
    client.stop();
    return;
  }

  uint32_t indexOffset = AVI_MOVI_LIST_HEADER_SIZE;
  for (size_t i = 0; i < frameSizes.size(); ++i) {
    uint32_t frameSize = frameSizes[i];
    if (!sdWriteClientFourCC(client, AVI_VIDEO_CHUNK_ID) ||
        !sdWriteClientU32LE(client, AVI_KEYFRAME_FLAG) ||
        !sdWriteClientU32LE(client, indexOffset) ||
        !sdWriteClientU32LE(client, frameSize)) {
      client.stop();
      return;
    }
    indexOffset += 8U + frameSize + (frameSize & 1U);
  }

  client.stop();
}

static void handleSDDownloadMjpgAviMain() {
  if (!checkAuth(server)) {
    return;
  }

  std::vector<String> filePaths;
  String selectionError;
  if (!collectMjpgAviFiles(server, filePaths, selectionError)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", selectionError);
    return;
  }

  uint32_t fps = 30U;
  if (!collectMjpgAviFps(server, fps, selectionError)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", selectionError);
    return;
  }

  String query;
  bool first = true;
  for (const String &filePath : filePaths) {
    if (!first) {
      query += "&";
    }
    query += String("file=") + urlEncode(filePath);
    first = false;
  }
  query += String("&fps=") + String((unsigned int)fps);

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/download_mjpg_avi?") + query, true));
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
<title>__PAGE_TITLE__</title>
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
.actions button{background:#2f7fc3;color:#fff;border:0;padding:8px 12px;border-radius:4px;cursor:pointer}
.actions button:hover{background:#25669c}
.hidden{display:none}
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
      <button id="playAgain" class="hidden" type="button">Play again</button>
      <a href="__MEDIA_URL__">Open Raw</a>
      <a href="__DOWNLOAD_URL__">Download</a>
    </div>
    <div class="hint">__PLAYER_HINT__</div>
  </div>
</div>
__APP_FOOTER__
<script>
(function(){
  const video = document.querySelector('video');
  const image = document.querySelector('img');
  const replay = document.getElementById('playAgain');
  if(!replay || (!video && !image)){
    return;
  }

  if(image){
    const baseSrc = image.currentSrc || image.src;
    replay.classList.remove('hidden');
    replay.addEventListener('click', function(){
      const separator = baseSrc.indexOf('?') >= 0 ? '&' : '?';
      image.src = baseSrc + separator + '_r=' + Date.now();
    });
    return;
  }

  function atEnd(){
    if(!Number.isFinite(video.duration) || video.duration <= 0){
      return false;
    }
    return (video.duration - video.currentTime) <= 0.2;
  }

  function syncReplayVisibility(){
    if(video.ended || (video.paused && atEnd())){
      replay.classList.remove('hidden');
    } else {
      replay.classList.add('hidden');
    }
  }

  video.addEventListener('ended', function(){
    syncReplayVisibility();
  });

  video.addEventListener('play', function(){
    syncReplayVisibility();
  });

  video.addEventListener('pause', function(){
    syncReplayVisibility();
  });

  video.addEventListener('timeupdate', function(){
    syncReplayVisibility();
  });

  video.addEventListener('seeked', function(){
    syncReplayVisibility();
  });

  video.addEventListener('loadedmetadata', function(){
    syncReplayVisibility();
  });

  replay.addEventListener('click', function(){
    video.currentTime = 0;
    video.play();
  });

  syncReplayVisibility();
})();
</script>
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
  applyAppChrome(page, AppPage::Sd, "SD Browser");
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
  server.on("/sd/download_mjpg_avi", HTTP_GET, handleSDDownloadMjpgAviMain);
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
  transferServer.on("/sd/download_mjpg_avi", HTTP_GET, handleSDDownloadMjpgAviWorker);
  transferServer.on("/sd/view", HTTP_GET, handleSDViewWorker);
  transferServer.on("/sd/playback", HTTP_GET, handleSDPlaybackWorker);
  transferServer.on("/sd/upload", HTTP_POST, handleSDUploadWorker, handleSDUploadDataWorker);
}
