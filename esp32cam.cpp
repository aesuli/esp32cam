/**
 * ESP32-CAM Multipurpose Firmware
 *
 * Hardware:
 *   - AI-Thinker ESP32-CAM with OV3660 camera sensor
 *   - microSD in 1-bit mode to keep GPIO12/GPIO13 free
 *   - GPIO12 reserved for push button (future local controls)
 *   - GPIO13 reserved for PIR input
 *
 * First Boot (unconfigured or missing config file):
 *   Broadcasts open WiFi AP "ESP32-CAM-Setup".
 *   Visit http://192.168.4.1 to enter WiFi credentials and an
 *   access password. Credentials are encrypted and stored on SD.
 *
 * Normal Operation:
 *   Port 80 — web UI with live MJPEG stream and camera controls.
 *   Port 81 — raw MJPEG stream endpoint (/stream).
 *   Protected by HTTP Basic Auth (username: admin).
 *
 * WiFi Failure Fallback:
 *   If STA connection fails, starts AP "ESP32-CAM" with password
 *   equal to the configured admin password and keeps camera features.
 */

#include <Arduino.h>
#include "esp_camera.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SD_MMC.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include "camera_pins.h"

// ─── Pin definitions ──────────────────────────────────────────────────────────
static constexpr int BUTTON_GPIO = 12;
static constexpr int PIR_GPIO    = 13;

// ─── AP setup credentials ─────────────────────────────────────────────────────
#define AP_SETUP_SSID   "ESP32-CAM-Setup"
#define AP_FALLBACK_SSID "ESP32-CAM"
static constexpr int AP_CHANNEL = 1;
static constexpr bool AP_HIDDEN = false;
static constexpr int AP_MAX_CONNECTIONS = 4;
static constexpr const char *NTP_SERVER = "pool.ntp.org";
static constexpr const char *TIME_ZONE = "BRT3";

// ─── SD configuration storage ──────────────────────────────────────────────────
#define CONFIG_FILE_PATH "/config.enc"
#define CAPTURE_COUNTER_FILE_PATH "/capture_counter.txt"

// ─── Globals ──────────────────────────────────────────────────────────────────
static WebServer   server(80);
static WiFiServer  streamServer(81);

static String cfgAccessPass;
static String cfgDeviceName;
static bool   isConfigured = false;
static bool   streamTaskStarted = false;
static bool   recordingActive = false;
static volatile bool streamClientConnected = false;
static bool   flashEnabled = false;
static unsigned long recordingStartTime = 0;
static uint32_t recordingDurationMs = 0;
static unsigned long recordingLastFrameAt = 0;
static uint32_t recordingFrameCount = 0;
static uint32_t recordingMaxFrameSize = 0;
static uint16_t recordingWidth = 0;
static uint16_t recordingHeight = 0;
static uint32_t recordingMoviListSize = 4;
static File   recordingFile;
static String recordingPath;
static File   sdUploadFile;
static bool   sdUploadFailed = false;
static String sdUploadPath;
static uint32_t captureSequence = 0;
static bool captureSequenceLoaded = false;
static SemaphoreHandle_t cameraMutex = nullptr;
static SemaphoreHandle_t recordingMutex = nullptr;
static constexpr unsigned long STREAM_FRAME_INTERVAL_MS = 100;
static constexpr unsigned long RECORDING_FRAME_INTERVAL_MS = 100;
static constexpr uint32_t AVI_HAS_INDEX_FLAG = 0x00000010UL;
static constexpr uint32_t AVI_KEYFRAME_FLAG = 0x00000010UL;
static constexpr size_t AVI_HEADER_SIZE = 224;

struct AviIndexEntry {
  uint32_t offset;
  uint32_t size;
};

static std::vector<AviIndexEntry> recordingIndex;

struct WifiCredential {
  String ssid;
  String wifiPass;
};

struct CameraSettings {
  int16_t framesize = FRAMESIZE_VGA;
  int16_t quality = 12;
  int16_t brightness = 0;
  int16_t contrast = 0;
  int16_t saturation = 0;
  int16_t specialEffect = 0;
  int16_t wbMode = 0;
  int16_t awb = 1;
  int16_t aec = 1;
  int16_t hmirror = 0;
  int16_t vflip = 0;
  int16_t lenc = 0;
  int16_t streamVisible = 1;
};

struct StoredConfig {
  std::vector<WifiCredential> wifiList;
  String adminPass;
  String deviceName;
  bool hasCameraSettings = false;
  CameraSettings cameraSettings;
};

static StoredConfig runtimeConfig;
static bool syncClockWithNtp();
static camera_fb_t *lockAndCaptureFrame(TickType_t timeoutTicks = pdMS_TO_TICKS(1000));
static void unlockCameraFrame(camera_fb_t *fb);
static bool isRecordingFrameDue(unsigned long now);
static bool recordFrameIfDue(camera_fb_t *fb, unsigned long now);
static bool appendRecordingFrame(camera_fb_t *fb);
static void stopRecordingSession(bool keepFile);

static bool initSDCard() {
  static bool sdInitialized = false;
  if (sdInitialized) {
    return true;
  }

  // 1-bit mode keeps GPIO12 and GPIO13 free for button/PIR.
  // Retry several times: SD cards can be slow to respond on cold boot.
  for (int attempt = 1; attempt <= 5; ++attempt) {
    if (SD_MMC.begin("/sdcard", true)) {
      if (SD_MMC.cardType() != CARD_NONE) {
        sdInitialized = true;
        Serial.printf("[SD] Mounted in 1-bit mode (attempt %d)\n", attempt);
        return true;
      }
      SD_MMC.end();
      Serial.printf("[SD] No card detected on attempt %d\n", attempt);
    } else {
      Serial.printf("[SD] Mount failed on attempt %d\n", attempt);
    }
    delay(500);
  }

  Serial.println("[SD] Failed to mount after 5 attempts");
  return false;
}

static camera_fb_t *lockAndCaptureFrame(TickType_t timeoutTicks) {
  if (!cameraMutex) {
    return nullptr;
  }

  if (xSemaphoreTake(cameraMutex, timeoutTicks) != pdTRUE) {
    return nullptr;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(cameraMutex);
    return nullptr;
  }

  return fb;
}

static void unlockCameraFrame(camera_fb_t *fb) {
  if (!fb) {
    return;
  }

  esp_camera_fb_return(fb);
  if (cameraMutex) {
    xSemaphoreGive(cameraMutex);
  }
}

static bool isRecordingFrameDue(unsigned long now) {
  if (!recordingMutex) {
    return false;
  }

  bool due = false;
  if (xSemaphoreTake(recordingMutex, portMAX_DELAY) == pdTRUE) {
    due = recordingActive
       && (recordingLastFrameAt == 0
        || (now - recordingLastFrameAt) >= RECORDING_FRAME_INTERVAL_MS);
    xSemaphoreGive(recordingMutex);
  }

  return due;
}

static bool saveCaptureSequence(uint32_t value) {
  SD_MMC.remove(CAPTURE_COUNTER_FILE_PATH);
  File file = SD_MMC.open(CAPTURE_COUNTER_FILE_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[SEQ] Failed to open capture counter file for write");
    return false;
  }

  if (file.print(value) == 0) {
    file.close();
    Serial.println("[SEQ] Failed to write capture counter value");
    return false;
  }

  file.close();
  return true;
}

static bool loadCaptureSequence() {
  if (captureSequenceLoaded) {
    return true;
  }

  if (!initSDCard()) {
    return false;
  }

  captureSequence = 0;

  if (SD_MMC.exists(CAPTURE_COUNTER_FILE_PATH)) {
    File file = SD_MMC.open(CAPTURE_COUNTER_FILE_PATH, FILE_READ);
    if (!file) {
      Serial.println("[SEQ] Failed to open capture counter file for read");
      return false;
    }

    String raw = file.readString();
    file.close();
    raw.trim();

    if (!raw.isEmpty()) {
      uint64_t parsed = 0;
      bool valid = true;
      for (size_t i = 0; i < raw.length(); ++i) {
        char c = raw[i];
        if (c < '0' || c > '9') {
          valid = false;
          break;
        }
        parsed = (parsed * 10ULL) + (uint64_t)(c - '0');
        if (parsed > 0xFFFFFFFFULL) {
          valid = false;
          break;
        }
      }

      if (valid) {
        captureSequence = (uint32_t)parsed;
      } else {
        Serial.println("[SEQ] Invalid capture counter content, resetting to 0");
      }
    }
  }

  captureSequenceLoaded = true;
  Serial.printf("[SEQ] Current capture sequence: %lu\n", (unsigned long)captureSequence);
  return true;
}

static bool nextCaptureSequence(uint32_t &nextValue) {
  if (!loadCaptureSequence()) {
    return false;
  }

  uint32_t candidate = captureSequence + 1;
  if (!saveCaptureSequence(candidate)) {
    return false;
  }

  captureSequence = candidate;
  nextValue = candidate;
  return true;
}

static bool isClockSane() {
  time_t now = time(nullptr);
  if (now < 1704067200) {  // 2024-01-01 00:00:00 UTC
    return false;
  }

  struct tm timeinfo;
  if (!localtime_r(&now, &timeinfo)) {
    return false;
  }

  return (timeinfo.tm_year + 1900) >= 2024;
}

static void applyLocalTimeZone() {
  setenv("TZ", TIME_ZONE, 1);
  tzset();
}

static void ensureClockBeforeTimestamp() {
  if (isClockSane()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncClockWithNtp();
  } else {
    Serial.println("[NTP] Clock not synced and WiFi is not connected");
  }
}

static String formatLocalTimeString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  char buf[32] = "1970-01-01 00:00:00";
  if (localtime_r(&now, &timeinfo)) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  }
  return String(buf);
}

static String buildCapturePath(uint32_t sequence, const char *extension) {
  ensureClockBeforeTimestamp();

  time_t now = time(nullptr);
  struct tm timeinfo;
  char stamp[24] = "19700101_000000";
  if (localtime_r(&now, &timeinfo)) {
    if (strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &timeinfo) == 0) {
      strncpy(stamp, "19700101_000000", sizeof(stamp));
      stamp[sizeof(stamp) - 1] = '\0';
    }
  }

  char path[80];
  snprintf(path, sizeof(path), "/capture/%lu-%s.%s",
           (unsigned long)sequence,
           stamp,
           extension);
  return String(path);
}

static void serviceNtpSync() {
  static unsigned long lastAttemptAt = 0;
  unsigned long nowMs = millis();
  if ((nowMs - lastAttemptAt) < 60000UL) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED || isClockSane()) {
    return;
  }

  lastAttemptAt = nowMs;
  syncClockWithNtp();
}

static void deriveKey(uint8_t key[16]) {
  static const uint8_t salt[16] = {
    0x2C, 0x47, 0xB1, 0x93, 0x5E, 0xAA, 0x14, 0x78,
    0xC0, 0x1D, 0x62, 0xEF, 0x39, 0x84, 0x55, 0x0B
  };

  uint64_t chipId = ESP.getEfuseMac();
  for (size_t i = 0; i < 16; ++i) {
    uint8_t macByte = (uint8_t)((chipId >> ((i % 8) * 8)) & 0xFF);
    key[i] = (uint8_t)(macByte ^ salt[i]);
  }
}

static String bytesToHex(const uint8_t *data, size_t len) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

static bool writeFourCC(File &file, const char *fourcc) {
  return file.write((const uint8_t *)fourcc, 4) == 4;
}

static bool writeU16LE(File &file, uint16_t value) {
  uint8_t bytes[2] = {
    (uint8_t)(value & 0xFF),
    (uint8_t)((value >> 8) & 0xFF)
  };
  return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
}

static bool writeU32LE(File &file, uint32_t value) {
  uint8_t bytes[4] = {
    (uint8_t)(value & 0xFF),
    (uint8_t)((value >> 8) & 0xFF),
    (uint8_t)((value >> 16) & 0xFF),
    (uint8_t)((value >> 24) & 0xFF)
  };
  return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
}

static bool writeMjpegFramePayload(File &file, const uint8_t *data, size_t len) {
  if (!data || len == 0U) {
    return false;
  }

  // Some players are more reliable when MJPEG-in-AVI frames advertise AVI1
  // in the APP0 marker instead of the camera's default JFIF signature.
  bool patchApp0 = len >= 10U
    && data[0] == 0xFF && data[1] == 0xD8
    && data[2] == 0xFF && data[3] == 0xE0
    && data[6] == 'J' && data[7] == 'F' && data[8] == 'I' && data[9] == 'F';
  if (!patchApp0) {
    return file.write(data, len) == len;
  }

  static const uint8_t avi1[4] = {'A', 'V', 'I', '1'};
  if (file.write(data, 6) != 6) {
    return false;
  }
  if (file.write(avi1, sizeof(avi1)) != sizeof(avi1)) {
    return false;
  }
  return file.write(data + 10, len - 10U) == (len - 10U);
}

static uint32_t gcdU32(uint32_t a, uint32_t b) {
  while (b != 0U) {
    uint32_t rem = a % b;
    a = b;
    b = rem;
  }
  return a == 0U ? 1U : a;
}

static bool hexToBytes(const String &hex, std::vector<uint8_t> &out) {
  if ((hex.length() % 2) != 0) {
    return false;
  }

  out.clear();
  out.reserve(hex.length() / 2);
  for (unsigned int i = 0; i < hex.length(); i += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      return -1;
    };

    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

static void appendField(std::vector<uint8_t> &buf, const String &value) {
  uint16_t len = (uint16_t)value.length();
  buf.push_back((uint8_t)(len & 0xFF));
  buf.push_back((uint8_t)((len >> 8) & 0xFF));
  for (uint16_t i = 0; i < len; ++i) {
    buf.push_back((uint8_t)value[i]);
  }
}

static bool readField(const std::vector<uint8_t> &buf, size_t &offset, String &out) {
  if (offset + 2 > buf.size()) {
    return false;
  }

  uint16_t len = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
  offset += 2;
  if (offset + len > buf.size()) {
    return false;
  }

  out = "";
  out.reserve(len);
  for (uint16_t i = 0; i < len; ++i) {
    out += (char)buf[offset + i];
  }
  offset += len;
  return true;
}

static void appendU8(std::vector<uint8_t> &buf, uint8_t value) {
  buf.push_back(value);
}

static bool readU8(const std::vector<uint8_t> &buf, size_t &offset, uint8_t &out) {
  if (offset >= buf.size()) {
    return false;
  }

  out = buf[offset++];
  return true;
}

static void appendI16(std::vector<uint8_t> &buf, int16_t value) {
  uint16_t raw = (uint16_t)value;
  buf.push_back((uint8_t)(raw & 0xFF));
  buf.push_back((uint8_t)((raw >> 8) & 0xFF));
}

static bool readI16(const std::vector<uint8_t> &buf, size_t &offset, int16_t &out) {
  if (offset + 2 > buf.size()) {
    return false;
  }

  uint16_t raw = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
  offset += 2;
  out = (int16_t)raw;
  return true;
}

static void captureCameraSettings(sensor_t *sensor, CameraSettings &settings) {
  settings.framesize = sensor->status.framesize;
  settings.quality = sensor->status.quality;
  settings.brightness = sensor->status.brightness;
  settings.contrast = sensor->status.contrast;
  settings.saturation = sensor->status.saturation;
  settings.specialEffect = sensor->status.special_effect;
  settings.wbMode = sensor->status.wb_mode;
  settings.awb = sensor->status.awb;
  settings.aec = sensor->status.aec;
  settings.hmirror = sensor->status.hmirror;
  settings.vflip = sensor->status.vflip;
  settings.lenc = sensor->status.lenc;
}

static bool syncCameraSettingsFromSensor(StoredConfig &cfg) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    return false;
  }

  captureCameraSettings(sensor, cfg.cameraSettings);
  cfg.hasCameraSettings = true;
  return true;
}

static bool readCameraSettings(const std::vector<uint8_t> &buf, size_t &offset, CameraSettings &settings) {
  return readI16(buf, offset, settings.framesize)
      && readI16(buf, offset, settings.quality)
      && readI16(buf, offset, settings.brightness)
      && readI16(buf, offset, settings.contrast)
      && readI16(buf, offset, settings.saturation)
      && readI16(buf, offset, settings.specialEffect)
      && readI16(buf, offset, settings.wbMode)
      && readI16(buf, offset, settings.awb)
      && readI16(buf, offset, settings.aec)
      && readI16(buf, offset, settings.hmirror)
      && readI16(buf, offset, settings.vflip)
  && readI16(buf, offset, settings.lenc)
  && readI16(buf, offset, settings.streamVisible);
}

static void appendCameraSettings(std::vector<uint8_t> &buf, const CameraSettings &settings) {
  appendI16(buf, settings.framesize);
  appendI16(buf, settings.quality);
  appendI16(buf, settings.brightness);
  appendI16(buf, settings.contrast);
  appendI16(buf, settings.saturation);
  appendI16(buf, settings.specialEffect);
  appendI16(buf, settings.wbMode);
  appendI16(buf, settings.awb);
  appendI16(buf, settings.aec);
  appendI16(buf, settings.hmirror);
  appendI16(buf, settings.vflip);
  appendI16(buf, settings.lenc);
  appendI16(buf, settings.streamVisible);
}

static bool updateStoredCameraSetting(StoredConfig &cfg, const String &varName, int val) {
  if (varName == "framesize") cfg.cameraSettings.framesize = val;
  else if (varName == "quality") cfg.cameraSettings.quality = val;
  else if (varName == "brightness") cfg.cameraSettings.brightness = val;
  else if (varName == "contrast") cfg.cameraSettings.contrast = val;
  else if (varName == "saturation") cfg.cameraSettings.saturation = val;
  else if (varName == "special_effect") cfg.cameraSettings.specialEffect = val;
  else if (varName == "wb_mode") cfg.cameraSettings.wbMode = val;
  else if (varName == "awb") cfg.cameraSettings.awb = val;
  else if (varName == "aec") cfg.cameraSettings.aec = val;
  else if (varName == "hmirror") cfg.cameraSettings.hmirror = val;
  else if (varName == "vflip") cfg.cameraSettings.vflip = val;
  else if (varName == "lenc") cfg.cameraSettings.lenc = val;
  else if (varName == "stream_visible") cfg.cameraSettings.streamVisible = val ? 1 : 0;
  else return false;

  cfg.hasCameraSettings = true;
  return true;
}

static void applyStoredCameraSettings(const StoredConfig &cfg) {
  if (!cfg.hasCameraSettings) {
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    Serial.println("[CAM] Cannot apply stored settings: sensor unavailable");
    return;
  }

  struct PendingSetting {
    const char *name;
    int result;
  } pending[] = {
    {"framesize", sensor->set_framesize(sensor, (framesize_t)cfg.cameraSettings.framesize)},
    {"quality", sensor->set_quality(sensor, cfg.cameraSettings.quality)},
    {"brightness", sensor->set_brightness(sensor, cfg.cameraSettings.brightness)},
    {"contrast", sensor->set_contrast(sensor, cfg.cameraSettings.contrast)},
    {"saturation", sensor->set_saturation(sensor, cfg.cameraSettings.saturation)},
    {"special_effect", sensor->set_special_effect(sensor, cfg.cameraSettings.specialEffect)},
    {"awb", sensor->set_whitebal(sensor, cfg.cameraSettings.awb)},
    {"wb_mode", sensor->set_wb_mode(sensor, cfg.cameraSettings.wbMode)},
    {"aec", sensor->set_exposure_ctrl(sensor, cfg.cameraSettings.aec)},
    {"hmirror", sensor->set_hmirror(sensor, cfg.cameraSettings.hmirror)},
    {"vflip", sensor->set_vflip(sensor, cfg.cameraSettings.vflip)},
    {"lenc", sensor->set_lenc(sensor, cfg.cameraSettings.lenc)}
  };

  for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); ++i) {
    if (pending[i].result != 0) {
      Serial.printf("[CAM] Failed to apply stored %s\n", pending[i].name);
    }
  }
}

static bool isPrintableAscii(const String &value) {
  for (unsigned int i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

static String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (unsigned int i = 0; i < value.length(); ++i) {
    char c = value[i];
    switch (c) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

static bool encryptPayload(const std::vector<uint8_t> &plain, String &ivHex, String &cipherHex) {
  if (plain.empty()) {
    return false;
  }

  std::vector<uint8_t> padded = plain;
  size_t padLen = 16 - (padded.size() % 16);
  if (padLen == 0) padLen = 16;
  padded.insert(padded.end(), padLen, (uint8_t)padLen);

  uint8_t key[16];
  uint8_t iv[16];
  deriveKey(key);
  for (size_t i = 0; i < 16; ++i) {
    iv[i] = (uint8_t)(esp_random() & 0xFF);
  }

  std::vector<uint8_t> cipher(padded.size(), 0);
  uint8_t ivWork[16];
  memcpy(ivWork, iv, sizeof(ivWork));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
  if (ret == 0) {
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded.size(), ivWork, padded.data(), cipher.data());
  }
  mbedtls_aes_free(&aes);

  if (ret != 0) {
    return false;
  }

  ivHex = bytesToHex(iv, sizeof(iv));
  cipherHex = bytesToHex(cipher.data(), cipher.size());
  return true;
}

static bool decryptPayload(const String &ivHex, const String &cipherHex, std::vector<uint8_t> &plain) {
  std::vector<uint8_t> iv;
  std::vector<uint8_t> cipher;
  if (!hexToBytes(ivHex, iv) || !hexToBytes(cipherHex, cipher)) {
    return false;
  }
  if (iv.size() != 16 || cipher.empty() || (cipher.size() % 16) != 0) {
    return false;
  }

  uint8_t key[16];
  deriveKey(key);

  plain.assign(cipher.size(), 0);
  uint8_t ivWork[16];
  memcpy(ivWork, iv.data(), sizeof(ivWork));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
  if (ret == 0) {
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher.size(), ivWork, cipher.data(), plain.data());
  }
  mbedtls_aes_free(&aes);
  if (ret != 0 || plain.empty()) {
    return false;
  }

  uint8_t padLen = plain.back();
  if (padLen == 0 || padLen > 16 || padLen > plain.size()) {
    return false;
  }
  for (size_t i = 0; i < padLen; ++i) {
    if (plain[plain.size() - 1 - i] != padLen) {
      return false;
    }
  }

  plain.resize(plain.size() - padLen);
  return true;
}

static bool encryptConfig(const StoredConfig &cfg, String &ivHex, String &cipherHex) {
  std::vector<uint8_t> plain;
  plain.reserve(cfg.adminPass.length() + cfg.deviceName.length() + cfg.wifiList.size() * 32 + 40);

  uint16_t wifiCount = (uint16_t)cfg.wifiList.size();
  plain.push_back((uint8_t)(wifiCount & 0xFF));
  plain.push_back((uint8_t)((wifiCount >> 8) & 0xFF));
  for (size_t i = 0; i < cfg.wifiList.size(); ++i) {
    appendField(plain, cfg.wifiList[i].ssid);
    appendField(plain, cfg.wifiList[i].wifiPass);
  }
  appendField(plain, cfg.adminPass);
  appendField(plain, cfg.deviceName);
  appendU8(plain, cfg.hasCameraSettings ? 1 : 0);
  if (cfg.hasCameraSettings) {
    appendCameraSettings(plain, cfg.cameraSettings);
  }

  return encryptPayload(plain, ivHex, cipherHex);
  }


static bool decryptConfigV1(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
  std::vector<uint8_t> plain;
  if (!decryptPayload(ivHex, cipherHex, plain)) {
    return false;
  }

  size_t offset = 0;
  WifiCredential wifi;
  if (!readField(plain, offset, wifi.ssid)) return false;
  if (!readField(plain, offset, wifi.wifiPass)) return false;
  if (!readField(plain, offset, cfg.adminPass)) return false;

  cfg.wifiList.clear();
  if (!wifi.ssid.isEmpty()) {
    cfg.wifiList.push_back(wifi);
  }
  cfg.deviceName = "ESP32-CAM";
  return offset == plain.size() && !cfg.adminPass.isEmpty();
}

static bool decryptConfigV2(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
  std::vector<uint8_t> plain;
  if (!decryptPayload(ivHex, cipherHex, plain)) {
    return false;
  }

  size_t offset = 0;
  if (plain.size() < 2) {
    return false;
  }

  uint16_t wifiCount = (uint16_t)plain[offset] | ((uint16_t)plain[offset + 1] << 8);
  offset += 2;

  cfg.wifiList.clear();
  cfg.wifiList.reserve(wifiCount);
  for (uint16_t i = 0; i < wifiCount; ++i) {
    WifiCredential wifi;
    if (!readField(plain, offset, wifi.ssid)) return false;
    if (!readField(plain, offset, wifi.wifiPass)) return false;
    cfg.wifiList.push_back(wifi);
  }
  if (!readField(plain, offset, cfg.adminPass)) {
    cfg.adminPass = "";
    cfg.deviceName = "ESP32-CAM";
    return false;
  }
  if (!readField(plain, offset, cfg.deviceName)) {
    cfg.deviceName = "ESP32-CAM";
  }
  return offset == plain.size() && !cfg.adminPass.isEmpty();
}

static bool decryptConfigV3(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
  std::vector<uint8_t> plain;
  if (!decryptPayload(ivHex, cipherHex, plain)) {
    return false;
  }

  size_t offset = 0;
  if (plain.size() < 2) {
    return false;
  }

  uint16_t wifiCount = (uint16_t)plain[offset] | ((uint16_t)plain[offset + 1] << 8);
  offset += 2;

  cfg.wifiList.clear();
  cfg.wifiList.reserve(wifiCount);
  for (uint16_t i = 0; i < wifiCount; ++i) {
    WifiCredential wifi;
    if (!readField(plain, offset, wifi.ssid)) return false;
    if (!readField(plain, offset, wifi.wifiPass)) return false;
    cfg.wifiList.push_back(wifi);
  }
  if (!readField(plain, offset, cfg.adminPass)) {
    cfg.adminPass = "";
    cfg.deviceName = "ESP32-CAM";
    cfg.hasCameraSettings = false;
    return false;
  }
  if (!readField(plain, offset, cfg.deviceName)) {
    cfg.deviceName = "ESP32-CAM";
  }

  uint8_t hasCameraSettings = 0;
  if (!readU8(plain, offset, hasCameraSettings)) {
    cfg.hasCameraSettings = false;
    return false;
  }

  cfg.hasCameraSettings = (hasCameraSettings != 0);
  if (cfg.hasCameraSettings && !readCameraSettings(plain, offset, cfg.cameraSettings)) {
    return false;
  }

  return offset == plain.size() && !cfg.adminPass.isEmpty();
}

static bool decryptConfigV4(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
  std::vector<uint8_t> plain;
  if (!decryptPayload(ivHex, cipherHex, plain)) {
    return false;
  }

  size_t offset = 0;
  if (plain.size() < 2) {
    return false;
  }

  uint16_t wifiCount = (uint16_t)plain[offset] | ((uint16_t)plain[offset + 1] << 8);
  offset += 2;

  cfg.wifiList.clear();
  cfg.wifiList.reserve(wifiCount);
  for (uint16_t i = 0; i < wifiCount; ++i) {
    WifiCredential wifi;
    if (!readField(plain, offset, wifi.ssid)) return false;
    if (!readField(plain, offset, wifi.wifiPass)) return false;
    cfg.wifiList.push_back(wifi);
  }
  if (!readField(plain, offset, cfg.adminPass)) {
    cfg.adminPass = "";
    cfg.deviceName = "ESP32-CAM";
    cfg.hasCameraSettings = false;
    return false;
  }
  if (!readField(plain, offset, cfg.deviceName)) {
    cfg.deviceName = "ESP32-CAM";
  }

  uint8_t hasCameraSettings = 0;
  if (!readU8(plain, offset, hasCameraSettings)) {
    cfg.hasCameraSettings = false;
    return false;
  }

  cfg.hasCameraSettings = (hasCameraSettings != 0);
  if (cfg.hasCameraSettings && !readCameraSettings(plain, offset, cfg.cameraSettings)) {
    return false;
  }

  return offset == plain.size() && !cfg.adminPass.isEmpty();
}

static bool saveConfigToSD(const StoredConfig &cfg) {
  if (!initSDCard()) {
    return false;
  }

  String ivHex;
  String cipherHex;
  if (!encryptConfig(cfg, ivHex, cipherHex)) {
    Serial.println("[CFG] Encryption failed");
    return false;
  }

  SD_MMC.remove(CONFIG_FILE_PATH);
  File file = SD_MMC.open(CONFIG_FILE_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[CFG] Failed to open config file for write");
    return false;
  }

  file.println("ESP32CAMCFG4");
  file.println(ivHex);
  file.println(cipherHex);
  file.close();
  Serial.println("[CFG] Encrypted config saved to SD");
  return true;
}

static bool loadConfigFromSD(StoredConfig &cfg) {
  if (!initSDCard()) {
    return false;
  }

  if (!SD_MMC.exists(CONFIG_FILE_PATH)) {
    Serial.println("[CFG] Config file missing");
    return false;
  }

  File file = SD_MMC.open(CONFIG_FILE_PATH, FILE_READ);
  if (!file) {
    Serial.println("[CFG] Failed to open config file");
    return false;
  }

  String magic = file.readStringUntil('\n');
  String ivHex = file.readStringUntil('\n');
  String cipherHex = file.readStringUntil('\n');
  file.close();

  magic.trim();
  ivHex.trim();
  cipherHex.trim();

  if (magic == "ESP32CAMCFG1") {
    if (!decryptConfigV1(ivHex, cipherHex, cfg)) {
      Serial.println("[CFG] Failed to decrypt legacy config");
      return false;
    }
    return !cfg.adminPass.isEmpty();
  }

  if (magic != "ESP32CAMCFG2") {
    if (magic != "ESP32CAMCFG3" && magic != "ESP32CAMCFG4") {
      Serial.println("[CFG] Invalid config format");
      return false;
    }
  }

  if (magic == "ESP32CAMCFG2") {
    if (!decryptConfigV2(ivHex, cipherHex, cfg)) {
      Serial.println("[CFG] Failed to decrypt config");
      return false;
    }
    return !cfg.adminPass.isEmpty();
  }

  if (magic == "ESP32CAMCFG3") {
    if (!decryptConfigV3(ivHex, cipherHex, cfg)) {
      Serial.println("[CFG] Failed to decrypt config");
      return false;
    }
    return !cfg.adminPass.isEmpty();
  }

  if (!decryptConfigV4(ivHex, cipherHex, cfg)) {
    Serial.println("[CFG] Failed to decrypt config");
    return false;
  }

  return !cfg.adminPass.isEmpty();
}

static bool persistRuntimeConfig(const StoredConfig &cfg) {
  StoredConfig updated = cfg;
  syncCameraSettingsFromSensor(updated);

  if (!saveConfigToSD(updated)) {
    return false;
  }

  runtimeConfig = updated;
  cfgAccessPass = updated.adminPass;
  cfgDeviceName = updated.deviceName;
  isConfigured = !cfgAccessPass.isEmpty();
  return true;
}

// ─── HTML pages (stored in flash) ─────────────────────────────────────────────

// Navigation bar HTML (reused across pages)
static const char NAV_HTML[] PROGMEM = R"html(
<nav style="background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap">
  <a href="/" style="color:#e94560;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573">📷 Camera</a>
  <a href="/sd" style="color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;hover:background:#234573">💾 SD Browser</a>
  <a href="/admin" style="color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573">⚙️ Admin</a>
</nav>
)html";

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
<form method="POST" action="/save">
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
sid('setup_scan_btn').addEventListener('click',setupScanWifi);
sid('setup_scan_list').addEventListener('change',function(){
  if(this.value){sid('setup_ssid').value=this.value;}
});
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

static const char MAIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Camera</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
header span{font-size:.85em;color:#888}
.main{display:flex;flex-wrap:wrap;gap:12px;padding:12px}
.stream-panel{flex:1 1 400px;text-align:center}
.stream-panel img{width:100%;max-width:800px;border:2px solid #0f3460;border-radius:6px;background:#111;min-height:200px}
.stream-panel img.hidden{display:none}
.stream-placeholder{display:none;width:100%;max-width:800px;min-height:200px;margin:0 auto;border:2px dashed #234573;border-radius:6px;background:#111827;color:#7dd3fc;align-items:center;justify-content:center;padding:24px;font-size:.95em}
.stream-placeholder.visible{display:flex}
.btn{display:inline-block;margin-top:8px;padding:8px 20px;background:#e94560;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;font-size:.9em}
.btn:hover{background:#c73652}
.btn.recording{background:#c73652;animation:pulse 1s infinite}
.btn.flash-on{background:#fbbf24}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.7}}
.controls{flex:0 1 280px;background:#16213e;border-radius:8px;padding:14px;height:fit-content}
.controls h3{color:#e94560;margin-bottom:12px;font-size:1em}
.cg{margin-bottom:10px}
.cg label{display:block;font-size:.82em;margin-bottom:3px;color:#bbb}
.cg select,.cg input[type=range]{width:100%}
.cg select{background:#0f3460;color:#eee;border:none;border-radius:4px;padding:4px}
.cg .row{display:flex;justify-content:space-between;align-items:center}
@media (max-width:640px){.main{flex-direction:column}.controls{flex:0 1 auto}}
</style>
</head>
<body>
<nav>
  <a href="/" style="color:#e94560">📷 Camera</a>
  <a href="/sd">💾 SD Browser</a>
  <a href="/admin">⚙️ Admin</a>
</nav>
<header>
  <h1>📷 ESP32-CAM</h1>
  <span id="ip_label"></span>
</header>
<div class="main">
  <div class="stream-panel">
    <img id="stream" alt="Loading stream...">
    <div id="stream_placeholder" class="stream-placeholder">Stream hidden</div>
    <br>
    <button class="btn" id="stream_toggle_btn">🙈 Hide Stream</button>
    <button class="btn" id="cap_btn">📸 Capture</button>
    <button class="btn" id="rec_btn">⏺️ Record</button>
    <button class="btn" id="flash_btn">💡 Flash Off</button>
    <div id="rec_status" style="margin-top:8px;font-size:.85em;color:#7dd3fc"></div>
  </div>
  <div class="controls">
    <h3>Camera Settings</h3>
    <div class="cg">
      <label>Resolution</label>
      <select id="framesize">
        <option value="10">UXGA 1600×1200</option>
        <option value="9">SXGA 1280×1024</option>
        <option value="8">XGA 1024×768</option>
        <option value="7">SVGA 800×600</option>
        <option value="6" selected>VGA 640×480</option>
        <option value="5">CIF 400×296</option>
        <option value="4">QVGA 320×240</option>
        <option value="0">QQVGA 160×120</option>
      </select>
    </div>
    <div class="cg">
      <div class="row"><label>Brightness</label><span id="brightness_v">0</span></div>
      <input type="range" id="brightness" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>Contrast</label><span id="contrast_v">0</span></div>
      <input type="range" id="contrast" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>Saturation</label><span id="saturation_v">0</span></div>
      <input type="range" id="saturation" min="-2" max="2" value="0">
    </div>
    <div class="cg">
      <div class="row"><label>JPEG Quality</label><span id="quality_v">12</span></div>
      <input type="range" id="quality" min="4" max="63" value="12">
    </div>
    <div class="cg">
      <label>Special Effect</label>
      <select id="special_effect">
        <option value="0">None</option>
        <option value="1">Negative</option>
        <option value="2">Grayscale</option>
        <option value="3">Red Tint</option>
        <option value="4">Green Tint</option>
        <option value="5">Blue Tint</option>
        <option value="6">Sepia</option>
      </select>
    </div>
    <div class="cg">
      <label>White Balance Mode</label>
      <select id="wb_mode">
        <option value="0">Auto</option>
        <option value="1">Sunny</option>
        <option value="2">Cloudy</option>
        <option value="3">Office</option>
        <option value="4">Home</option>
      </select>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="awb" checked> Auto White Balance</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="aec" checked> Auto Exposure</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="hmirror"> Horizontal Mirror</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="vflip"> Vertical Flip</label>
    </div>
    <div class="cg">
      <label><input type="checkbox" id="lenc"> Lens Correction</label>
    </div>
  </div>
</div>
<script>
var recordingMode=false;
var streamVisible=true;
function id(n){return document.getElementById(n);}
function chk(el){return el.checked?1:0;}
function ctrl(v,val,persist){
  var url='/control?var='+encodeURIComponent(v)+'&val='+encodeURIComponent(val);
  if(persist===false){url+='&persist=0';}
  else{url+='&persist=1';}
  fetch(url);
}
function setStreamVisibility(isVisible){
  var img=id('stream');
  var placeholder=id('stream_placeholder');
  var toggle=id('stream_toggle_btn');
  streamVisible=!!isVisible;
  img.classList.toggle('hidden',!streamVisible);
  placeholder.classList.toggle('visible',!streamVisible);
  toggle.textContent=streamVisible?'🙈 Hide Stream':'👁️ Show Stream';
  if(streamVisible){
    if(!img.dataset.src){img.dataset.src='http://'+window.location.hostname+':81/stream';}
    if(img.src!==img.dataset.src){img.src=img.dataset.src;}
  }else if(img.src){
    img.dataset.src=img.dataset.src||img.src;
    img.removeAttribute('src');
  }
}
function setRecordingState(isRecording,statusText){
  var btn=id('rec_btn');
  var status=id('rec_status');
  recordingMode=!!isRecording;
  btn.classList.toggle('recording',recordingMode);
  btn.textContent=recordingMode?'⏹️ Stop':'⏺️ Record';
  if(statusText!==undefined){status.textContent=statusText;}
}
function bindSelectControl(name){
  var el=id(name);
  if(!el)return;
  el.addEventListener('change',function(){ctrl(name,el.value);});
}
function bindRangeControl(name){
  var el=id(name);
  var valueEl=id(name+'_v');
  if(!el)return;
  el.addEventListener('input',function(){
    ctrl(name,el.value,false);
    if(valueEl)valueEl.innerText=el.value;
  });
  el.addEventListener('change',function(){ctrl(name,el.value,true);});
}
function bindCheckboxControl(name){
  var el=id(name);
  if(!el)return;
  el.addEventListener('change',function(){ctrl(name,chk(el));});
}
['framesize','special_effect','wb_mode'].forEach(bindSelectControl);
['brightness','contrast','saturation','quality'].forEach(bindRangeControl);
['awb','aec','hmirror','vflip','lenc'].forEach(bindCheckboxControl);
id('stream_toggle_btn').addEventListener('click',function(){
  var nextVisible=!streamVisible;
  setStreamVisibility(nextVisible);
  ctrl('stream_visible',nextVisible?1:0,true);
});
id('cap_btn').addEventListener('click',function(){fetch('/capture').then(function(r){if(r.ok)id('rec_status').textContent='Capture saved to /capture folder';});});
id('rec_btn').addEventListener('click',function(){
  if(recordingMode){
    fetch('/record/stop',{method:'POST'}).then(function(r){
      return r.text().then(function(msg){
        if(r.ok){setRecordingState(false,msg||'Recording saved.');}
      });
    });
  }else{
    fetch('/record/start',{method:'POST'}).then(function(r){
      return r.text().then(function(msg){
        if(r.ok){setRecordingState(true,msg||'Recording...');}
      });
    });
  }
});
id('flash_btn').addEventListener('click',function(){
  var isFlashOn=id('flash_btn').classList.contains('flash-on');
  fetch('/control?var=flash&val='+(isFlashOn?0:1)).then(function(r){
    if(r.ok){
      if(isFlashOn){
        id('flash_btn').classList.remove('flash-on');
        id('flash_btn').textContent='💡 Flash Off';
      }else{
        id('flash_btn').classList.add('flash-on');
        id('flash_btn').textContent='💡 Flash On';
      }
    }
  });
});
var h=window.location.hostname;
id('stream').dataset.src='http://'+h+':81/stream';
id('ip_label').innerText=h;
setStreamVisibility(false);
fetch('/status').then(function(r){return r.json();}).then(function(s){
  ['framesize','brightness','contrast','saturation','quality','special_effect','wb_mode'].forEach(function(k){
    if(s[k]!==undefined){var e=id(k);if(e)e.value=s[k];var v=id(k+'_v');if(v)v.innerText=s[k];}
  });
  ['awb','aec','hmirror','vflip','lenc'].forEach(function(k){if(s[k]!==undefined){var e=id(k);if(e)e.checked=!!s[k];}});
  setStreamVisibility(s.stream_visible!==undefined?!!s.stream_visible:true);
  if(s.recording_active!==undefined){setRecordingState(!!s.recording_active,s.recording_active?'Recording...':'');}
}).catch(function(){
  setStreamVisibility(true);
});
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char ADMIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Admin</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
.panel{flex:1 1 100%;background:#16213e;border-radius:8px;padding:14px;margin:12px;max-width:600px}
.panel h3{color:#e94560;margin-bottom:12px;font-size:1em}
.form{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;align-items:end}
.form label{display:block;font-size:.82em;margin-bottom:3px;color:#bbb}
.form input,.form select{width:100%;padding:10px;box-sizing:border-box;background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px}
.form button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:8px 12px;cursor:pointer}
.form button:hover{background:#c73652}
.status{min-height:20px;font-size:.85em;margin-top:10px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.list{display:flex;flex-direction:column;gap:10px;margin-top:12px}
.item{display:flex;justify-content:space-between;align-items:center;gap:10px;background:#0f3460;border-radius:6px;padding:10px 12px}
.item button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;font-size:.8em}
.item button:hover{background:#c73652}
.empty{font-size:.85em;color:#bbb}
@media (max-width:640px){.panel{margin:12px 0}.item{flex-direction:column;align-items:flex-start}}
</style>
</head>
<body>
<nav>
  <a href="/">📷 Camera</a>
  <a href="/sd">💾 SD Browser</a>
  <a href="/admin" style="color:#e94560">⚙️ Admin</a>
</nav>
<header>
  <h1>⚙️ Administration</h1>
</header>
<div style="padding:12px;display:flex;flex-wrap:wrap">
  <div class="panel">
    <h3>WiFi Priority</h3>
    <form class="form" id="wifi_form">
      <div>
        <label>WiFi SSID</label>
        <input id="wifi_ssid" type="text" maxlength="32" required>
      </div>
      <div>
        <label>WiFi Password</label>
        <input id="wifi_wpass" type="password" maxlength="64" placeholder="Leave blank if open">
      </div>
      <button type="submit">Add / Update</button>
    </form>
    <div style="display:grid;grid-template-columns:1fr auto;gap:8px;margin:10px 0 12px">
      <select id="wifi_scan_list"><option value="">Scan & select</option></select>
      <button onclick="scanWiFi()">Scan</button>
    </div>
    <div id="wifi_status" class="status"></div>
    <div class="list" id="wifi_list"></div>
  </div>
  <div class="panel">
    <h3>Admin Password</h3>
    <form class="form" id="admin_form">
      <div>
        <label>Current Password</label>
        <input id="admin_current" type="password" maxlength="32" required>
      </div>
      <div>
        <label>New Password</label>
        <input id="admin_new" type="password" minlength="8" maxlength="32" required>
      </div>
      <div>
        <label>Confirm New Password</label>
        <input id="admin_confirm" type="password" minlength="8" maxlength="32" required>
      </div>
      <button type="submit">Change Password</button>
    </form>
    <div id="admin_status" class="status"></div>
  </div>
  <div class="panel">
    <h3>Device Name</h3>
    <form class="form" id="name_form">
      <div>
        <label>New Device Name</label>
        <input id="device_name" type="text" maxlength="32" required>
      </div>
      <button type="submit">Change Name</button>
    </form>
    <div id="name_status" class="status"></div>
  </div>
  <div class="panel">
    <h3>Time</h3>
    <div class="status" id="time_now"></div>
    <form class="form" id="time_form">
      <div>
        <label>Manual Local Time</label>
        <input id="manual_time" type="datetime-local" step="1" required>
      </div>
      <button type="submit">Set Time</button>
      <button type="button" id="ntp_sync_btn">Sync NTP</button>
    </form>
    <div id="time_status" class="status"></div>
  </div>
</div>
<script>
function id(n){return document.getElementById(n);}
function setWiFiStatus(msg,err){var e=id('wifi_status');e.textContent=msg;e.className=err?'status error':'status';}
function setAdminStatus(msg,err){var e=id('admin_status');e.textContent=msg;e.className=err?'status error':'status';}
function setNameStatus(msg,err){var e=id('name_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTimeStatus(msg,err){var e=id('time_status');e.textContent=msg;e.className=err?'status error':'status';}
function formData(obj){return Object.keys(obj).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(obj[k]);}).join('&');}
function toDateTimeLocalValue(epoch){
  var d=new Date((Number(epoch)||0)*1000);
  if(isNaN(d.getTime())) return '';
  var pad=function(n){return n<10?'0'+n:String(n);};
  return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'T'+pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
}
function refreshTimeStatus(){
  fetch('/admin/time').then(function(r){
    if(!r.ok){throw new Error('Failed to load time status');}
    return r.json();
  }).then(function(d){
    id('time_now').textContent='Current: '+(d.local||'unknown')+' • '+(d.sane?'Clock synced':'Clock not synced')+' • '+(d.wifiConnected?'WiFi connected':'WiFi offline');
    if(d.epoch){id('manual_time').value=toDateTimeLocalValue(d.epoch);}
  }).catch(function(e){
    id('time_now').textContent='Current: unavailable';
    setTimeStatus(e.message,true);
  });
}
function syncNtpTime(){
  setTimeStatus('Syncing NTP...',false);
  fetch('/admin/time/sync',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setTimeStatus(msg||'NTP sync request finished',!r.ok);
      refreshTimeStatus();
    });
  }).catch(function(e){setTimeStatus(e.message,true);});
}
function refreshDeviceName(){
  var input=id('device_name');
  fetch('/admin/name').then(function(r){
    if(!r.ok){throw new Error('Failed to load device name');}
    return r.json();
  }).then(function(d){
    if(d && typeof d.deviceName==='string' && d.deviceName.length){input.value=d.deviceName;}
  }).catch(function(){});
}
function scanWiFi(){
  setWiFiStatus('Scanning...',false);
  fetch('/wifi/scan').then(function(r){return r.json();}).then(function(data){
    var sel=id('wifi_scan_list');sel.innerHTML='<option value="">Select a network</option>';
    (data.networks||[]).forEach(function(n){var opt=document.createElement('option');opt.value=n.ssid;opt.textContent=n.ssid+' ('+n.rssi+' dBm)';sel.appendChild(opt);});
    setWiFiStatus(data.networks.length+' networks found',false);
  }).catch(function(e){setWiFiStatus(e.message,true);});
}
function renderWiFiList(items){
  var list=id('wifi_list');
  if(!items.length){list.innerHTML='<div class="empty">No networks saved.</div>';return;}
  list.innerHTML=items.map(function(item,i){
    return '<div class="item"><div><strong>'+(i+1)+'. '+item.ssid+'</strong><span style="font-size:.8em;color:#bbb">'+(item.hasPassword?'Protected':'Open')+'</span></div><div style="display:flex;gap:6px"><button onclick="moveWiFi('+i+',\'up\')"'+(i===0?' disabled':'')+'>↑</button><button onclick="moveWiFi('+i+',\'down\')"'+(i===items.length-1?' disabled':'')+'>↓</button><button onclick="deleteWiFi('+i+')">✕</button></div></div>';
  }).join('');
}
function moveWiFi(i,dir){
  fetch('/wifi/move',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i,dir:dir})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Updated':'Failed',!r.ok);});
}
function deleteWiFi(i){
  fetch('/wifi/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({index:i})}).then(function(r){if(r.ok)refreshWiFiList();setWiFiStatus(r.ok?'Deleted':'Failed',!r.ok);});
}
function refreshWiFiList(){fetch('/wifi/list').then(function(r){return r.json();}).then(function(d){renderWiFiList(d.networks||[]);});}
id('wifi_form').addEventListener('submit',function(e){e.preventDefault();fetch('/wifi/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({ssid:id('wifi_ssid').value,wpass:id('wifi_wpass').value})}).then(function(r){r.text().then(function(msg){setWiFiStatus(msg,!r.ok);if(r.ok){id('wifi_form').reset();refreshWiFiList();id('wifi_scan_list').value='';}});});});
id('admin_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({current:id('admin_current').value,next:id('admin_new').value,confirm:id('admin_confirm').value})}).then(function(r){r.text().then(function(msg){setAdminStatus(msg,!r.ok);if(r.ok)id('admin_form').reset();});});});
id('name_form').addEventListener('submit',function(e){e.preventDefault();fetch('/admin/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({name:id('device_name').value})}).then(function(r){r.text().then(function(msg){setNameStatus(msg,!r.ok);if(r.ok)refreshDeviceName();});});});
id('time_form').addEventListener('submit',function(e){
  e.preventDefault();
  var raw=id('manual_time').value;
  if(!raw){setTimeStatus('Choose a date and time first',true);return;}
  var dt=new Date(raw);
  if(isNaN(dt.getTime())){setTimeStatus('Invalid date/time value',true);return;}
  var epoch=Math.floor(dt.getTime()/1000);
  fetch('/admin/time/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({epoch:epoch})}).then(function(r){
    return r.text().then(function(msg){
      setTimeStatus(msg||'Time updated',!r.ok);
      refreshTimeStatus();
    });
  }).catch(function(err){setTimeStatus(err.message,true);});
});
id('ntp_sync_btn').addEventListener('click',syncNtpTime);
id('wifi_scan_list').addEventListener('change',function(){if(this.value)id('wifi_ssid').value=this.value;});
refreshWiFiList();
refreshDeviceName();
refreshTimeStatus();
</script>
</body>
</html>)html";

// ──────────────────────────────────────────────────────────────────────────────

static const char SD_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - SD Browser</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
.container{max-width:1080px;margin:0 auto;padding:12px}
.actions{display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap}
.actions label,.actions button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.actions button:hover{background:#234573}
.actions input[type=file]{display:none}
.pathbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.pathbar button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.85em}
.pathbar button:hover{background:#234573}
.crumbs{font-size:.88em;color:#bbb;word-break:break-all}
.crumbs a{color:#7dd3fc;text-decoration:none}
.crumbs a:hover{text-decoration:underline}
.sortbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.sortbar label{font-size:.85em;color:#bbb}
.sortbar select{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px}
.status{min-height:20px;margin-bottom:12px;color:#7dd3fc;font-size:.9em}
.status.error{color:#ff8a8a}
.file-list{display:flex;flex-direction:column;gap:12px}
.group{background:#16213e;border:1px solid #234573;border-radius:8px;padding:10px}
.group h3{color:#7dd3fc;font-size:.95em;margin-bottom:8px}
.file-item{display:flex;justify-content:space-between;align-items:center;gap:10px;background:#0f3460;padding:10px;border-radius:6px;border:1px solid #234573;margin-bottom:8px}
.file-left{display:flex;align-items:center;gap:10px;min-width:0}
.thumb{width:72px;height:54px;border-radius:4px;object-fit:cover;background:#111;border:1px solid #234573;display:block}
.meta{display:flex;flex-direction:column;min-width:0}
.meta strong{color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:360px}
.meta span{font-size:.82em;color:#bbb}
.file-actions{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.file-actions a,.file-actions button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;text-decoration:none;font-size:.8em}
.file-actions button:hover,.file-actions a:hover{background:#c73652}
.empty{text-align:center;padding:40px;color:#bbb}
@media (max-width:700px){.file-item{flex-direction:column;align-items:flex-start}.meta strong{max-width:100%}.thumb{width:100px;height:75px}.file-actions{justify-content:flex-start}}
</style>
</head>
<body>
<nav>
  <a href="/">📷 Camera</a>
  <a href="/sd" style="color:#e94560">💾 SD Browser</a>
  <a href="/admin">⚙️ Admin</a>
</nav>
<header>
  <h1>💾 SD Card Browser</h1>
</header>
<div class="container">
  <div class="actions">
    <button onclick="loadFiles()">Refresh</button>
    <label>Upload: <input id="upload_file" type="file" onchange="uploadFile(this)"></label>
  </div>
  <div class="pathbar">
    <button onclick="goUp()">Up</button>
    <div id="crumbs" class="crumbs"></div>
  </div>
  <div class="sortbar">
    <label for="sort_by">Sort by</label>
    <select id="sort_by" onchange="applySortAndRender()">
      <option value="name">Name</option>
      <option value="size">Size</option>
      <option value="type">Type</option>
    </select>
    <label for="sort_dir">Direction</label>
    <select id="sort_dir" onchange="applySortAndRender()">
      <option value="asc">Ascending</option>
      <option value="desc">Descending</option>
    </select>
  </div>
  <div id="status" class="status"></div>
  <div id="file_list" class="file-list"></div>
</div>
<script>
var allItems=[];
var currentDir='/';
function esc(s){return String(s).replace(/[&<>\"']/g,function(ch){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;','\'':'&#39;'}[ch];});}
function setStatus(msg,isError){
  var el=document.getElementById('status');
  el.textContent=msg||'';
  el.className=isError?'status error':'status';
}
function normalizePath(p){
  var path=String(p||'/').replace(/\\/g,'/');
  if(!path.startsWith('/')) path='/'+path;
  if(path.length>1 && path.endsWith('/')) path=path.slice(0,-1);
  return path;
}
function baseName(path){
  var clean=normalizePath(path);
  if(clean==='/') return '/';
  var idx=clean.lastIndexOf('/');
  return idx>=0?clean.slice(idx+1):clean;
}
function isImage(path){
  var p=String(path).toLowerCase();
  return p.endsWith('.jpg')||p.endsWith('.jpeg')||p.endsWith('.png')||p.endsWith('.gif')||p.endsWith('.webp')||p.endsWith('.bmp');
}
function formatSize(bytes){
  var n=Number(bytes)||0;
  if(n<1024)return n+' B';
  if(n<1024*1024)return (n/1024).toFixed(1)+' KB';
  return (n/(1024*1024)).toFixed(2)+' MB';
}
function renderCrumbs(){
  var el=document.getElementById('crumbs');
  var clean=normalizePath(currentDir);
  if(clean==='/'){
    el.innerHTML='<strong>/</strong>';
    return;
  }
  var parts=clean.slice(1).split('/');
  var acc='';
  var links=['<a href="#" onclick="openDir(\'/\');return false;">/</a>'];
  for(var i=0;i<parts.length;i++){
    acc+='/'+parts[i];
    if(i===parts.length-1){
      links.push('<strong>'+esc(parts[i])+'</strong>');
    }else{
      links.push('<a href="#" onclick="openDir(\''+acc.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">'+esc(parts[i])+'</a>');
    }
  }
  el.innerHTML=links.join(' / ');
}
function sortItems(items){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value==='desc'?-1:1;
  function splitNumericPrefix(name){
    var text=String(name||'');
    var m=text.match(/^(\d+)(.*)$/);
    if(!m) return {hasPrefix:false,num:0,rest:text};
    return {hasPrefix:true,num:parseInt(m[1],10)||0,rest:m[2]};
  }
  function compareSmartName(aName,bName){
    var aParts=splitNumericPrefix(aName);
    var bParts=splitNumericPrefix(bName);
    if(aParts.hasPrefix&&bParts.hasPrefix){
      if(aParts.num!==bParts.num) return aParts.num-bParts.num;
      return aParts.rest.localeCompare(bParts.rest);
    }
    if(aParts.hasPrefix!==bParts.hasPrefix) return aParts.hasPrefix?-1:1;
    return String(aName||'').localeCompare(String(bName||''));
  }
  return items.slice().sort(function(a,b){
    if(a.isDir!==b.isDir) return a.isDir?-1:1;
    var ba=String(a.name||'');
    var bb=String(b.name||'');
    var ta=ba.indexOf('.')>=0?ba.slice(ba.lastIndexOf('.')+1).toLowerCase():'';
    var tb=bb.indexOf('.')>=0?bb.slice(bb.lastIndexOf('.')+1).toLowerCase():'';

    var cmp=0;
    if(by==='size') cmp=(Number(a.size)||0)-(Number(b.size)||0);
    else if(by==='type') cmp=ta.localeCompare(tb)||compareSmartName(ba,bb);
    else cmp=compareSmartName(ba,bb);

    return cmp*dir;
  });
}
function renderItems(items){
  var list=document.getElementById('file_list');
  if(!items.length){
    list.innerHTML='<div class="empty">This folder is empty</div>';
    setStatus('Folder '+currentDir+' is empty.',false);
    return;
  }

  var totalSize=0;
  items.forEach(function(f){if(!f.isDir)totalSize+=Number(f.size)||0;});

  var rows=items.map(function(item){
    if(item.isDir){
      return '<div class="file-item">'
        +'<div class="file-left"><div class="thumb"></div><div class="meta"><strong>'+esc(item.name)+'</strong><span>Folder • '+esc(item.path)+'</span></div></div>'
        +'<div class="file-actions"><button onclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Open</button></div>'
        +'</div>';
    }

    var preview=isImage(item.path)
      ?'<img class="thumb" loading="lazy" src="/sd/view?file='+encodeURIComponent(item.path)+'" alt="preview">'
      :'<div class="thumb"></div>';
    return '<div class="file-item">'
      +'<div class="file-left">'+preview+'<div class="meta"><strong>'+esc(item.name)+'</strong><span>'+formatSize(item.size)+' • '+esc(item.path)+'</span></div></div>'
      +'<div class="file-actions"><a href="/sd/download?file='+encodeURIComponent(item.path)+'">Download</a><a href="/sd/view?file='+encodeURIComponent(item.path)+'" target="_blank" rel="noopener">Open</a><button onclick="deleteFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
      +'</div>';
  }).join('');

  list.innerHTML='<section class="group"><h3>'+esc(currentDir)+'</h3>'+rows+'</section>';
  var fileCount=items.filter(function(i){return !i.isDir;}).length;
  var dirCount=items.filter(function(i){return i.isDir;}).length;
  setStatus(dirCount+' folder(s), '+fileCount+' file(s), '+formatSize(totalSize),false);
}
function applySortAndRender(){
  renderCrumbs();
  renderItems(sortItems(allItems));
}
function openDir(path){
  currentDir=normalizePath(path);
  loadFiles();
}
function goUp(){
  if(currentDir==='/') return;
  var parent=currentDir.substring(0,currentDir.lastIndexOf('/'));
  if(!parent) parent='/';
  openDir(parent);
}
function loadFiles(){
  setStatus('Loading files...',false);
  fetch('/sd/list?dir='+encodeURIComponent(currentDir)).then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load files');}
      return d;
    });
  }).then(function(d){
    currentDir=normalizePath(d.dir||currentDir);
    allItems=d.items||[];
    applySortAndRender();
  }).catch(function(e){
    document.getElementById('file_list').innerHTML='<div class="empty">Error loading files</div>';
    setStatus(e.message||'Failed to load files',true);
  });
}
function deleteFile(name){
  if(!confirm('Delete '+name+'?'))return;
  setStatus('Deleting '+name+'...',false);
  fetch('/sd/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'file='+encodeURIComponent(name)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Delete failed');}
      setStatus(t||'File deleted',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Delete failed',true);});
}
function uploadFile(input){
  if(!input.files.length)return;
  setStatus('Uploading '+input.files[0].name+'...',false);
  var fd=new FormData();fd.append('file',input.files[0]);
  fetch('/sd/upload',{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Upload failed');}
      setStatus(t||'Upload complete',false);
      loadFiles();
      input.value='';
    });
  }).catch(function(e){setStatus(e.message||'Upload failed',true);});
}
loadFiles();
</script>
</body>
</html>)html";

// ─── Camera initialisation ────────────────────────────────────────────────────
static bool initCamera() {
    // Power-cycle the camera via PWDN pin. On cold boot the sensor may be
    // in an indeterminate state; toggling PWDN ensures a clean startup.
    if (PWDN_GPIO_NUM >= 0) {
        pinMode(PWDN_GPIO_NUM, OUTPUT);
        digitalWrite(PWDN_GPIO_NUM, HIGH);  // power down
        delay(100);
        digitalWrite(PWDN_GPIO_NUM, LOW);   // power up
        delay(100);
    }

    camera_config_t config;
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = Y2_GPIO_NUM;
    config.pin_d1        = Y3_GPIO_NUM;
    config.pin_d2        = Y4_GPIO_NUM;
    config.pin_d3        = Y5_GPIO_NUM;
    config.pin_d4        = Y6_GPIO_NUM;
    config.pin_d5        = Y7_GPIO_NUM;
    config.pin_d6        = Y8_GPIO_NUM;
    config.pin_d7        = Y9_GPIO_NUM;
    config.pin_xclk      = XCLK_GPIO_NUM;
    config.pin_pclk      = PCLK_GPIO_NUM;
    config.pin_vsync     = VSYNC_GPIO_NUM;
    config.pin_href      = HREF_GPIO_NUM;
    config.pin_sccb_sda  = SIOD_GPIO_NUM;
    config.pin_sccb_scl  = SIOC_GPIO_NUM;
    config.pin_pwdn      = PWDN_GPIO_NUM;
    config.pin_reset     = RESET_GPIO_NUM;
    config.xclk_freq_hz  = 20000000;
    config.pixel_format  = PIXFORMAT_JPEG;
    config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location   = CAMERA_FB_IN_DRAM;
    config.frame_size    = FRAMESIZE_VGA;
    config.jpeg_quality  = 12;
    config.fb_count      = 1;

    if (psramFound()) {
        config.fb_location  = CAMERA_FB_IN_PSRAM;
        config.jpeg_quality = 10;
    } else {
        config.frame_size = FRAMESIZE_CIF;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
        return false;
    }

    // OV3660-specific defaults for better image quality
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }

    applyStoredCameraSettings(runtimeConfig);

    // Initialize LED flash pin
    pinMode(LED_FLASH_GPIO_NUM, OUTPUT);
    digitalWrite(LED_FLASH_GPIO_NUM, LOW);
    flashEnabled = false;

    return true;
}

// ─── MJPEG streaming task (core 0, port 81) ───────────────────────────────────
static void streamTask(void *pvParameters) {
    streamServer.begin();
    Serial.println("[STREAM] Ready on port 81");

    for (;;) {
        WiFiClient client = streamServer.accept();
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        Serial.println("[STREAM] Client connected");
        streamClientConnected = true;
        unsigned long lastFrameAt = 0;

        // Consume request headers (wait for blank line)
        {
            unsigned long t = millis();
            while (client.connected() && millis() - t < 3000) {
                if (client.available()) {
                    String line = client.readStringUntil('\n');
                    if (line == "\r") break;
                }
            }
        }

        // Send multipart response headers
        client.print(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=--jpgbound\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "\r\n"
        );

        // Stream JPEG frames until client disconnects
        while (client.connected()) {
            unsigned long now = millis();
            if (lastFrameAt != 0) {
                unsigned long elapsed = now - lastFrameAt;
                if (elapsed < STREAM_FRAME_INTERVAL_MS) {
                    vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_INTERVAL_MS - elapsed));
                    continue;
                }
            }

            camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (fb->format != PIXFORMAT_JPEG) {
                unlockCameraFrame(fb);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            now = millis();
            recordFrameIfDue(fb, now);

            char partHeader[128];
            int hlen = snprintf(partHeader, sizeof(partHeader),
                "--jpgbound\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %u\r\n"
                "\r\n",
                (unsigned int)fb->len);

            bool ok = (client.write((const uint8_t *)partHeader, (size_t)hlen) == (size_t)hlen);
            if (ok) ok = (client.write(fb->buf, fb->len) == fb->len);
            if (ok) ok = (client.print("\r\n") > 0);

            unlockCameraFrame(fb);
            lastFrameAt = now;

            if (!ok) break;
        }

        streamClientConnected = false;
        client.stop();
        Serial.println("[STREAM] Client disconnected");
    }
}

// ─── Authentication helper ────────────────────────────────────────────────────
static bool checkAuth() {
    if (cfgAccessPass.isEmpty()) return true;
    if (!server.authenticate("admin", cfgAccessPass.c_str())) {
        server.requestAuthentication(BASIC_AUTH, "ESP32-CAM");
        return false;
    }
    return true;
}

// ─── Route handlers: AP (setup) mode ─────────────────────────────────────────
static void handleSetupRoot() {
    server.send_P(200, "text/html", SETUP_HTML);
}

static void handleSave() {
    if (!server.hasArg("ssid") || !server.hasArg("apass")) {
        server.send(400, "text/plain", "Missing required fields");
        return;
    }

    String newSSID  = server.arg("ssid");
    String newWPass = server.arg("wpass");
    String newAPass = server.arg("apass");

    if (newSSID.isEmpty()) {
        server.send(400, "text/plain", "SSID is required");
        return;
    }
    if (newAPass.length() < 8) {
      server.send(400, "text/plain", "Access password must be at least 8 characters");
        return;
    }

    if (!isPrintableAscii(newSSID) || !isPrintableAscii(newWPass) || !isPrintableAscii(newAPass)) {
        server.send(400, "text/plain", "Invalid characters in input");
        return;
    }

    StoredConfig cfg;
    WifiCredential wifi;
    wifi.ssid = newSSID;
    wifi.wifiPass = newWPass;
    cfg.wifiList.push_back(wifi);
    cfg.adminPass = newAPass;
    cfg.deviceName = server.hasArg("dname") ? server.arg("dname") : "ESP32-CAM";
    if (cfg.deviceName.isEmpty()) {
      cfg.deviceName = "ESP32-CAM";
    }

    if (!persistRuntimeConfig(cfg)) {
      server.send(500, "text/plain", "Failed to save configuration to SD card");
      return;
    }

    server.send_P(200, "text/html", SAVED_HTML);
    delay(2000);
    ESP.restart();
}

// ─── Route handlers: STA (camera) mode ───────────────────────────────────────
static void handleCameraRoot() {
    if (!checkAuth()) return;
    server.send_P(200, "text/html", MAIN_HTML);
}

static String wifiEncryptionLabel(wifi_auth_mode_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
#ifdef WIFI_AUTH_WPA2_ENTERPRISE
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
#endif
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
#endif
#ifdef WIFI_AUTH_WAPI_PSK
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
#endif
    default: return "Secure";
  }
}

static void sendWifiScanResponse(bool requireAuth) {
  if (requireAuth && !checkAuth()) {
    return;
  }

  wifi_mode_t previousMode = WiFi.getMode();
  bool restoreApOnlyMode = (previousMode == WIFI_AP);
  if (restoreApOnlyMode) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
  }

  int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0) {
    if (restoreApOnlyMode) {
      WiFi.mode(WIFI_AP);
    }
    server.send(500, "application/json", "{\"error\":\"WiFi scan failed\"}");
    return;
  }

  String json = "{\"networks\":[";
  bool first = true;
  for (int i = 0; i < networkCount; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    bool duplicate = false;
    for (int j = 0; j < i; ++j) {
      if (WiFi.SSID(j) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    if (!first) {
      json += ',';
    }
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + ",";
    json += "\"security\":\"" + jsonEscape(wifiEncryptionLabel(WiFi.encryptionType(i))) + "\"}";
    first = false;
  }
  json += "]}";

  WiFi.scanDelete();
  if (restoreApOnlyMode) {
    WiFi.mode(WIFI_AP);
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handleSetupWifiScan() {
  sendWifiScanResponse(false);
}

static void handleWifiScan() {
  sendWifiScanResponse(true);
}

static void handleWifiList() {
  if (!checkAuth()) return;

  String json = "{\"networks\":[";
  for (size_t i = 0; i < runtimeConfig.wifiList.size(); ++i) {
    if (i > 0) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(runtimeConfig.wifiList[i].ssid) + "\",";
    json += "\"hasPassword\":";
    json += runtimeConfig.wifiList[i].wifiPass.isEmpty() ? "false" : "true";
    json += '}';
  }
  json += "]}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handleWifiAdd() {
  if (!checkAuth()) return;
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "SSID is required");
    return;
  }

  String newSSID = server.arg("ssid");
  String newWPass = server.arg("wpass");
  newSSID.trim();

  if (newSSID.isEmpty()) {
    server.send(400, "text/plain", "SSID is required");
    return;
  }
  if (!isPrintableAscii(newSSID) || !isPrintableAscii(newWPass)) {
    server.send(400, "text/plain", "Invalid characters in input");
    return;
  }

  StoredConfig updated = runtimeConfig;
  bool replaced = false;
  for (size_t i = 0; i < updated.wifiList.size(); ++i) {
    if (updated.wifiList[i].ssid == newSSID) {
      updated.wifiList[i].wifiPass = newWPass;
      replaced = true;
      break;
    }
  }

  if (!replaced) {
    WifiCredential wifi;
    wifi.ssid = newSSID;
    wifi.wifiPass = newWPass;
    updated.wifiList.push_back(wifi);
  }

  if (!persistRuntimeConfig(updated)) {
    server.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(200, "text/plain", replaced ? "WiFi credential updated" : "WiFi credential added");
}

static void handleWifiDelete() {
  if (!checkAuth()) return;
  if (!server.hasArg("index")) {
    server.send(400, "text/plain", "Index is required");
    return;
  }

  int index = server.arg("index").toInt();
  if (index < 0 || (size_t)index >= runtimeConfig.wifiList.size()) {
    server.send(400, "text/plain", "Invalid WiFi index");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.wifiList.erase(updated.wifiList.begin() + index);
  if (!persistRuntimeConfig(updated)) {
    server.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(200, "text/plain", "WiFi credential deleted");
}

static void handleWifiMove() {
  if (!checkAuth()) return;
  if (!server.hasArg("index") || !server.hasArg("dir")) {
    server.send(400, "text/plain", "Index and dir are required");
    return;
  }

  int index = server.arg("index").toInt();
  String dir = server.arg("dir");
  int target = dir == "up" ? index - 1 : (dir == "down" ? index + 1 : -1);

  if (index < 0 || target < 0 || (size_t)index >= runtimeConfig.wifiList.size() || (size_t)target >= runtimeConfig.wifiList.size()) {
    server.send(400, "text/plain", "Invalid WiFi move request");
    return;
  }

  StoredConfig updated = runtimeConfig;
  WifiCredential temp = updated.wifiList[index];
  updated.wifiList[index] = updated.wifiList[target];
  updated.wifiList[target] = temp;

  if (!persistRuntimeConfig(updated)) {
    server.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(200, "text/plain", "WiFi priority updated");
}

static void handleAdminPasswordChange() {
  if (!checkAuth()) return;
  if (!server.hasArg("current") || !server.hasArg("next") || !server.hasArg("confirm")) {
    server.send(400, "text/plain", "Current, next, and confirm passwords are required");
    return;
  }

  String currentPass = server.arg("current");
  String nextPass = server.arg("next");
  String confirmPass = server.arg("confirm");

  if (currentPass != cfgAccessPass) {
    server.send(403, "text/plain", "Current password is incorrect");
    return;
  }
  if (nextPass.length() < 8) {
    server.send(400, "text/plain", "New password must be at least 8 characters");
    return;
  }
  if (nextPass != confirmPass) {
    server.send(400, "text/plain", "New password confirmation does not match");
    return;
  }
  if (!isPrintableAscii(nextPass) || !isPrintableAscii(confirmPass) || !isPrintableAscii(currentPass)) {
    server.send(400, "text/plain", "Invalid characters in password");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.adminPass = nextPass;
  if (!persistRuntimeConfig(updated)) {
    server.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  String message = "Admin password updated. Your browser will need the new password for subsequent requests.";
  if (WiFi.getMode() == WIFI_AP) {
    message += " Fallback AP password changes on the next AP restart.";
  }
  server.send(200, "text/plain", message);
}

static void handleCapture() {
    if (!checkAuth()) return;

    camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
    if (!fb) {
        server.send(503, "text/plain", "Camera capture failed");
        return;
    }

    // Send binary JPEG directly via the underlying TCP client
    WiFiClient client = server.client();
    client.printf(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Disposition: inline; filename=\"capture.jpg\"\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        (unsigned int)fb->len
    );
    client.write(fb->buf, fb->len);

    unlockCameraFrame(fb);
}

static void handleControl() {
    if (!checkAuth()) return;

    if (!server.hasArg("var") || !server.hasArg("val")) {
        server.send(400, "text/plain", "Missing var or val parameter");
        return;
    }

    String varName = server.arg("var");
    int    val     = server.arg("val").toInt();
    bool   persist = !server.hasArg("persist") || server.arg("persist") != "0";

    // Handle non-sensor controls separately.
    if (varName == "flash") {
        if (val) {
            digitalWrite(LED_FLASH_GPIO_NUM, HIGH);
            flashEnabled = true;
            Serial.println("[FLASH] Enabled");
        } else {
            digitalWrite(LED_FLASH_GPIO_NUM, LOW);
            flashEnabled = false;
            Serial.println("[FLASH] Disabled");
        }
        server.send(200, "text/plain", "OK");
        return;
    }

      if (varName == "stream_visible") {
        updateStoredCameraSetting(runtimeConfig, varName, val);
        if (persist && !persistRuntimeConfig(runtimeConfig)) {
          server.send(500, "text/plain", "Failed to persist stream visibility");
          return;
        }
        server.send(200, "text/plain", "OK");
        return;
      }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        server.send(503, "text/plain", "Camera sensor not available");
        return;
    }

    int res = 0;
    if      (varName == "framesize")      res = s->set_framesize(s, (framesize_t)val);
    else if (varName == "quality")        res = s->set_quality(s, val);
    else if (varName == "brightness")     res = s->set_brightness(s, val);
    else if (varName == "contrast")       res = s->set_contrast(s, val);
    else if (varName == "saturation")     res = s->set_saturation(s, val);
    else if (varName == "sharpness")      res = s->set_sharpness(s, val);
    else if (varName == "special_effect") res = s->set_special_effect(s, val);
    else if (varName == "awb")            res = s->set_whitebal(s, val);
    else if (varName == "awb_gain")       res = s->set_awb_gain(s, val);
    else if (varName == "wb_mode")        res = s->set_wb_mode(s, val);
    else if (varName == "aec")            res = s->set_exposure_ctrl(s, val);
    else if (varName == "aec2")           res = s->set_aec2(s, val);
    else if (varName == "aec_value")      res = s->set_aec_value(s, val);
    else if (varName == "ae_level")       res = s->set_ae_level(s, val);
    else if (varName == "agc")            res = s->set_gain_ctrl(s, val);
    else if (varName == "agc_gain")       res = s->set_agc_gain(s, val);
    else if (varName == "gainceiling")    res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (varName == "bpc")            res = s->set_bpc(s, val);
    else if (varName == "wpc")            res = s->set_wpc(s, val);
    else if (varName == "raw_gma")        res = s->set_raw_gma(s, val);
    else if (varName == "lenc")           res = s->set_lenc(s, val);
    else if (varName == "hmirror")        res = s->set_hmirror(s, val);
    else if (varName == "vflip")          res = s->set_vflip(s, val);
    else if (varName == "dcw")            res = s->set_dcw(s, val);
    else if (varName == "colorbar")       res = s->set_colorbar(s, val);
    else {
        server.send(400, "text/plain", "Unknown variable");
        return;
    }

    if (res == 0) {
      updateStoredCameraSetting(runtimeConfig, varName, val);
      if (persist && !persistRuntimeConfig(runtimeConfig)) {
        server.send(500, "text/plain", "Failed to persist camera setting");
        return;
      }
    }

    server.send(200, "text/plain", res == 0 ? "OK" : "ERROR");
}

static void handleStatus() {
    if (!checkAuth()) return;

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        server.send(503, "text/plain", "Camera sensor not available");
        return;
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"framesize\":%u,"
        "\"quality\":%u,"
        "\"brightness\":%d,"
        "\"contrast\":%d,"
        "\"saturation\":%d,"
        "\"sharpness\":%d,"
        "\"special_effect\":%u,"
        "\"wb_mode\":%u,"
        "\"awb\":%u,"
        "\"awb_gain\":%u,"
        "\"aec\":%u,"
        "\"aec2\":%u,"
        "\"ae_level\":%d,"
        "\"aec_value\":%u,"
        "\"agc\":%u,"
        "\"agc_gain\":%u,"
        "\"gainceiling\":%u,"
        "\"bpc\":%u,"
        "\"wpc\":%u,"
        "\"raw_gma\":%u,"
        "\"lenc\":%u,"
        "\"hmirror\":%u,"
        "\"vflip\":%u,"
        "\"dcw\":%u,"
        "\"colorbar\":%u,"
        "\"stream_visible\":%u,"
        "\"recording_active\":%u"
        "}",
        s->status.framesize,   s->status.quality,
        s->status.brightness,  s->status.contrast,
        s->status.saturation,  s->status.sharpness,
        s->status.special_effect, s->status.wb_mode,
        s->status.awb,         s->status.awb_gain,
        s->status.aec,         s->status.aec2,
        s->status.ae_level,    s->status.aec_value,
        s->status.agc,         s->status.agc_gain,
        s->status.gainceiling, s->status.bpc,
        s->status.wpc,         s->status.raw_gma,
        s->status.lenc,        s->status.hmirror,
        s->status.vflip,       s->status.dcw,
        s->status.colorbar,
        runtimeConfig.cameraSettings.streamVisible ? 1U : 0U,
        recordingActive ? 1U : 0U
    );

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

static void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

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

static bool appendSDFilesRecursive(const String &dirPath, String &json, bool &first, uint8_t depth) {
  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String entryPathRaw = String(entry.name());
    String entryPath;
    if (!normalizeAndValidateSDPath(entryPathRaw, entryPath)) {
      entry.close();
      entry = dir.openNextFile();
      continue;
    }

    if (entry.isDirectory()) {
      if (depth < 6) {
        appendSDFilesRecursive(entryPath, json, first, depth + 1);
      }
    } else {
      if (!first) {
        json += ",";
      }
      json += "{\"name\":\"" + jsonEscape(entryPath) + "\",\"size\":" + String((unsigned int)entry.size()) + "}";
      first = false;
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
  return true;
}

static void handleDeviceNameRename() {
  if (!checkAuth()) return;
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Device name is required");
    return;
  }

  String newName = server.arg("name");
  if (newName.isEmpty() || newName.length() > 32) {
    server.send(400, "text/plain", "Device name must be between 1 and 32 characters");
    return;
  }

  if (!isPrintableAscii(newName)) {
    server.send(400, "text/plain", "Invalid characters in device name");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.deviceName = newName;
  if (!persistRuntimeConfig(updated)) {
    server.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(200, "text/plain", "Device name updated to: " + newName);
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
  server.send(200, "application/json", json);
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
  server.send(200, "application/json", json);
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
    server.send(400, "text/plain", "epoch is required");
    return;
  }

  time_t epoch = 0;
  if (!parseEpochArg(server.arg("epoch"), epoch)) {
    server.send(400, "text/plain", "Invalid epoch value");
    return;
  }

  applyLocalTimeZone();

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    server.send(500, "text/plain", "Failed to set system time");
    return;
  }

  server.send(200, "text/plain", "Time set to: " + formatLocalTimeString());
}

static void handleAdminTimeSync() {
  if (!checkAuth()) return;

  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain", "WiFi is not connected");
    return;
  }

  bool ok = syncClockWithNtp();
  if (!ok) {
    server.send(500, "text/plain", "NTP sync failed");
    return;
  }

  server.send(200, "text/plain", "NTP synced: " + formatLocalTimeString());
}

static void handleAdminPage() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", ADMIN_HTML);
}

static void handleSDPage() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", SD_HTML);
}

static void handleSDList() {
  if (!checkAuth()) {
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "application/json", "{\"error\":\"SD card not available\"}");
    return;
  }

  String dirPath = "/";
  if (server.hasArg("dir")) {
    if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
      server.send(400, "application/json", "{\"error\":\"Invalid directory path\"}");
      return;
    }
  }

  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    server.send(404, "application/json", "{\"error\":\"Directory not found\"}");
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
  server.send(200, "application/json", json);
}

static void handleSDDownload() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg("file"), filePath)) {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  String downloadName = filePath;
  int slash = downloadName.lastIndexOf('/');
  if (slash >= 0) {
    downloadName = downloadName.substring(slash + 1);
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  server.streamFile(file, "application/octet-stream");
  file.close();
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

static void handleSDView() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg("file"), filePath)) {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  server.streamFile(file, sdMimeTypeForPath(filePath));
  file.close();
}

static void handleSDDelete() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg("file"), filePath)) {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (SD_MMC.remove(filePath)) {
    server.send(200, "text/plain", "File deleted");
  } else {
    server.send(500, "text/plain", "Failed to delete file");
  }
}

static void handleSDUploadData() {
  if (!initSDCard()) {
    sdUploadFailed = true;
    return;
  }

  if (!cfgAccessPass.isEmpty() && !server.authenticate("admin", cfgAccessPass.c_str())) {
    sdUploadFailed = true;
    return;
  }

  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    sdUploadFailed = false;
    sdUploadPath = "";

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

    sdUploadPath = "/" + filename;
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

static void handleSDUpload() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (sdUploadFile) {
    sdUploadFile.close();
  }

  if (sdUploadFailed) {
    server.send(500, "text/plain", "Upload failed");
  } else if (sdUploadPath.isEmpty()) {
    server.send(400, "text/plain", "No file provided");
  } else {
    server.send(200, "text/plain", "Uploaded: " + sdUploadPath);
  }

  sdUploadPath = "";
  sdUploadFailed = false;
}

static void handleCaptureSD() {
  if (!checkAuth()) return;

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  if (!SD_MMC.exists("/capture")) {
    SD_MMC.mkdir("/capture");
  }

  uint32_t sequence = 0;
  if (!nextCaptureSequence(sequence)) {
    server.send(500, "text/plain", "Failed to update capture sequence");
    return;
  }

  String photoPath = buildCapturePath(sequence, "jpg");

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    server.send(503, "text/plain", "Camera capture failed");
    return;
  }

  File file = SD_MMC.open(photoPath, FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    server.send(200, "text/plain", String("Saved: ") + photoPath);
  } else {
    server.send(500, "text/plain", "Failed to save image to SD");
  }

  unlockCameraFrame(fb);
}

static bool writeAviHeader(File &file, uint32_t riffSize, uint32_t durationMs, uint32_t frameCount, uint32_t maxFrameSize, uint16_t width, uint16_t height, uint32_t moviListSize) {
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
    writeU32LE(file, AVI_HAS_INDEX_FLAG) &&
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

  uint32_t indexSize = (uint32_t)recordingIndex.size() * 16UL;
  if (!recordingFile.seek(recordingFile.size())) {
    return false;
  }

  if (!writeFourCC(recordingFile, "idx1") || !writeU32LE(recordingFile, indexSize)) {
    return false;
  }

  for (size_t i = 0; i < recordingIndex.size(); ++i) {
    if (!writeFourCC(recordingFile, "00dc") ||
        !writeU32LE(recordingFile, AVI_KEYFRAME_FLAG) ||
        !writeU32LE(recordingFile, recordingIndex[i].offset) ||
        !writeU32LE(recordingFile, recordingIndex[i].size)) {
      return false;
    }
  }

  recordingFile.flush();

  uint32_t riffSize = (uint32_t)recordingFile.size() - 8UL;
  if (!writeAviHeader(recordingFile, riffSize, durationMs, recordingFrameCount, recordingMaxFrameSize, recordingWidth, recordingHeight, recordingMoviListSize)) {
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

  recordingActive = false;
  recordingStartTime = 0;
  recordingDurationMs = 0;
  recordingLastFrameAt = 0;
  recordingFrameCount = 0;
  recordingMaxFrameSize = 0;
  recordingWidth = 0;
  recordingHeight = 0;
  recordingMoviListSize = 4;
  recordingPath = "";
  recordingIndex.clear();
}

static bool appendRecordingFrame(camera_fb_t *fb) {
  if (!recordingFile || !fb || fb->format != PIXFORMAT_JPEG) {
    return false;
  }

  if (recordingWidth == 0 || recordingHeight == 0) {
    recordingWidth = fb->width;
    recordingHeight = fb->height;
  }
  recordingMaxFrameSize = std::max(recordingMaxFrameSize, (uint32_t)fb->len);

  AviIndexEntry entry;
  // idx1 offsets are relative to the start of the movi list payload, whose
  // first four bytes are the literal "movi" tag.
  entry.offset = recordingMoviListSize;
  entry.size = (uint32_t)fb->len;

  if (!writeFourCC(recordingFile, "00dc") || !writeU32LE(recordingFile, entry.size)) {
    return false;
  }
  if (!writeMjpegFramePayload(recordingFile, fb->buf, fb->len)) {
    return false;
  }

  size_t padding = fb->len & 1U;
  if (padding != 0U) {
    uint8_t zero = 0;
    if (recordingFile.write(&zero, 1) != 1) {
      return false;
    }
  }

  recordingIndex.push_back(entry);
  recordingMoviListSize += 8U + (uint32_t)fb->len + (uint32_t)padding;

  return true;
}

static bool recordFrameIfDue(camera_fb_t *fb, unsigned long now) {
  if (!fb || !recordingMutex) {
    return false;
  }

  bool ok = true;
  if (xSemaphoreTake(recordingMutex, portMAX_DELAY) == pdTRUE) {
    if (recordingActive &&
        (recordingLastFrameAt == 0 || (now - recordingLastFrameAt) >= RECORDING_FRAME_INTERVAL_MS)) {
      ok = appendRecordingFrame(fb);
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
    xSemaphoreGive(recordingMutex);
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

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return;
  }

  recordFrameIfDue(fb, now);
  unlockCameraFrame(fb);
}

static void handleRecordStart() {
  if (!checkAuth()) return;

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  int statusCode = 200;
  String message;

  if (!recordingMutex || xSemaphoreTake(recordingMutex, portMAX_DELAY) != pdTRUE) {
    server.send(500, "text/plain", "Recording lock unavailable");
    return;
  }

  if (recordingActive) {
    statusCode = 400;
    message = "Recording already in progress";
  } else {
    if (!SD_MMC.exists("/capture")) {
      SD_MMC.mkdir("/capture");
    }

    uint32_t sequence = 0;
    if (!nextCaptureSequence(sequence)) {
      statusCode = 500;
      message = "Failed to update capture sequence";
    } else {
      recordingPath = buildCapturePath(sequence, "avi");
      if (recordingFile) {
        recordingFile.close();
      }
      recordingFile = SD_MMC.open(recordingPath, FILE_WRITE);
      if (!recordingFile) {
        recordingPath = "";
        statusCode = 500;
        message = "Failed to open recording file";
      } else {
        uint8_t aviHeader[AVI_HEADER_SIZE] = {0};
        if (recordingFile.write(aviHeader, sizeof(aviHeader)) != sizeof(aviHeader)) {
          recordingFile.close();
          SD_MMC.remove(recordingPath);
          recordingPath = "";
          statusCode = 500;
          message = "Failed to initialize AVI file";
        } else {
          recordingActive = true;
          recordingStartTime = millis();
          recordingDurationMs = 0;
          recordingLastFrameAt = 0;
          recordingFrameCount = 0;
          recordingMaxFrameSize = 0;
          recordingWidth = 0;
          recordingHeight = 0;
          recordingMoviListSize = 4;
          recordingIndex.clear();
          message = String("Recording started: ") + recordingPath;
        }
      }
    }
  }

  xSemaphoreGive(recordingMutex);
  server.send(statusCode, "text/plain", message);
}

static void handleRecordStop() {
  if (!checkAuth()) return;

  int statusCode = 200;
  String message;

  if (!recordingMutex || xSemaphoreTake(recordingMutex, portMAX_DELAY) != pdTRUE) {
    server.send(500, "text/plain", "Recording lock unavailable");
    return;
  }

  if (!recordingActive) {
    statusCode = 400;
    message = "No recording in progress";
  } else {
    unsigned long duration = millis() - recordingStartTime;
    recordingDurationMs = duration == 0UL ? 1U : (uint32_t)duration;

    bool keepFile = recordingFrameCount > 0;
    String savedPath = recordingPath;
    uint32_t savedFrameCount = recordingFrameCount;
    stopRecordingSession(keepFile);
    if (!keepFile) {
      savedPath = "";
    }

    message = "Recording stopped. Duration: " + String(duration / 1000) + "s, Frames: " + String(savedFrameCount);
    if (keepFile) {
      message += ", Saved: " + savedPath;
    } else {
      message += ". No frames captured.";
    }
  }

  xSemaphoreGive(recordingMutex);
  server.send(statusCode, "text/plain", message);
}

// ─── WiFi mode starters ───────────────────────────────────────────────────────
static void registerCameraRoutes() {
  server.on("/",              HTTP_GET,  handleCameraRoot);
  server.on("/admin",         HTTP_GET,  handleAdminPage);
  server.on("/sd",            HTTP_GET,  handleSDPage);
  server.on("/capture",       HTTP_GET,  handleCaptureSD);
  server.on("/control",       HTTP_GET,  handleControl);
  server.on("/status",        HTTP_GET,  handleStatus);
  server.on("/wifi/scan",     HTTP_GET,  handleWifiScan);
  server.on("/admin/password",HTTP_POST, handleAdminPasswordChange);
  server.on("/admin/rename",  HTTP_POST, handleDeviceNameRename);
  server.on("/admin/name",    HTTP_GET,  handleDeviceNameGet);
  server.on("/admin/time",    HTTP_GET,  handleAdminTimeStatus);
  server.on("/admin/time/set",HTTP_POST, handleAdminTimeSet);
  server.on("/admin/time/sync",HTTP_POST, handleAdminTimeSync);
  server.on("/wifi/list",     HTTP_GET,  handleWifiList);
  server.on("/wifi/add",      HTTP_POST, handleWifiAdd);
  server.on("/wifi/delete",   HTTP_POST, handleWifiDelete);
  server.on("/wifi/move",     HTTP_POST, handleWifiMove);
  server.on("/sd/list",       HTTP_GET,  handleSDList);
  server.on("/sd/download",   HTTP_GET,  handleSDDownload);
  server.on("/sd/view",       HTTP_GET,  handleSDView);
  server.on("/sd/delete",     HTTP_POST, handleSDDelete);
  server.on("/sd/upload",     HTTP_POST, handleSDUpload, handleSDUploadData);
  server.on("/record/start",  HTTP_POST, handleRecordStart);
  server.on("/record/stop",   HTTP_POST, handleRecordStop);
  server.onNotFound(handleNotFound);
}

static void ensureStreamTask() {
  if (streamTaskStarted) {
    return;
  }
  xTaskCreatePinnedToCore(streamTask, "streamTask", 8192, NULL, 2, NULL, 0);
  streamTaskStarted = true;
}

static void startSetupAPMode() {
    WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SETUP_SSID, nullptr, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS);
  if (!ok) {
    Serial.println("[WIFI] Setup AP start failed");
    return;
  }
    Serial.printf("[WIFI] Open AP started — SSID: %s  IP: %s\n",
    AP_SETUP_SSID, WiFi.softAPIP().toString().c_str());

    server.on("/",     HTTP_GET,  handleSetupRoot);
    server.on("/wifi/scan", HTTP_GET, handleSetupWifiScan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[HTTP] Setup server ready on port 80");
}

static void startCameraAPMode() {
  if (!cfgDeviceName.isEmpty()) {
    if (!WiFi.softAPsetHostname(cfgDeviceName.c_str())) {
      Serial.println("[WIFI] Failed to set AP hostname");
    } else {
      Serial.printf("[WIFI] AP hostname set to: %s\n", cfgDeviceName.c_str());
    }
  }

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_FALLBACK_SSID, cfgAccessPass.c_str(), AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS);
  if (!ok) {
    Serial.println("[WIFI] Fallback AP start failed (check password length >= 8)");
    return;
  }

  Serial.printf("[WIFI] Fallback AP started — SSID: %s  IP: %s\n",
    AP_FALLBACK_SSID, WiFi.softAPIP().toString().c_str());

  registerCameraRoutes();
  server.begin();
  Serial.println("[HTTP] Camera server ready on port 80 (AP mode)");
  ensureStreamTask();
}

static bool syncClockWithNtp() {
  Serial.printf("[NTP] Syncing clock using %s (TZ=%s)\n", NTP_SERVER, TIME_ZONE);
  applyLocalTimeZone();
  configTzTime(TIME_ZONE, NTP_SERVER);

  struct tm timeinfo;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (getLocalTime(&timeinfo, 500)) {
      char ts[32];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.printf("[NTP] Time synced: %s\n", ts);
      return true;
    }
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  Serial.println("[NTP] Time sync failed; clock may be incorrect");
  return false;
}

static void startSTAMode() {
  if (runtimeConfig.wifiList.empty()) {
    Serial.println("[WIFI] No saved STA networks — switching to fallback AP");
    startCameraAPMode();
    return;
  }

  WiFi.mode(WIFI_STA);

  for (size_t i = 0; i < runtimeConfig.wifiList.size(); ++i) {
    const WifiCredential &wifi = runtimeConfig.wifiList[i];
    WiFi.disconnect(true, true);
    delay(250);
    WiFi.mode(WIFI_STA);

    if (!cfgDeviceName.isEmpty()) {
      if (!WiFi.setHostname(cfgDeviceName.c_str())) {
        Serial.println("[WIFI] Failed to set STA hostname");
      } else {
        Serial.printf("[WIFI] STA hostname set to: %s\n", cfgDeviceName.c_str());
      }
    }

    if (wifi.wifiPass.isEmpty()) {
      WiFi.begin(wifi.ssid.c_str());
    } else {
      WiFi.begin(wifi.ssid.c_str(), wifi.wifiPass.c_str());
    }

    Serial.printf("[WIFI] Trying network %u/%u: %s",
      (unsigned int)(i + 1),
      (unsigned int)runtimeConfig.wifiList.size(),
      wifi.ssid.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
      delay(500);
      Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Connected to %s — IP: %s\n", wifi.ssid.c_str(), WiFi.localIP().toString().c_str());
      syncClockWithNtp();
      registerCameraRoutes();
      server.begin();
      Serial.println("[HTTP] Camera server ready on port 80");
      ensureStreamTask();
      return;
    }

    Serial.printf("[WIFI] Failed to connect to %s\n", wifi.ssid.c_str());
  }

  Serial.println("[WIFI] All saved networks failed — switching to fallback AP");
  startCameraAPMode();
}

// ─── Arduino entry points ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    // Allow power rails to stabilize on cold boot / USB-brick power-up.
    delay(1500);
    Serial.println("\n[BOOT] ESP32-CAM starting");

  cameraMutex = xSemaphoreCreateMutex();
  recordingMutex = xSemaphoreCreateMutex();
  if (!cameraMutex || !recordingMutex) {
    Serial.println("[BOOT] Failed to create runtime mutexes — halting");
    for (;;) {
      delay(1000);
    }
  }

  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  pinMode(PIR_GPIO, INPUT);
  Serial.printf("[GPIO] Button on GPIO%d, PIR on GPIO%d\n", BUTTON_GPIO, PIR_GPIO);

  // Load stored encrypted configuration from SD card.
  StoredConfig cfg;
  if (loadConfigFromSD(cfg)) {
    runtimeConfig = cfg;
    cfgAccessPass = cfg.adminPass;
    cfgDeviceName = cfg.deviceName;
    if (cfgDeviceName.isEmpty()) {
      cfgDeviceName = "ESP32-CAM";
    }
    isConfigured = true;
  } else {
    cfgDeviceName = "ESP32-CAM";
    isConfigured = false;
  }
    Serial.printf("[CFG] Configured: %s\n", isConfigured ? "yes" : "no");

    // Initialise camera
    if (!initCamera()) {
        Serial.println("[CAM] Fatal: camera init failed — halting");
      // Halt here in test build (no LED signaling)
        for (;;) {
        delay(1000);
        }
    }
    Serial.println("[CAM] Camera ready");

    if (isConfigured) {
        startSTAMode();
    } else {
      startSetupAPMode();
    }
}

void loop() {
    server.handleClient();
  serviceNtpSync();
  serviceRecording();
}
