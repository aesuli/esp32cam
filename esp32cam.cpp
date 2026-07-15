/**
 * ESP32-CAM Multipurpose Firmware
 *
 * Hardware:
 *   - AI-Thinker ESP32-CAM with OV3660 camera sensor
 *
 * First Boot (unconfigured or missing config file):
 *   Broadcasts protected WiFi AP "ESP32-CAM-Setup"
 *   with password "ESP32-CAM".
 *   Visit https://192.168.4.1 to enter WiFi credentials and an
 *   access password. Credentials are encrypted and stored in flash.
 *
 * Normal Operation:
 *   Port 80 — HTTP web UI with live MJPEG stream and camera controls.
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
#include <Preferences.h>
#include <Update.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include <mbedtls/aes.h>
#include <time.h>
#include <vector>
#include <cstring>
#include <cstdarg>
#include "camera_pins.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// Pin definitions
static constexpr int LED_GPIO = GPIO_NUM_33;

// AP setup credentials
#define AP_SETUP_SSID "ESP32-CAM-Setup"
#define AP_SETUP_PASS "ESP32-CAM"
#define AP_FALLBACK_SSID "ESP32-CAM"
static constexpr int AP_CHANNEL = 1;
static constexpr bool AP_HIDDEN = false;
static constexpr int AP_MAX_CONNECTIONS = 4;
static constexpr size_t AP_SSID_MAX_LEN = 32;
static constexpr size_t AP_PASS_MIN_LEN = 8;
static constexpr size_t AP_PASS_MAX_LEN = 63;
static constexpr wifi_power_t DEFAULT_TX_POWER_STA = WIFI_POWER_11dBm;
static constexpr wifi_power_t DEFAULT_TX_POWER_AP = WIFI_POWER_8_5dBm;
static constexpr uint16_t AP_FALLBACK_BEACON_INTERVAL_TU = 10000;
static constexpr bool AP_FALLBACK_MODEM_SLEEP_ENABLED = false;
static constexpr bool AP_FALLBACK_EXTENDED_BEACON_ENABLED = false;
static constexpr int CONFIG_LOAD_RETRIES = 5;
static constexpr unsigned long CONFIG_LOAD_RETRY_DELAY_MS = 1000;
static constexpr int CAMERA_INIT_RETRIES = 8;
static constexpr unsigned long CAMERA_INIT_RETRY_DELAY_MS = 500;
static constexpr int AP_START_RETRIES = 3;
static constexpr unsigned long AP_START_RETRY_DELAY_MS = 1000;
static constexpr uint32_t CAMERA_XCLK_FREQS_HZ[] = {20000000UL, 10000000UL, 8000000UL, 4000000UL};

// Config persistence metadata
#define CONFIG_FILE_MAGIC "ESP32CAMCFG14"
static constexpr const char *CONFIG_NVS_NAMESPACE = "esp32camcfg";
static constexpr const char *CONFIG_NVS_MAGIC_KEY = "cfg_magic";
static constexpr const char *CONFIG_NVS_IV_KEY = "cfg_iv";
static constexpr const char *CONFIG_NVS_CIPHER_KEY = "cfg_cipher";

// HTTP status and shared parameters
static constexpr int HTTP_OK = 200;
static constexpr int HTTP_FOUND = 302;
static constexpr int HTTP_TEMPORARY_REDIRECT = 307;
static constexpr int HTTP_BAD_REQUEST = 400;
static constexpr int HTTP_UNAUTHORIZED = 401;
static constexpr int HTTP_FORBIDDEN = 403;
static constexpr int HTTP_NOT_FOUND = 404;
static constexpr int HTTP_INTERNAL_ERROR = 500;
static constexpr int HTTP_SERVICE_UNAVAILABLE = 503;

static constexpr const char *PARAM_SSID = "ssid";
static constexpr const char *PARAM_WPASS = "wpass";
static constexpr const char *PARAM_APASS = "apass";
static constexpr const char *PARAM_INDEX = "index";
static constexpr const char *PARAM_CURRENT = "current";
static constexpr const char *PARAM_NEXT = "next";
static constexpr const char *PARAM_CONFIRM = "confirm";
static constexpr const char *PARAM_NET_MODE = "netmode";
static constexpr const char *PARAM_STATIC_IP = "ip";
static constexpr const char *PARAM_GATEWAY = "gw";
static constexpr const char *PARAM_SUBNET = "mask";
static constexpr const char *PARAM_DNS1 = "dns1";
static constexpr const char *PARAM_DNS2 = "dns2";

static constexpr const char *ERR_UNAUTHORIZED = "Unauthorized";

static constexpr uint16_t HTTP_MAIN_PORT = 80;
static constexpr uint16_t HTTP_STREAM_PORT = 81;
static constexpr uint16_t HTTP_STREAM_PORT_ALT = 83;
static constexpr uint16_t HTTP_TRANSFER_PORT = 82;
static constexpr uint32_t HTTP_STREAM_TASK_STACK = 8192;
static constexpr uint32_t HTTP_TRANSFER_TASK_STACK = 8192;
static constexpr uint8_t STREAM_SLOT_COUNT = 2;
static constexpr unsigned long STREAM_SLOT_RESERVATION_TIMEOUT_MS = 5000;

enum class StreamSlotState : uint8_t {
  Free = 0,
  Reserved = 1,
  Active = 2,
};

static WebServer server(HTTP_MAIN_PORT);
static WebServer streamServer(HTTP_STREAM_PORT);
static WebServer streamServerAlt(HTTP_STREAM_PORT_ALT);
static WebServer transferServer(HTTP_TRANSFER_PORT);

static String cfgAccessPass;
static String cfgDeviceName;
static String routeAccessToken;
static bool   isConfigured = false;
static bool   cameraInitialized = false;
static bool   ledAccessBlinkEnabled = false;
static bool   flashEnabled = false;
static bool   wifiModemSleepEnabled = false;
static bool   staConnectedAtBoot = false;
static volatile bool staLinkUp = false;
static unsigned long lastUrlAccessBlink = 0;
static unsigned long lastCameraActivityAt = 0;
static unsigned long lastStaReconnectAttemptAt = 0;
static constexpr unsigned long LED_ACCESS_BLINK_INTERVAL_MS = 100;  // Minimum interval between access blinks
static constexpr unsigned long STA_RECONNECT_INTERVAL_MS = 30000;
static constexpr unsigned long STA_CONNECT_TIMEOUT_MS = 20000;
static constexpr uint32_t CPU_ACTIVE_FREQ_MHZ = 240;
static constexpr uint32_t CPU_IDLE_FREQ_MHZ = 80;
static unsigned long cameraIdleTimeoutMs = 3000;
static bool   firmwareUploadFailed = false;
static bool   firmwareUploadSuccess = false;
static unsigned long firmwareRestartAt = 0;
static bool   adminRestartPending = false;
static unsigned long adminRestartAt = 0;
static String serialFactoryResetCode;
static String serialCommandBuffer;
static bool serialFactoryResetArmed = false;
static unsigned long serialLastInputAt = 0;
static SemaphoreHandle_t cameraMutex = nullptr;
static SemaphoreHandle_t streamSlotMutex = nullptr;
static TaskHandle_t streamServerTaskHandle = nullptr;
static TaskHandle_t streamServerAltTaskHandle = nullptr;
static TaskHandle_t transferServerTaskHandle = nullptr;
static constexpr unsigned long STREAM_FRAME_INTERVAL_MS = 33;
static constexpr unsigned long FIRMWARE_RESTART_DELAY_MS = 1500;
static constexpr size_t SERIAL_COMMAND_MAX_LEN = 96;
static constexpr unsigned long SERIAL_COMMAND_IDLE_FLUSH_MS = 1200;
static constexpr const char *FIRMWARE_VERSION_TEXT = FIRMWARE_VERSION;
static constexpr const char *FIRMWARE_BUILD_TEXT = __DATE__ " " __TIME__;
static StreamSlotState streamSlotStates[STREAM_SLOT_COUNT] = {
  StreamSlotState::Free,
  StreamSlotState::Free,
};
static unsigned long streamSlotReservedAt[STREAM_SLOT_COUNT] = {0, 0};

class SemaphoreLock {
 public:
  SemaphoreLock(SemaphoreHandle_t semaphore, TickType_t timeoutTicks)
      : semaphore_(semaphore),
        locked_(semaphore && xSemaphoreTake(semaphore, timeoutTicks) == pdTRUE) {}

  ~SemaphoreLock() {
    release();
  }

  SemaphoreLock(const SemaphoreLock &) = delete;
  SemaphoreLock &operator=(const SemaphoreLock &) = delete;

  bool locked() const {
    return locked_;
  }

  void release() {
    if (locked_) {
      xSemaphoreGive(semaphore_);
      locked_ = false;
    }
  }

 private:
  SemaphoreHandle_t semaphore_;
  bool locked_;
};

class AppLogger {
 public:
  void begin(unsigned long baud) {
    Serial.begin(baud);
  }

  int Log(const char *format, ...) {
    va_list args;
    va_start(args, format);

    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    if (needed <= 0) {
      va_end(args);
      return needed;
    }

    std::vector<char> buffer((size_t)needed + 1);
    int written = vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);

    if (written > 0) {
      String line = buildTimestampPrefix();
      line += String(buffer.data());
      if (!line.endsWith("\n")) {
        line += "\n";
      }
      write((const uint8_t *)line.c_str(), line.length());
    }

    return written;
  }

  void LogLine(const char *message) {
    Log("%s", message ? message : "");
  }

  void LogLine(const String &message) {
    Log("%s", message.c_str());
  }

  void LogRaw(char c) {
    write((const uint8_t *)&c, 1);
  }

  void LogRaw(const char *text) {
    if (!text) {
      return;
    }
    write((const uint8_t *)text, strlen(text));
  }

 private:
  size_t write(const uint8_t *buffer, size_t size) {
    if (!buffer || size == 0) {
      return 0;
    }
    Serial.write(buffer, size);
    return size;
  }

  String buildTimestampPrefix() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    char stamp[32];
    if (now >= 1704067200 && localtime_r(&now, &timeinfo)) {
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
      snprintf(stamp, sizeof(stamp), "uptime+%lus", (unsigned long)(millis() / 1000UL));
    }

    String prefix = "[";
    prefix += stamp;
    prefix += "] ";
    return prefix;
  }
};

static AppLogger Logger;

struct OwnedJpegFrame {
  uint8_t *data = nullptr;
  size_t capacity = 0;
  size_t len = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  ~OwnedJpegFrame() {
    release();
  }

  OwnedJpegFrame() = default;
  OwnedJpegFrame(const OwnedJpegFrame &) = delete;
  OwnedJpegFrame &operator=(const OwnedJpegFrame &) = delete;

  void release() {
    if (data) {
      heap_caps_free(data);
      data = nullptr;
    }
    capacity = 0;
    len = 0;
    width = 0;
    height = 0;
  }
};

struct WifiCredential {
  String ssid;
  String wifiPass;
  bool enabled = true;
  bool useStaticIp = false;
  String staticIp;
  String gateway;
  String subnet;
  String dns1;
  String dns2;
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
  int16_t viewRotate90 = 0;
};

struct StoredConfig {
  std::vector<WifiCredential> wifiList;
  String adminPass;
  String deviceName;
  bool hasCameraSettings = false;
  CameraSettings cameraSettings;
  bool ledAccessBlink = false;  // LED blink on URL access
  int8_t txPowerSta = (int8_t)DEFAULT_TX_POWER_STA;  // wifi_power_t cast to int8
  int8_t txPowerAp  = (int8_t)DEFAULT_TX_POWER_AP;
};

struct FrameSizeOption {
  framesize_t value;
  const char *label;
};

static constexpr FrameSizeOption FRAME_SIZE_OPTIONS[] = {
  {FRAMESIZE_QXGA, "QXGA"},
  {FRAMESIZE_P_3MP, "P-3MP"},
  {FRAMESIZE_P_HD, "P-HD"},
  {FRAMESIZE_FHD, "FHD"},
  {FRAMESIZE_UXGA, "UXGA"},
  {FRAMESIZE_SXGA, "SXGA"},
  {FRAMESIZE_HD, "HD"},
  {FRAMESIZE_XGA, "XGA"},
  {FRAMESIZE_SVGA, "SVGA"},
  {FRAMESIZE_VGA, "VGA"},
  {FRAMESIZE_HVGA, "HVGA"},
  {FRAMESIZE_CIF, "CIF"},
  {FRAMESIZE_QVGA, "QVGA"},
  {FRAMESIZE_240X240, "240x240"},
  {FRAMESIZE_HQVGA, "HQVGA"},
  {FRAMESIZE_QCIF, "QCIF"},
  {FRAMESIZE_QQVGA, "QQVGA"},
  {FRAMESIZE_96X96, "96x96"}
};

static constexpr framesize_t DEFAULT_SENSOR_MAX_FRAMESIZE = FRAMESIZE_QXGA;

static StoredConfig runtimeConfig;
static camera_fb_t *lockAndCaptureFrame(TickType_t timeoutTicks = pdMS_TO_TICKS(1000));
static void unlockCameraFrame(camera_fb_t *fb);
static bool copyCameraFrame(camera_fb_t *fb, OwnedJpegFrame &frame);
static bool hasActiveStreamClients();
static int reserveStreamSlot();
static bool activateStreamSlot(uint8_t slotIndex);
static void releaseStreamSlot(uint8_t slotIndex);
static void cleanupExpiredStreamReservations(unsigned long now);
static bool ensureCameraReady(TickType_t timeoutTicks = pdMS_TO_TICKS(5000));
static void serviceCameraIdleTimeout();
static bool loadRuntimeConfigWithRetries(StoredConfig &cfg);
static bool initCameraWithRetries();
static bool applyWifiClientConfig(const WifiCredential &wifi);
static void servicePendingFirmwareRestart();
static void servicePendingAdminRestart();
static void setCpuClockActive(const char *reason = nullptr);
static void setCpuClockIdleIfPossible(const char *reason = nullptr);
static bool requestFactoryResetAndReboot(const char *origin);
static void initializeSerialFactoryResetChallenge();
static void serviceSerialConsole();
static bool isPrintableAscii(const String &value);
static bool isValidApSsid(const String &value);
static bool isValidApPassword(const String &value);
static bool isValidRuntimeTxPowerValue(int value);
static wifi_power_t validatedTxPowerValue(int configuredValue, wifi_power_t fallback, const char *label);
static void setWifiModemSleep(bool enabled, const char *reason = nullptr);
static void setFlashOutput(bool enabled);
static void registerCameraRoutes();
static void startAuxHttpServers();

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Logger.LogLine("[WIFI] STA associated with AP");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staLinkUp = true;
      Logger.Log("[WIFI] STA got IP: %s\n", WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staLinkUp = false;
      Logger.Log("[WIFI] STA disconnected (reason=%u)\n",
        (unsigned int)info.wifi_sta_disconnected.reason);
      break;

    default:
      break;
  }
}

static framesize_t getSensorMaxFrameSize(sensor_t *sensor) {
  if (sensor) {
    camera_sensor_info_t *sensorInfo = esp_camera_sensor_get_info(&sensor->id);
    if (sensorInfo) {
      return sensorInfo->max_size;
    }
  }

  return DEFAULT_SENSOR_MAX_FRAMESIZE;
}

static bool isValidFrameSizeValue(sensor_t *sensor, int value) {
  return value >= 0
      && value < FRAMESIZE_INVALID
      && value <= (int)getSensorMaxFrameSize(sensor);
}

static const char *frameSizeLabel(framesize_t value) {
  for (size_t i = 0; i < sizeof(FRAME_SIZE_OPTIONS) / sizeof(FRAME_SIZE_OPTIONS[0]); ++i) {
    if (FRAME_SIZE_OPTIONS[i].value == value) {
      return FRAME_SIZE_OPTIONS[i].label;
    }
  }

  return "Unknown";
}

static void logSensorFrameSize(sensor_t *sensor, const char *prefix) {
  if (!sensor || sensor->status.framesize < 0 || sensor->status.framesize >= FRAMESIZE_INVALID) {
    Logger.Log("%s: invalid framesize %d\n", prefix, sensor ? sensor->status.framesize : -1);
    return;
  }

  const resolution_info_t &info = resolution[sensor->status.framesize];
  Logger.Log("%s: %s (%ux%u, enum=%d)\n",
    prefix,
    frameSizeLabel(sensor->status.framesize),
    info.width,
    info.height,
    sensor->status.framesize);
}

static String buildFrameSizeOptionsHtml(framesize_t selected) {
  sensor_t *sensor = esp_camera_sensor_get();
  framesize_t maxSize = getSensorMaxFrameSize(sensor);

  if (!isValidFrameSizeValue(sensor, selected)) {
    selected = maxSize >= FRAMESIZE_VGA ? FRAMESIZE_VGA : maxSize;
  }

  String html;
  html.reserve(1200);

  for (size_t i = 0; i < sizeof(FRAME_SIZE_OPTIONS) / sizeof(FRAME_SIZE_OPTIONS[0]); ++i) {
    const FrameSizeOption &option = FRAME_SIZE_OPTIONS[i];
    if (option.value > maxSize) {
      continue;
    }

    const resolution_info_t &info = resolution[option.value];
    html += "<option value=\"";
    html += String((int)option.value);
    html += "\"";
    if (option.value == selected) {
      html += " selected";
    }
    html += ">";
    html += option.label;
    html += " ";
    html += String(info.width);
    html += "&times;";
    html += String(info.height);
    html += "</option>";
  }

  return html;
}

// ─── LED Control Functions ────────────────────────────────────────────────────
static void initLED() {
  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, HIGH);  // HIGH = OFF (active low)
}

static void ledOn() {
  digitalWrite(LED_GPIO, LOW);   // LOW = ON (active low)
}

static void ledOff() {
  digitalWrite(LED_GPIO, HIGH);  // HIGH = OFF (active low)
}

// Fixed on for specified duration
static void ledFixedOn(unsigned long durationMs) {
  ledOn();
  delay(durationMs);
  ledOff();
}

// Single blink with specified on/off times
static void ledBlink(unsigned long onMs, unsigned long offMs) {
  ledOn();
  delay(onMs);
  ledOff();
  delay(offMs);
}

// Multiple blinks
static void ledBlinkCount(int count, unsigned long onMs, unsigned long offMs) {
  for (int i = 0; i < count; ++i) {
    ledBlink(onMs, offMs);
    if (i < count - 1) {
      delay(100);  // Gap between blinks
    }
  }
}

// Very short blink for URL access (does not block long)
static void ledQuickBlink() {
  ledOn();
  delayMicroseconds(50000);  // 50ms very short blink
  ledOff();
}

// Boot sequence: Fixed on
static void ledBootSequence() {
  ledFixedOn(1000);  // 1 second fixed on
}

// WiFi test sequence: Double blink
static void ledWifiTestSequence() {
  ledBlinkCount(2, 100, 100);
}

// WiFi success: Short blink
static void ledWifiSuccessSequence() {
  ledBlink(100, 200);  // 100ms on, 200ms off
}

// WiFi failure: Long blink
static void ledWifiFailureSequence() {
  ledBlink(500, 200);  // 500ms on, 200ms off
}

// Fallback AP activation: Triple blink
static void ledFallbackAPSequence() {
  ledBlinkCount(3, 100, 100);
}

// Setup AP activation: four short blinks
static void ledSetupAPSequence() {
  ledBlinkCount(6, 100, 100);
}

static void powerDownCameraHardware() {
  if (PWDN_GPIO_NUM >= 0) {
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, HIGH);
  }

  setFlashOutput(false);
}

static camera_fb_t *lockAndCaptureFrame(TickType_t timeoutTicks) {
  if (!cameraMutex) {
    return nullptr;
  }

  if (xSemaphoreTake(cameraMutex, timeoutTicks) != pdTRUE) {
    return nullptr;
  }

  if (!cameraInitialized) {
    xSemaphoreGive(cameraMutex);
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

static uint8_t *allocateFrameCopyBuffer(size_t len) {
  if (len == 0U) {
    return nullptr;
  }

  uint8_t *buffer = nullptr;
  if (psramFound()) {
    buffer = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buffer) {
    buffer = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
  }

  return buffer;
}

static bool copyJpegBufferToFrame(const uint8_t *data, size_t len, uint16_t width, uint16_t height, OwnedJpegFrame &frame) {
  frame.len = 0;
  frame.width = 0;
  frame.height = 0;
  if (!data || len == 0U) {
    return false;
  }

  if (!frame.data || frame.capacity < len) {
    uint8_t *copy = allocateFrameCopyBuffer(len);
    if (!copy) {
      return false;
    }
    frame.release();
    frame.data = copy;
    frame.capacity = len;
  }

  memcpy(frame.data, data, len);
  frame.len = len;
  frame.width = width;
  frame.height = height;
  return true;
}

static bool copyCameraFrame(camera_fb_t *fb, OwnedJpegFrame &frame) {
  if (!fb || fb->format != PIXFORMAT_JPEG) {
    frame.len = 0;
    frame.width = 0;
    frame.height = 0;
    return false;
  }
  return copyJpegBufferToFrame(fb->buf, fb->len, fb->width, fb->height, frame);
}

static bool ensureCameraReady(TickType_t timeoutTicks) {
  setCpuClockActive("camera activity");

  if (!cameraMutex) {
    return false;
  }

  SemaphoreLock cameraLock(cameraMutex, timeoutTicks);
  if (!cameraLock.locked()) {
    return false;
  }

  bool ok = true;
  if (!cameraInitialized) {
    Logger.LogLine("[CAM] Powering up camera on demand");
    ok = initCameraWithRetries();
    if (ok) {
      cameraInitialized = true;
      Logger.LogLine("[CAM] Camera ready");
    } else {
      Logger.LogLine("[CAM] Camera init failed");
    }
  }

  if (ok) {
    lastCameraActivityAt = millis();
  }

  return ok;
}

static void serviceCameraIdleTimeout() {
  if (!cameraInitialized || hasActiveStreamClients() || cameraIdleTimeoutMs == 0UL) {
    return;
  }

  unsigned long now = millis();
  if (lastCameraActivityAt != 0 && (now - lastCameraActivityAt) < cameraIdleTimeoutMs) {
    return;
  }

  SemaphoreLock cameraLock(cameraMutex, 0);
  if (!cameraLock.locked()) {
    return;
  }

  if (cameraInitialized && !hasActiveStreamClients()) {
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
      Logger.Log("[CAM] Deinit failed: 0x%x\n", err);
    } else {
      cameraInitialized = false;
      powerDownCameraHardware();
      Logger.Log("[CAM] Camera powered down after %lu ms idle\n", cameraIdleTimeoutMs);
      setCpuClockIdleIfPossible("camera idle");
    }
  }
}

static bool isHostnameLabelChar(char c) {
  return (c >= 'a' && c <= 'z')
      || (c >= '0' && c <= '9')
      || c == '-';
}

static String buildNetworkHostname(const String &deviceName) {
  String trimmed = deviceName;
  trimmed.trim();

  String hostname;
  hostname.reserve(trimmed.length());

  bool lastWasHyphen = false;
  for (unsigned int i = 0; i < trimmed.length(); ++i) {
    char c = trimmed[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }

    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      hostname += c;
      lastWasHyphen = false;
      continue;
    }

    if (!lastWasHyphen && hostname.length() > 0) {
      hostname += '-';
      lastWasHyphen = true;
    }
  }

  while (hostname.length() > 0 && hostname[hostname.length() - 1] == '-') {
    hostname.remove(hostname.length() - 1);
  }

  if (hostname.isEmpty()) {
    hostname = "esp32-cam";
  }

  if (!hostname.isEmpty() && !isHostnameLabelChar(hostname[0])) {
    hostname = "esp32-cam";
  }

  if (hostname.length() > 63) {
    hostname.remove(63);
    while (hostname.length() > 0 && hostname[hostname.length() - 1] == '-') {
      hostname.remove(hostname.length() - 1);
    }
  }

  if (hostname.isEmpty()) {
    hostname = "esp32-cam";
  }

  return hostname;
}

static String buildFallbackApSsid() {
  String ssid = cfgDeviceName;
  ssid.trim();

  if (!isPrintableAscii(ssid)) {
    Logger.LogLine("[WIFI] Device name has non-ASCII characters; using default fallback AP SSID");
    return AP_FALLBACK_SSID;
  }

  if (ssid.length() > AP_SSID_MAX_LEN) {
    ssid.remove(AP_SSID_MAX_LEN);
    ssid.trim();
    Logger.Log("[WIFI] Truncated fallback AP SSID to %u characters\n", (unsigned int)AP_SSID_MAX_LEN);
  }

  if (!isValidApSsid(ssid)) {
    ssid = AP_FALLBACK_SSID;
  }
  return ssid;
}

static String buildFallbackApPassword() {
  String password = cfgAccessPass;

  if (!isPrintableAscii(password)) {
    Logger.LogLine("[WIFI] Admin password has non-ASCII characters; using setup AP password for fallback AP");
    return AP_SETUP_PASS;
  }

  if (password.length() > AP_PASS_MAX_LEN) {
    password.remove(AP_PASS_MAX_LEN);
    Logger.Log("[WIFI] Truncated fallback AP password to %u characters\n", (unsigned int)AP_PASS_MAX_LEN);
  }

  if (!isValidApPassword(password)) {
    Logger.LogLine("[WIFI] Invalid fallback AP password length; using setup AP password");
    return AP_SETUP_PASS;
  }

  return password;
}

static bool connectToSavedStaNetworks(bool showLedFeedback, bool initializeCameraHttpServices) {
  setCpuClockActive("STA connect");

  if (runtimeConfig.wifiList.empty()) {
    Logger.LogLine("[WIFI] No saved STA networks");
    setCpuClockIdleIfPossible("STA idle");
    return false;
  }

  size_t enabledCount = 0;
  for (size_t i = 0; i < runtimeConfig.wifiList.size(); ++i) {
    if (runtimeConfig.wifiList[i].enabled) {
      ++enabledCount;
    }
  }

  if (enabledCount == 0) {
    Logger.LogLine("[WIFI] No enabled STA networks");
    setCpuClockIdleIfPossible("STA idle");
    return false;
  }

  WiFi.persistent(false);
  wifiModemSleepEnabled = false;
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);

  size_t attemptIndex = 0;
  for (size_t i = 0; i < runtimeConfig.wifiList.size(); ++i) {
    const WifiCredential &wifi = runtimeConfig.wifiList[i];
    if (!wifi.enabled) {
      Logger.Log("[WIFI] Skipping disabled network: %s\n", wifi.ssid.c_str());
      continue;
    }

    ++attemptIndex;

    // Hard reset STA state between credential attempts so each SSID starts
    // from a clean state machine and scan context.
    WiFi.disconnect(true, false);
    delay(200);
    WiFi.mode(WIFI_OFF);
    delay(150);
    WiFi.mode(WIFI_STA);
    delay(150);
    WiFi.setSleep(false);

    String staHostname = buildNetworkHostname(cfgDeviceName);
    if (!staHostname.isEmpty()) {
      if (!WiFi.setHostname(staHostname.c_str())) {
        Logger.LogLine("[WIFI] Failed to set STA hostname");
      } else {
        Logger.Log("[WIFI] STA hostname set to: %s (device name: %s)\n",
          staHostname.c_str(),
          cfgDeviceName.c_str());
      }
    }

    if (!applyWifiClientConfig(wifi)) {
      if (showLedFeedback) {
        ledWifiFailureSequence();
      }
      Logger.Log("[WIFI] Skipping network %s due to invalid network configuration\n", wifi.ssid.c_str());
      continue;
    }

    if (wifi.wifiPass.isEmpty()) {
      WiFi.begin(wifi.ssid.c_str());
    } else {
      WiFi.begin(wifi.ssid.c_str(), wifi.wifiPass.c_str());
    }

    if (showLedFeedback) {
      // LED feedback: double blink when testing WiFi credentials
      ledWifiTestSequence();
    }

    Logger.Log("[WIFI] Trying network %u/%u: %s\n",
      (unsigned int)attemptIndex,
      (unsigned int)enabledCount,
      wifi.ssid.c_str());

    int result = (int)WiFi.waitForConnectResult(STA_CONNECT_TIMEOUT_MS);

    // Stop any stale connection attempt before trying the next credential.
    if (result != (int)WL_CONNECTED || WiFi.status() != WL_CONNECTED) {
      // Keep radio powered so periodic reconnect service can continue.
      WiFi.disconnect(false, false);
      delay(100);
    }

    if (result == (int)WL_CONNECTED && WiFi.status() == WL_CONNECTED) {
      staLinkUp = true;
      if (showLedFeedback) {
        // LED feedback: short blink on success
        ledWifiSuccessSequence();
      }

      wifi_power_t staTxPower = validatedTxPowerValue((int)runtimeConfig.txPowerSta, DEFAULT_TX_POWER_STA, "STA");
      WiFi.setTxPower(staTxPower);
      Logger.Log("[WIFI] STA TX power set to %d (raw)\n", (int)staTxPower);
      Logger.Log("[WIFI] Connected to %s — IP: %s\n", wifi.ssid.c_str(), WiFi.localIP().toString().c_str());

      if (initializeCameraHttpServices) {
        registerCameraRoutes();
        server.begin();
        startAuxHttpServers();
        Logger.Log("[HTTP] Camera server ready on ports %u, %u, %u\n",
          (unsigned int)HTTP_MAIN_PORT,
          (unsigned int)HTTP_STREAM_PORT,
          (unsigned int)HTTP_TRANSFER_PORT);
      }

      setWifiModemSleep(true, "idle");
      setCpuClockIdleIfPossible("STA idle");
      return true;
    }

    if (showLedFeedback) {
      // LED feedback: long blink on failure
      ledWifiFailureSequence();
    }
    Logger.Log("[WIFI] Failed to connect to %s (status=%d)\n", wifi.ssid.c_str(), (int)result);
  }

  staLinkUp = false;

  // Make sure STA is still armed for the next periodic reconnect cycle.
  if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
    delay(50);
    WiFi.setSleep(false);
  }

  setCpuClockIdleIfPossible("STA retry idle");
  return false;
}

static void serviceStaReconnect() {
  if (!staConnectedAtBoot) {
    return;
  }

  wifi_mode_t mode = WiFi.getMode();
  bool staCapableMode = (mode == WIFI_STA || mode == WIFI_AP_STA);
  bool linkDown = (!staLinkUp || WiFi.status() != WL_CONNECTED);
  if (!staCapableMode || !linkDown) {
    return;
  }

  unsigned long now = millis();
  if ((now - lastStaReconnectAttemptAt) < STA_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastStaReconnectAttemptAt = now;
  Logger.LogLine("[WIFI] STA link lost — attempting reconnect to saved networks");

  if (connectToSavedStaNetworks(false, false)) {
    Logger.LogLine("[WIFI] STA reconnect successful");
  } else {
    Logger.LogLine("[WIFI] STA reconnect failed; will retry");
  }
}

static void setWifiModemSleep(bool enabled, const char *reason) {
  wifi_mode_t mode = WiFi.getMode();
  bool isStaMode = (mode == WIFI_STA || mode == WIFI_AP_STA);
  bool isApMode  = (mode == WIFI_AP);

  if (!isStaMode && !isApMode) {
    wifiModemSleepEnabled = false;
    return;
  }

  // In STA mode only apply when the station link is up
  if (isStaMode && WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (wifiModemSleepEnabled == enabled) {
    return;
  }

  wifi_ps_type_t sleepType = enabled ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE;
  if (!WiFi.setSleep(sleepType)) {
    if (reason && reason[0] != '\0') {
      Logger.Log("[WIFI] Failed to %s modem sleep (%s)\n",
        enabled ? "enable" : "disable",
        reason);
    } else {
      Logger.Log("[WIFI] Failed to %s modem sleep\n",
        enabled ? "enable" : "disable");
    }
    return;
  }

  wifiModemSleepEnabled = enabled;
  if (reason && reason[0] != '\0') {
    Logger.Log("[WIFI] Modem sleep %s (%s)\n",
      enabled ? "enabled" : "disabled",
      reason);
  } else {
    Logger.Log("[WIFI] Modem sleep %s\n",
      enabled ? "enabled" : "disabled");
  }
}

static bool setCpuClockMhz(uint32_t targetMhz, const char *mode, const char *reason) {
  if (getCpuFrequencyMhz() == targetMhz) {
    return true;
  }

  if (!setCpuFrequencyMhz(targetMhz)) {
    if (reason && reason[0] != '\0') {
      Logger.Log("[CPU] Failed to set %s clock to %lu MHz (%s)\n",
        mode ? mode : "requested",
        (unsigned long)targetMhz,
        reason);
    } else {
      Logger.Log("[CPU] Failed to set %s clock to %lu MHz\n",
        mode ? mode : "requested",
        (unsigned long)targetMhz);
    }
    return false;
  }

  if (reason && reason[0] != '\0') {
    Logger.Log("[CPU] Clock set to %lu MHz (%s)\n",
      (unsigned long)targetMhz,
      reason);
  } else {
    Logger.Log("[CPU] Clock set to %lu MHz\n", (unsigned long)targetMhz);
  }
  return true;
}

static void setCpuClockActive(const char *reason) {
  setCpuClockMhz(CPU_ACTIVE_FREQ_MHZ, "active", reason);
}

static void setCpuClockIdleIfPossible(const char *reason) {
  if (cameraInitialized || hasActiveStreamClients() || firmwareUploadSuccess || adminRestartPending) {
    return;
  }

  setCpuClockMhz(CPU_IDLE_FREQ_MHZ, "idle", reason);
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
      && readI16(buf, offset, settings.streamVisible)
      && readI16(buf, offset, settings.viewRotate90);
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
  appendI16(buf, settings.viewRotate90);
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
  else if (varName == "view_rotate_90") cfg.cameraSettings.viewRotate90 = val ? 1 : 0;
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
    Logger.LogLine("[CAM] Cannot apply stored settings: sensor unavailable");
    return;
  }

  int framesizeResult = 0;
  if (!isValidFrameSizeValue(sensor, cfg.cameraSettings.framesize)) {
    Logger.Log("[CAM] Stored framesize %d is not supported by this sensor\n", cfg.cameraSettings.framesize);
    framesizeResult = -1;
  } else {
    framesizeResult = sensor->set_framesize(sensor, (framesize_t)cfg.cameraSettings.framesize);
    if (framesizeResult == 0) {
      logSensorFrameSize(sensor, "[CAM] Applied stored framesize");
    }
  }

  struct PendingSetting {
    const char *name;
    int result;
  } pending[] = {
    {"framesize", framesizeResult},
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
      Logger.Log("[CAM] Failed to apply stored %s\n", pending[i].name);
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

static bool isValidApSsid(const String &value) {
  return !value.isEmpty()
      && value.length() <= AP_SSID_MAX_LEN
      && isPrintableAscii(value);
}

static bool isValidApPassword(const String &value) {
  if (value.isEmpty()) {
    return true;
  }

  return value.length() >= AP_PASS_MIN_LEN
      && value.length() <= AP_PASS_MAX_LEN
      && isPrintableAscii(value);
}

static bool parseIpv4String(const String &raw, IPAddress &out) {
  String trimmed = raw;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    return false;
  }
  return out.fromString(trimmed);
}

static String normalizedIpv4String(const String &raw) {
  IPAddress ip;
  if (!parseIpv4String(raw, ip)) {
    return "";
  }
  return ip.toString();
}

static bool parseWifiNetworkMode(const String &rawMode, bool &useStaticIp) {
  String mode = rawMode;
  mode.trim();
  mode.toLowerCase();
  if (mode.isEmpty() || mode == "dhcp") {
    useStaticIp = false;
    return true;
  }
  if (mode == "static") {
    useStaticIp = true;
    return true;
  }
  return false;
}

static bool parseBoolString(const String &raw, bool &valueOut) {
  String value = raw;
  value.trim();
  value.toLowerCase();
  if (value == "1" || value == "true" || value == "on") {
    valueOut = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off") {
    valueOut = false;
    return true;
  }
  return false;
}

static bool parseWifiNetworkArgs(WifiCredential &wifi, String &errorOut) {
  bool useStaticIp = false;
  String modeValue = server.hasArg(PARAM_NET_MODE) ? server.arg(PARAM_NET_MODE) : "dhcp";
  if (!parseWifiNetworkMode(modeValue, useStaticIp)) {
    errorOut = "Invalid network mode";
    return false;
  }

  String ip = server.hasArg(PARAM_STATIC_IP) ? server.arg(PARAM_STATIC_IP) : "";
  String gw = server.hasArg(PARAM_GATEWAY) ? server.arg(PARAM_GATEWAY) : "";
  String mask = server.hasArg(PARAM_SUBNET) ? server.arg(PARAM_SUBNET) : "";
  String dns1 = server.hasArg(PARAM_DNS1) ? server.arg(PARAM_DNS1) : "";
  String dns2 = server.hasArg(PARAM_DNS2) ? server.arg(PARAM_DNS2) : "";

  ip.trim();
  gw.trim();
  mask.trim();
  dns1.trim();
  dns2.trim();

  if (useStaticIp) {
    if (ip.isEmpty() || gw.isEmpty() || mask.isEmpty()) {
      errorOut = "Static mode requires IP, gateway, and subnet";
      return false;
    }
    String normalizedIp = normalizedIpv4String(ip);
    String normalizedGw = normalizedIpv4String(gw);
    String normalizedMask = normalizedIpv4String(mask);
    if (normalizedIp.isEmpty() || normalizedGw.isEmpty() || normalizedMask.isEmpty()) {
      errorOut = "Invalid static IPv4 address values";
      return false;
    }
    wifi.staticIp = normalizedIp;
    wifi.gateway = normalizedGw;
    wifi.subnet = normalizedMask;
  } else {
    wifi.staticIp = "";
    wifi.gateway = "";
    wifi.subnet = "";
  }

  if (!dns1.isEmpty()) {
    String normalizedDns1 = normalizedIpv4String(dns1);
    if (normalizedDns1.isEmpty()) {
      errorOut = "Invalid DNS 1 IPv4 address";
      return false;
    }
    wifi.dns1 = normalizedDns1;
  } else {
    wifi.dns1 = "";
  }

  if (!dns2.isEmpty()) {
    String normalizedDns2 = normalizedIpv4String(dns2);
    if (normalizedDns2.isEmpty()) {
      errorOut = "Invalid DNS 2 IPv4 address";
      return false;
    }
    wifi.dns2 = normalizedDns2;
  } else {
    wifi.dns2 = "";
  }

  wifi.useStaticIp = useStaticIp;
  return true;
}

static bool applyWifiClientConfig(const WifiCredential &wifi) {
  IPAddress none((uint32_t)0U);
  IPAddress dns1 = none;
  IPAddress dns2 = none;

  if (!wifi.dns1.isEmpty() && !dns1.fromString(wifi.dns1)) {
    Logger.Log("[WIFI] Invalid DNS1 for %s: %s\n", wifi.ssid.c_str(), wifi.dns1.c_str());
    return false;
  }
  if (!wifi.dns2.isEmpty() && !dns2.fromString(wifi.dns2)) {
    Logger.Log("[WIFI] Invalid DNS2 for %s: %s\n", wifi.ssid.c_str(), wifi.dns2.c_str());
    return false;
  }

  if (wifi.useStaticIp) {
    IPAddress ip;
    IPAddress gw;
    IPAddress mask;
    if (!ip.fromString(wifi.staticIp) || !gw.fromString(wifi.gateway) || !mask.fromString(wifi.subnet)) {
      Logger.Log("[WIFI] Invalid static IP config for %s\n", wifi.ssid.c_str());
      return false;
    }

    if (dns1 == none) {
      dns1 = gw;
    }

    bool ok = WiFi.config(ip, gw, mask, dns1, dns2);
    Logger.Log("[WIFI] %s static config for %s\n", ok ? "Applied" : "Failed to apply", wifi.ssid.c_str());
    return ok;
  }

  bool ok = (dns1 == none && dns2 == none)
      ? WiFi.config(none, none, none)
      : WiFi.config(none, none, none, dns1, dns2);
  if (!ok) {
    Logger.Log("[WIFI] Failed to apply DHCP config for %s\n", wifi.ssid.c_str());
  }
  return ok;
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

static void sendHtmlWithToken(String &page) {
  page.replace("__ROUTE_TOKEN__", routeAccessToken);
  server.send(HTTP_OK, "text/html", page);
}

static void sendHtmlWithToken(const char *html) {
  String page(html);
  sendHtmlWithToken(page);
}

enum class AppPage {
  Camera,
  Admin
};

static String buildDevicePageTitle(const char *pageLabel) {
  String deviceName = cfgDeviceName;
  deviceName.trim();
  if (deviceName.isEmpty()) {
    deviceName = "ESP32-CAM";
  }

  if (pageLabel == nullptr || *pageLabel == '\0') {
    return deviceName;
  }

  String title = deviceName;
  title += " - ";
  title += pageLabel;
  return title;
}

static String buildAppNavLink(const char *href, const char *label, AppPage page, AppPage activePage) {
  String html;
  html.reserve(80);
  html += "<a href=\"";
  html += href;
  html += "\"";
  if (page == activePage) {
    html += " style=\"color:#e94560\"";
  }
  html += ">";
  html += label;
  html += "</a>";
  return html;
}

static String buildAppNav(AppPage activePage) {
  String html;
  html.reserve(180);
  html += "<nav>\n";
  html += "  ";
  html += buildAppNavLink("/", "📷 Camera", AppPage::Camera, activePage);
  html += "\n  ";
  html += buildAppNavLink("/admin", "⚙️ Admin", AppPage::Admin, activePage);
  html += "\n</nav>";
  return html;
}

static const char kFooterLicenseText[] = R"LICENSE(Copyright 2026 Andrea Esuli <andrea@esuli.it>

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.)LICENSE";

static String buildAppFooter() {
  String html;
  html.reserve(3600);
  html += "<footer style=\"padding:14px 20px;color:#7c8aa6;font-size:.82em;text-align:center\">";
  html += "ESP32-CAM &middot; Firmware ";
  html += FIRMWARE_VERSION_TEXT;
  html += " &middot; &copy; 2026 <a href=\"https://esuli.it\" target=\"_blank\" rel=\"noopener noreferrer\" style=\"color:#93c5fd\">Andrea Esuli</a> &middot; ";
  html += "<a href=\"#\" onclick=\"return toggleLicenseBox(true)\" style=\"color:#93c5fd\">License</a>";
  html += "<div id=\"license-modal\" style=\"display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);align-items:center;justify-content:center;padding:16px;z-index:9999\" onclick=\"return toggleLicenseBox(false)\">";
  html += "<div style=\"width:min(760px,100%);max-height:85vh;overflow:auto;background:#0f172a;color:#cbd5e1;border:1px solid #334155;border-radius:12px;padding:14px;box-shadow:0 20px 50px rgba(0,0,0,.4);text-align:left\" onclick=\"event.stopPropagation()\">";
  html += "<div style=\"display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:10px\"><strong style=\"font-size:1rem;color:#e2e8f0\">License</strong><button type=\"button\" onclick=\"return toggleLicenseBox(false)\" style=\"background:#1e293b;color:#e2e8f0;border:1px solid #475569;border-radius:6px;padding:4px 9px;cursor:pointer\">Close</button></div>";
  html += "<pre style=\"white-space:pre-wrap;word-break:break-word;margin:0;padding:10px;border-radius:8px;border:1px solid #334155;background:#020617;color:#dbeafe;font-size:.86rem;line-height:1.45\">";
  html += kFooterLicenseText;
  html += "</pre>";
  html += "<div style=\"margin-top:10px;color:#94a3b8\">Website: <a href=\"https://esuli.it\" target=\"_blank\" rel=\"noopener noreferrer\" style=\"color:#93c5fd\">https://esuli.it</a></div>";
  html += "</div></div>";
  html += "<script>(function(){if(window.__licenseBoxReady){return;}window.__licenseBoxReady=true;window.toggleLicenseBox=function(open){var m=document.getElementById('license-modal');if(!m){return false;}m.style.display=open?'flex':'none';return false;};document.addEventListener('keydown',function(e){if(e.key==='Escape'){window.toggleLicenseBox(false);}});}());</script>";
  html += "</footer>";
  return html;
}

static const char *buildAppPageLabel(AppPage activePage) {
  switch (activePage) {
    case AppPage::Camera: return "Camera";
    case AppPage::Admin: return "Admin";
  }

  return nullptr;
}

static void applyAppChrome(String &page, AppPage activePage, const char *pageLabel) {
  page.replace("__APP_NAV__", buildAppNav(activePage));
  page.replace("__APP_FOOTER__", buildAppFooter());
  page.replace("__PAGE_TITLE__", buildDevicePageTitle(pageLabel));
}

static void sendAppHtmlWithToken(String &page, AppPage activePage, const char *pageLabel) {
  applyAppChrome(page, activePage, pageLabel);
  sendHtmlWithToken(page);
}

static void sendAppHtmlWithToken(const char *html, AppPage activePage, const char *pageLabel) {
  String page(html);
  sendAppHtmlWithToken(page, activePage, pageLabel);
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
  plain.reserve(cfg.adminPass.length() + cfg.deviceName.length() + cfg.wifiList.size() * 96 + 160);

  uint16_t wifiCount = (uint16_t)cfg.wifiList.size();
  plain.push_back((uint8_t)(wifiCount & 0xFF));
  plain.push_back((uint8_t)((wifiCount >> 8) & 0xFF));
  for (size_t i = 0; i < cfg.wifiList.size(); ++i) {
    appendField(plain, cfg.wifiList[i].ssid);
    appendField(plain, cfg.wifiList[i].wifiPass);
    appendU8(plain, cfg.wifiList[i].enabled ? 1 : 0);
    appendU8(plain, cfg.wifiList[i].useStaticIp ? 1 : 0);
    appendField(plain, cfg.wifiList[i].staticIp);
    appendField(plain, cfg.wifiList[i].gateway);
    appendField(plain, cfg.wifiList[i].subnet);
    appendField(plain, cfg.wifiList[i].dns1);
    appendField(plain, cfg.wifiList[i].dns2);
  }
  appendField(plain, cfg.adminPass);
  appendField(plain, cfg.deviceName);
  appendU8(plain, cfg.hasCameraSettings ? 1 : 0);
  if (cfg.hasCameraSettings) {
    appendCameraSettings(plain, cfg.cameraSettings);
  }
  appendU8(plain, cfg.ledAccessBlink ? 1 : 0);
  appendU8(plain, (uint8_t)cfg.txPowerSta);
  appendU8(plain, (uint8_t)cfg.txPowerAp);

  return encryptPayload(plain, ivHex, cipherHex);
}

static bool decryptConfig(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
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
    uint8_t enabled = 1;
    uint8_t useStaticIp = 0;
    if (!readField(plain, offset, wifi.ssid)) return false;
    if (!readField(plain, offset, wifi.wifiPass)) return false;
    if (!readU8(plain, offset, enabled)) return false;
    wifi.enabled = (enabled != 0);
    if (!readU8(plain, offset, useStaticIp)) return false;
    wifi.useStaticIp = (useStaticIp != 0);
    if (!readField(plain, offset, wifi.staticIp)) return false;
    if (!readField(plain, offset, wifi.gateway)) return false;
    if (!readField(plain, offset, wifi.subnet)) return false;
    if (!readField(plain, offset, wifi.dns1)) return false;
    if (!readField(plain, offset, wifi.dns2)) return false;
    cfg.wifiList.push_back(wifi);
  }
  if (!readField(plain, offset, cfg.adminPass)) {
    return false;
  }
  if (!readField(plain, offset, cfg.deviceName)) return false;

  uint8_t hasCameraSettings = 0;
  if (!readU8(plain, offset, hasCameraSettings)) return false;

  cfg.hasCameraSettings = (hasCameraSettings != 0);
  if (cfg.hasCameraSettings && !readCameraSettings(plain, offset, cfg.cameraSettings)) {
    return false;
  }

  uint8_t ledAccessBlink = 0;
  if (!readU8(plain, offset, ledAccessBlink)) return false;
  cfg.ledAccessBlink = (ledAccessBlink != 0);

  uint8_t txPowerSta = (uint8_t)DEFAULT_TX_POWER_STA;
  if (!readU8(plain, offset, txPowerSta)) return false;
  cfg.txPowerSta = (int8_t)txPowerSta;

  uint8_t txPowerAp = (uint8_t)DEFAULT_TX_POWER_AP;
  if (!readU8(plain, offset, txPowerAp)) return false;
  cfg.txPowerAp = (int8_t)txPowerAp;

  // Consume legacy trailing bytes from older config formats.
  uint8_t legacy = 0;
  if (offset < plain.size()) {
    if (!readU8(plain, offset, legacy)) return false;
  }

  if (offset < plain.size()) {
    if (!readU8(plain, offset, legacy)) return false;
  }

  return offset == plain.size() && !cfg.adminPass.isEmpty();
}

static bool saveConfigToSD(const StoredConfig &cfg) {
  String ivHex;
  String cipherHex;
  if (!encryptConfig(cfg, ivHex, cipherHex)) {
    Logger.LogLine("[CFG] Encryption failed");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NVS_NAMESPACE, false)) {
    Logger.LogLine("[CFG] Failed to open NVS for write");
    return false;
  }

  size_t magicLen = prefs.putString(CONFIG_NVS_MAGIC_KEY, CONFIG_FILE_MAGIC);
  size_t ivLen = prefs.putString(CONFIG_NVS_IV_KEY, ivHex);
  size_t cipherLen = prefs.putString(CONFIG_NVS_CIPHER_KEY, cipherHex);
  prefs.end();

  if (magicLen == 0 || ivLen == 0 || cipherLen == 0) {
    Logger.LogLine("[CFG] Failed to save encrypted config in NVS");
    return false;
  }

  Logger.LogLine("[CFG] Encrypted config saved to NVS");
  return true;
}

static bool loadConfigFromSD(StoredConfig &cfg) {
  Preferences prefs;
  if (!prefs.begin(CONFIG_NVS_NAMESPACE, true)) {
    Logger.LogLine("[CFG] Failed to open NVS for read");
    return false;
  }

  String magic = prefs.getString(CONFIG_NVS_MAGIC_KEY, "");
  String ivHex = prefs.getString(CONFIG_NVS_IV_KEY, "");
  String cipherHex = prefs.getString(CONFIG_NVS_CIPHER_KEY, "");
  prefs.end();

  if (magic.isEmpty() || ivHex.isEmpty() || cipherHex.isEmpty()) {
    Logger.LogLine("[CFG] Config missing in NVS");
    return false;
  }

  magic.trim();
  ivHex.trim();
  cipherHex.trim();

  if (magic != CONFIG_FILE_MAGIC) {
    Logger.Log("[CFG] Unsupported config magic: %s\n", magic.c_str());
    return false;
  }

  if (!decryptConfig(ivHex, cipherHex, cfg)) {
    Logger.Log("[CFG] Failed to decrypt %s config\n", magic.c_str());
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


#include "modules/ui_pages.inc.h"

// ─── Camera initialisation ────────────────────────────────────────────────────
static void powerUpCameraHardware() {
  if (PWDN_GPIO_NUM < 0) {
    return;
  }

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(300);
}

static void configureCameraPins(camera_config_t &config, uint32_t xclkFreqHz) {
  config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = xclkFreqHz;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.frame_size = FRAMESIZE_SXGA;
  config.jpeg_quality = 25;
  config.fb_count = 2;
}

static void applySensorDefaults() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor && sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
  }

  applyStoredCameraSettings(runtimeConfig);
}

static void resetFlashOutput() {
  setFlashOutput(false);
}

static void setFlashOutput(bool enabled) {
  pinMode(LED_FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(LED_FLASH_GPIO_NUM, enabled ? HIGH : LOW);
  flashEnabled = enabled;
}


static bool initCamera(uint32_t xclkFreqHz) {
  powerUpCameraHardware();

  camera_config_t config;
  configureCameraPins(config, xclkFreqHz);

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Logger.Log("[CAM] Init failed: 0x%x\n", err);
    return false;
  }

  applySensorDefaults();
  resetFlashOutput();
  return true;
}

static bool initCameraWithRetries() {
  const size_t xclkCount = sizeof(CAMERA_XCLK_FREQS_HZ) / sizeof(CAMERA_XCLK_FREQS_HZ[0]);

  for (int attempt = 1; attempt <= CAMERA_INIT_RETRIES; ++attempt) {
    uint32_t xclkHz = CAMERA_XCLK_FREQS_HZ[(size_t)(attempt - 1) % xclkCount];

    Logger.Log("[CAM] Init attempt %d/%d using XCLK=%lu Hz\n",
                  attempt,
                  CAMERA_INIT_RETRIES,
                  (unsigned long)xclkHz);

    if (initCamera(xclkHz)) {
      if (attempt > 1) {
        Logger.Log("[CAM] Init succeeded on attempt %d (XCLK=%lu Hz)\n",
                      attempt,
                      (unsigned long)xclkHz);
      }
      return true;
    }

    esp_camera_deinit();
    Logger.Log("[CAM] Retry %d/%d\n", attempt, CAMERA_INIT_RETRIES);
    delay(CAMERA_INIT_RETRY_DELAY_MS);
  }

  return false;
}

static bool loadRuntimeConfigWithRetries(StoredConfig &cfg) {
  for (int attempt = 1; attempt <= CONFIG_LOAD_RETRIES; ++attempt) {
    if (loadConfigFromSD(cfg)) {
      if (attempt > 1) {
        Logger.Log("[CFG] Loaded config on attempt %d\n", attempt);
      }
      return true;
    }

    if (attempt < CONFIG_LOAD_RETRIES) {
      Logger.Log("[CFG] Load attempt %d/%d failed, retrying in %lu ms\n",
        attempt,
        CONFIG_LOAD_RETRIES,
        (unsigned long)CONFIG_LOAD_RETRY_DELAY_MS);
      delay(CONFIG_LOAD_RETRY_DELAY_MS);
    }
  }

  return false;
}



static bool startSoftAPWithRetries(const char *ssid, const char *password, const char *hostname = nullptr) {
  for (int attempt = 1; attempt <= AP_START_RETRIES; ++attempt) {
    WiFi.mode(WIFI_AP);
    if (hostname && hostname[0] != '\0') {
      if (!WiFi.softAPsetHostname(hostname)) {
        Logger.Log("[WIFI] Failed to set AP hostname on attempt %d\n", attempt);
      }
    }

    bool started = false;
    if (password && password[0] != '\0') {
      started = WiFi.softAP(ssid, password, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS);
    } else {
      started = WiFi.softAP(ssid, nullptr, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS);
    }

    if (started) {
      if (attempt > 1) {
        Logger.Log("[WIFI] AP start succeeded on attempt %d\n", attempt);
      }
      return true;
    }

    Logger.Log("[WIFI] AP start attempt %d/%d failed\n", attempt, AP_START_RETRIES);
    delay(AP_START_RETRY_DELAY_MS);
  }

  return false;
}

// Forward declaration
static bool checkAuth();
static bool checkAuth(WebServer &srv, bool allowSharedToken = false);
static bool hasSharedAccessToken(WebServer &srv);
static String buildLocalUrl(uint16_t port, const String &path, bool withToken = false);
static void handleStreamMain();
static void handleStreamClose();
static void handleStreamWorker();
static void handleFirmwareUploadMain();
static void handleFirmwareUploadWorker();
static void handleFirmwareUploadDataWorker();
static void streamServerTask(void *arg);
static void transferServerTask(void *arg);
static void startAuxHttpServers();
static void registerAdminRoutes();
static void registerOtaRoutes();
static void registerOtaTransferRoutes();

static bool isValidRuntimeTxPowerValue(int value) {
  switch ((wifi_power_t)value) {
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

static wifi_power_t validatedTxPowerValue(int configuredValue, wifi_power_t fallback, const char *label) {
  if (isValidRuntimeTxPowerValue(configuredValue)) {
    return (wifi_power_t)configuredValue;
  }

  Logger.Log("[WIFI] Invalid %s TX power value in config: %d; using default %d\n",
    label ? label : "WiFi",
    configuredValue,
    (int)fallback);
  return fallback;
}

static bool hasSharedAccessToken(WebServer &srv) {
  if (routeAccessToken.isEmpty() || !srv.hasArg("t")) {
    return false;
  }

  return srv.arg("t") == routeAccessToken;
}

static bool checkAuth(WebServer &srv, bool allowSharedToken) {
  // LED feedback for URL access blink (if enabled)
  if (ledAccessBlinkEnabled) {
    unsigned long now = millis();
    if (now - lastUrlAccessBlink > LED_ACCESS_BLINK_INTERVAL_MS) {
      ledQuickBlink();
      lastUrlAccessBlink = now;
    }
  }

  if (cfgAccessPass.isEmpty()) {
    return true;
  }

  if (allowSharedToken && hasSharedAccessToken(srv)) {
    return true;
  }

  if (!srv.authenticate("admin", cfgAccessPass.c_str())) {
    srv.requestAuthentication(BASIC_AUTH, "ESP32-CAM");
    return false;
  }

  return true;
}

static bool checkAuth() {
  return checkAuth(server);
}

static String buildLocalUrl(uint16_t port, const String &path, bool withToken) {
  String url = "http://";

  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    url += WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    url += WiFi.localIP().toString();
  } else {
    url += "127.0.0.1";
  }

  url += ":";
  url += String(port);
  url += path;

  if (withToken && !routeAccessToken.isEmpty()) {
    url += (path.indexOf('?') >= 0) ? "&t=" : "?t=";
    url += routeAccessToken;
  }

  return url;
}

static bool writeMjpegStreamFrame(WiFiClient &client, const uint8_t *data, size_t len) {
  if (!data || len == 0U) {
    return false;
  }

  char partHeader[128];
  int hlen = snprintf(partHeader, sizeof(partHeader),
      "--jpgbound\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n"
      "\r\n",
      (unsigned int)len);
  if (hlen <= 0 || (size_t)hlen >= sizeof(partHeader)) {
    return false;
  }

  bool ok = (client.write((const uint8_t *)partHeader, (size_t)hlen) == (size_t)hlen);
  if (ok) ok = (client.write(data, len) == len);
  if (ok) ok = (client.print("\r\n") > 0);
  return ok;
}

static bool streamCameraFramebufferDirect(WiFiClient &client) {
  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return true;
  }

  bool ok = fb->format == PIXFORMAT_JPEG && writeMjpegStreamFrame(client, fb->buf, fb->len);
  unlockCameraFrame(fb);
  return ok;
}

static uint16_t streamPortForSlot(uint8_t slotIndex) {
  return slotIndex == 0 ? HTTP_STREAM_PORT : HTTP_STREAM_PORT_ALT;
}

static bool hasActiveStreamClients() {
  if (!streamSlotMutex) {
    return false;
  }

  SemaphoreLock lock(streamSlotMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return false;
  }

  for (uint8_t i = 0; i < STREAM_SLOT_COUNT; ++i) {
    if (streamSlotStates[i] == StreamSlotState::Active) {
      return true;
    }
  }

  return false;
}

static void cleanupExpiredStreamReservationsLocked(unsigned long now) {
  for (uint8_t i = 0; i < STREAM_SLOT_COUNT; ++i) {
    if (streamSlotStates[i] == StreamSlotState::Reserved &&
        (now - streamSlotReservedAt[i]) >= STREAM_SLOT_RESERVATION_TIMEOUT_MS) {
      streamSlotStates[i] = StreamSlotState::Free;
      streamSlotReservedAt[i] = 0;
    }
  }
}

static void cleanupExpiredStreamReservations(unsigned long now) {
  if (!streamSlotMutex) {
    return;
  }

  SemaphoreLock lock(streamSlotMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return;
  }

  cleanupExpiredStreamReservationsLocked(now);
}

static int reserveStreamSlot() {
  if (!streamSlotMutex) {
    return -1;
  }

  SemaphoreLock lock(streamSlotMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return -1;
  }

  unsigned long now = millis();
  cleanupExpiredStreamReservationsLocked(now);

  for (uint8_t i = 0; i < STREAM_SLOT_COUNT; ++i) {
    if (streamSlotStates[i] == StreamSlotState::Free) {
      streamSlotStates[i] = StreamSlotState::Reserved;
      streamSlotReservedAt[i] = now;
      return (int)i;
    }
  }

  return -1;
}

static bool activateStreamSlot(uint8_t slotIndex) {
  if (!streamSlotMutex || slotIndex >= STREAM_SLOT_COUNT) {
    return false;
  }

  SemaphoreLock lock(streamSlotMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return false;
  }

  if (streamSlotStates[slotIndex] == StreamSlotState::Active) {
    return false;
  }

  streamSlotStates[slotIndex] = StreamSlotState::Active;
  streamSlotReservedAt[slotIndex] = 0;
  return true;
}

static void releaseStreamSlot(uint8_t slotIndex) {
  if (!streamSlotMutex || slotIndex >= STREAM_SLOT_COUNT) {
    return;
  }

  SemaphoreLock lock(streamSlotMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return;
  }

  streamSlotStates[slotIndex] = StreamSlotState::Free;
  streamSlotReservedAt[slotIndex] = 0;
}

static void handleStreamRequest(uint8_t slotIndex, bool allowSharedToken) {
  WebServer &srv = (slotIndex == 0 ? streamServer : streamServerAlt);
  if (!checkAuth(srv, allowSharedToken)) {
    return;
  }

  if (!ensureCameraReady()) {
    srv.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera unavailable");
    return;
  }

  if (!activateStreamSlot(slotIndex)) {
    srv.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Stream busy");
    return;
  }

  setWifiModemSleep(false, "active stream");

  WiFiClient client = srv.client();
  client.setNoDelay(true);
  client.setTimeout(1000);
  Logger.LogLine("[STREAM] Client connected");
  unsigned long lastFrameAt = 0;

  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=jpgbound\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n"
  );

  while (client.connected()) {
    unsigned long now = millis();
    if (lastFrameAt != 0) {
      unsigned long elapsed = now - lastFrameAt;
      if (elapsed < STREAM_FRAME_INTERVAL_MS) {
        delay(STREAM_FRAME_INTERVAL_MS - elapsed);
        continue;
      }
    }

    now = millis();
    bool ok = streamCameraFramebufferDirect(client);

    lastFrameAt = now;

    if (!ok) {
      break;
    }

    lastCameraActivityAt = millis();
  }

  lastCameraActivityAt = millis();
  client.stop();
  releaseStreamSlot(slotIndex);
  if (!hasActiveStreamClients()) {
    setWifiModemSleep(true, "idle");
  }
  Logger.LogLine("[STREAM] Client disconnected");
}

static void handleStreamWorker() {
  handleStreamRequest(0, true);
}

static void handleStreamWorkerAlt() {
  handleStreamRequest(1, true);
}

static void handleStreamMain() {
  if (!checkAuth(server)) {
    return;
  }

  int slotIndex = reserveStreamSlot();
  if (slotIndex < 0) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "All stream slots are busy");
    return;
  }

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Location", buildLocalUrl(streamPortForSlot((uint8_t)slotIndex), "/stream", true));
  server.send(HTTP_FOUND, "text/plain", "Redirecting to stream server");
}

static void handleStreamClose() {
  if (!checkAuth(server)) {
    return;
  }

  server.send(HTTP_OK, "text/plain", "OK");
}

// ─── Route handlers: AP (setup) mode ─────────────────────────────────────────
static void handleUrlAccess() {
  // LED feedback for URL access blink (if enabled)
  if (ledAccessBlinkEnabled) {
    unsigned long now = millis();
    if (now - lastUrlAccessBlink > LED_ACCESS_BLINK_INTERVAL_MS) {
      ledQuickBlink();
      lastUrlAccessBlink = now;
    }
  }
}

static void handleSetupRoot() {
    handleUrlAccess();
  String page(SETUP_HTML);
  page.replace("__PAGE_TITLE__", buildDevicePageTitle("Setup"));
  server.send(HTTP_OK, "text/html", page);
}

static void handleSave() {
    handleUrlAccess();

  if (!server.hasArg(PARAM_SSID) || !server.hasArg(PARAM_APASS)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Missing required fields");
        return;
    }

  String newSSID  = server.arg(PARAM_SSID);
  String newWPass = server.arg(PARAM_WPASS);
  String newAPass = server.arg(PARAM_APASS);

    if (newSSID.isEmpty()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "SSID is required");
        return;
    }
    if (newAPass.length() < 8) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Access password must be at least 8 characters");
        return;
    }
    if (newAPass.length() > AP_PASS_MAX_LEN) {
      server.send(HTTP_BAD_REQUEST, "text/plain", "Access password must be at most 63 characters");
          return;
      }

    String newDeviceName = server.hasArg("dname") ? server.arg("dname") : "ESP32-CAM";
    newDeviceName.trim();
    if (newDeviceName.isEmpty()) {
      newDeviceName = "ESP32-CAM";
    }
    if (newDeviceName.length() > AP_SSID_MAX_LEN) {
      server.send(HTTP_BAD_REQUEST, "text/plain", "Device name must be between 1 and 32 characters");
      return;
    }

      if (!isPrintableAscii(newSSID) || !isPrintableAscii(newWPass) || !isPrintableAscii(newAPass) || !isPrintableAscii(newDeviceName)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid characters in input");
        return;
    }

    StoredConfig cfg;
    WifiCredential wifi;
    wifi.ssid = newSSID;
    wifi.wifiPass = newWPass;
    wifi.enabled = true;
    String wifiNetworkError;
    if (!parseWifiNetworkArgs(wifi, wifiNetworkError)) {
      server.send(HTTP_BAD_REQUEST, "text/plain", wifiNetworkError);
      return;
    }
    cfg.wifiList.push_back(wifi);
    cfg.adminPass = newAPass;
    cfg.deviceName = newDeviceName;

    if (!persistRuntimeConfig(cfg)) {
      server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
      return;
    }

    server.send_P(HTTP_OK, "text/html", SAVED_HTML);
    delay(2000);
    ESP.restart();
}

// ─── Route handlers: STA (camera) mode ───────────────────────────────────────
static void handleCameraRoot() {
    if (!checkAuth()) return;
    framesize_t selected = FRAMESIZE_VGA;
    if (runtimeConfig.hasCameraSettings && runtimeConfig.cameraSettings.framesize >= 0) {
      selected = (framesize_t)runtimeConfig.cameraSettings.framesize;
    }

    String page(MAIN_HTML);
    page.replace("__FRAME_SIZE_OPTIONS__", buildFrameSizeOptionsHtml(selected));
    sendAppHtmlWithToken(page, AppPage::Camera, "Camera");
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

  setCpuClockActive("WiFi scan");

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
    server.send(HTTP_INTERNAL_ERROR, "application/json", "{\"error\":\"WiFi scan failed\"}");
    setCpuClockIdleIfPossible("WiFi scan complete");
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
  server.send(HTTP_OK, "application/json", json);
  setCpuClockIdleIfPossible("WiFi scan complete");
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
    json += "\"enabled\":";
    json += runtimeConfig.wifiList[i].enabled ? "true" : "false";
    json += ",";
    json += "\"hasPassword\":";
    json += runtimeConfig.wifiList[i].wifiPass.isEmpty() ? "false" : "true";
    json += ",\"netmode\":\"";
    json += runtimeConfig.wifiList[i].useStaticIp ? "static" : "dhcp";
    json += "\",";
    json += "\"ip\":\"" + jsonEscape(runtimeConfig.wifiList[i].staticIp) + "\",";
    json += "\"gw\":\"" + jsonEscape(runtimeConfig.wifiList[i].gateway) + "\",";
    json += "\"mask\":\"" + jsonEscape(runtimeConfig.wifiList[i].subnet) + "\",";
    json += "\"dns1\":\"" + jsonEscape(runtimeConfig.wifiList[i].dns1) + "\",";
    json += "\"dns2\":\"" + jsonEscape(runtimeConfig.wifiList[i].dns2) + "\"";
    json += '}';
  }
  json += "]}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(HTTP_OK, "application/json", json);
}

static void handleWifiAdd() {
  if (!checkAuth()) return;
  if (!server.hasArg(PARAM_SSID)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "SSID is required");
    return;
  }

  String newSSID = server.arg(PARAM_SSID);
  String newWPass = server.arg(PARAM_WPASS);
  newSSID.trim();

  if (newSSID.isEmpty()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "SSID is required");
    return;
  }
  if (!isPrintableAscii(newSSID) || !isPrintableAscii(newWPass)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid characters in input");
    return;
  }

  WifiCredential incoming;
  incoming.ssid = newSSID;
  incoming.wifiPass = newWPass;

  bool hasEnabledArg = server.hasArg("enabled");
  bool requestedEnabled = true;
  if (hasEnabledArg && !parseBoolString(server.arg("enabled"), requestedEnabled)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid enabled value");
    return;
  }
  incoming.enabled = requestedEnabled;

  String wifiNetworkError;
  if (!parseWifiNetworkArgs(incoming, wifiNetworkError)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", wifiNetworkError);
    return;
  }

  StoredConfig updated = runtimeConfig;
  bool replaced = false;
  for (size_t i = 0; i < updated.wifiList.size(); ++i) {
    if (updated.wifiList[i].ssid == newSSID) {
      if (!hasEnabledArg) {
        incoming.enabled = updated.wifiList[i].enabled;
      }
      updated.wifiList[i] = incoming;
      replaced = true;
      break;
    }
  }

  if (!replaced) {
    updated.wifiList.push_back(incoming);
  }

  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", replaced ? "WiFi credential updated" : "WiFi credential added");
}

static void handleWifiSetEnabled() {
  if (!checkAuth()) return;
  if (!server.hasArg(PARAM_INDEX) || !server.hasArg("enabled")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Index and enabled are required");
    return;
  }

  int index = server.arg(PARAM_INDEX).toInt();
  if (index < 0 || (size_t)index >= runtimeConfig.wifiList.size()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid WiFi index");
    return;
  }

  bool enabled = false;
  if (!parseBoolString(server.arg("enabled"), enabled)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid enabled value");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.wifiList[index].enabled = enabled;
  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", enabled ? "WiFi credential enabled" : "WiFi credential disabled");
}

static void handleWifiDelete() {
  if (!checkAuth()) return;
  if (!server.hasArg(PARAM_INDEX)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Index is required");
    return;
  }

  int index = server.arg(PARAM_INDEX).toInt();
  if (index < 0 || (size_t)index >= runtimeConfig.wifiList.size()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid WiFi index");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.wifiList.erase(updated.wifiList.begin() + index);
  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "WiFi credential deleted");
}

static void handleWifiMove() {
  if (!checkAuth()) return;
  if (!server.hasArg(PARAM_INDEX) || !server.hasArg("dir")) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Index and dir are required");
    return;
  }

  int index = server.arg(PARAM_INDEX).toInt();
  String dir = server.arg("dir");
  int target = dir == "up" ? index - 1 : (dir == "down" ? index + 1 : -1);

  if (index < 0 || target < 0 || (size_t)index >= runtimeConfig.wifiList.size() || (size_t)target >= runtimeConfig.wifiList.size()) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid WiFi move request");
    return;
  }

  StoredConfig updated = runtimeConfig;
  WifiCredential temp = updated.wifiList[index];
  updated.wifiList[index] = updated.wifiList[target];
  updated.wifiList[target] = temp;

  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  server.send(HTTP_OK, "text/plain", "WiFi priority updated");
}

static void handleAdminPasswordChange() {
  if (!checkAuth()) return;
  if (!server.hasArg(PARAM_CURRENT) || !server.hasArg(PARAM_NEXT) || !server.hasArg(PARAM_CONFIRM)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Current, next, and confirm passwords are required");
    return;
  }

  String currentPass = server.arg(PARAM_CURRENT);
  String nextPass = server.arg(PARAM_NEXT);
  String confirmPass = server.arg(PARAM_CONFIRM);

  if (currentPass != cfgAccessPass) {
    server.send(HTTP_FORBIDDEN, "text/plain", "Current password is incorrect");
    return;
  }
  if (nextPass.length() < 8) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "New password must be at least 8 characters");
    return;
  }
  if (nextPass.length() > AP_PASS_MAX_LEN) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "New password must be at most 63 characters");
    return;
  }
  if (nextPass != confirmPass) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "New password confirmation does not match");
    return;
  }
  if (!isPrintableAscii(nextPass) || !isPrintableAscii(confirmPass) || !isPrintableAscii(currentPass)) {
    server.send(HTTP_BAD_REQUEST, "text/plain", "Invalid characters in password");
    return;
  }

  StoredConfig updated = runtimeConfig;
  updated.adminPass = nextPass;
  if (!persistRuntimeConfig(updated)) {
    server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to save configuration");
    return;
  }

  String message = "Admin password updated. Your browser will need the new password for subsequent requests.";
  if (WiFi.getMode() == WIFI_AP) {
    message += " Fallback AP password changes on the next AP restart.";
  }
  server.send(HTTP_OK, "text/plain", message);
}

static void handleCapture() {
    if (!checkAuth()) return;

  if (!ensureCameraReady()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera unavailable");
    return;
  }

    camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
    if (!fb) {
      server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera capture failed");
        return;
    }

    OwnedJpegFrame frame;
    bool copied = copyCameraFrame(fb, frame);
    unlockCameraFrame(fb);
    if (!copied) {
      server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera frame copy failed");
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
        (unsigned int)frame.len
    );
    client.write(frame.data, frame.len);
}

static void handleControl() {
    if (!checkAuth()) return;

    if (!server.hasArg("var") || !server.hasArg("val")) {
      server.send(HTTP_BAD_REQUEST, "text/plain", "Missing var or val parameter");
        return;
    }

    String varName = server.arg("var");
    int    val     = server.arg("val").toInt();
    bool   persist = !server.hasArg("persist") || server.arg("persist") != "0";

    // Handle non-sensor controls separately.
    if (varName == "flash") {
        setFlashOutput(val != 0);
        Logger.LogLine(val ? "[FLASH] Enabled" : "[FLASH] Disabled");
        server.send(HTTP_OK, "text/plain", "OK");
        return;
    }

      if (varName == "stream_visible" || varName == "view_rotate_90") {
        updateStoredCameraSetting(runtimeConfig, varName, val);
        if (persist && !persistRuntimeConfig(runtimeConfig)) {
          server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to persist view setting");
          return;
        }
        server.send(HTTP_OK, "text/plain", "OK");
        return;
      }

      if (!ensureCameraReady()) {
        server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera unavailable");
        return;
      }

    SemaphoreLock cameraLock(cameraMutex, pdMS_TO_TICKS(1500));
    if (!cameraLock.locked()) {
      server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera busy");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!cameraInitialized || !s) {
      server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera sensor not available");
        return;
    }

    int res = 0;
    int statusCode = 200;
    const char *message = "OK";
    if (varName == "framesize") {
      if (!isValidFrameSizeValue(s, val)) {
            statusCode = 400;
            message = "Unsupported resolution";
        } else {
            res = s->set_framesize(s, (framesize_t)val);
            if (res == 0) {
                logSensorFrameSize(s, "[CAM] Updated framesize");
            }
        }
    } else if (varName == "quality")        res = s->set_quality(s, val);
    else if (varName == "brightness")       res = s->set_brightness(s, val);
    else if (varName == "contrast")         res = s->set_contrast(s, val);
    else if (varName == "saturation")       res = s->set_saturation(s, val);
    else if (varName == "sharpness")        res = s->set_sharpness(s, val);
    else if (varName == "special_effect")   res = s->set_special_effect(s, val);
    else if (varName == "awb")              res = s->set_whitebal(s, val);
    else if (varName == "awb_gain")         res = s->set_awb_gain(s, val);
    else if (varName == "wb_mode")          res = s->set_wb_mode(s, val);
    else if (varName == "aec")              res = s->set_exposure_ctrl(s, val);
    else if (varName == "aec2")             res = s->set_aec2(s, val);
    else if (varName == "aec_value")        res = s->set_aec_value(s, val);
    else if (varName == "ae_level")         res = s->set_ae_level(s, val);
    else if (varName == "agc")              res = s->set_gain_ctrl(s, val);
    else if (varName == "agc_gain")         res = s->set_agc_gain(s, val);
    else if (varName == "gainceiling")      res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (varName == "bpc")              res = s->set_bpc(s, val);
    else if (varName == "wpc")              res = s->set_wpc(s, val);
    else if (varName == "raw_gma")          res = s->set_raw_gma(s, val);
    else if (varName == "lenc")             res = s->set_lenc(s, val);
    else if (varName == "hmirror")          res = s->set_hmirror(s, val);
    else if (varName == "vflip")            res = s->set_vflip(s, val);
    else if (varName == "dcw")              res = s->set_dcw(s, val);
    else if (varName == "colorbar")         res = s->set_colorbar(s, val);
    else {
        server.send(HTTP_BAD_REQUEST, "text/plain", "Unknown variable");
        return;
    }

    if (statusCode == 200 && res == 0) {
      lastCameraActivityAt = millis();
    }
    cameraLock.release();

    if (statusCode != 200) {
      server.send(statusCode, "text/plain", message);
      return;
    }

    if (res == 0) {
      updateStoredCameraSetting(runtimeConfig, varName, val);
      if (persist && !persistRuntimeConfig(runtimeConfig)) {
        server.send(HTTP_INTERNAL_ERROR, "text/plain", "Failed to persist camera setting");
        return;
      }
    }

    server.send(HTTP_OK, "text/plain", res == 0 ? "OK" : "ERROR");
}

static void sendFlashlightStatus() {
  String json = "{\"enabled\":";
  json += flashEnabled ? "true" : "false";
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(HTTP_OK, "application/json", json);
}

static bool readFlashlightRequestedState(bool &enabled) {
  if (server.hasArg("enabled") && parseBoolString(server.arg("enabled"), enabled)) {
    return true;
  }
  if (server.hasArg("state") && parseBoolString(server.arg("state"), enabled)) {
    return true;
  }
  if (server.hasArg("val") && parseBoolString(server.arg("val"), enabled)) {
    return true;
  }
  if (server.hasArg("plain") && parseBoolString(server.arg("plain"), enabled)) {
    return true;
  }
  return false;
}

static void handleFlashlight() {
  if (!checkAuth()) return;

  bool enabled = false;
  if (readFlashlightRequestedState(enabled)) {
    setFlashOutput(enabled);
    Logger.LogLine(enabled ? "[FLASH] Enabled" : "[FLASH] Disabled");
  }

  sendFlashlightStatus();
}

static void handleStatus() {
    if (!checkAuth()) return;

  if (!ensureCameraReady()) {
    server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera unavailable");
    return;
  }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
      server.send(HTTP_SERVICE_UNAVAILABLE, "text/plain", "Camera sensor not available");
        return;
    }

    char json[640];
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
        "\"flashlight\":%u,"
        "\"stream_visible\":%u,"
        "\"view_rotate_90\":%u"
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
        flashEnabled ? 1U : 0U,
        runtimeConfig.cameraSettings.streamVisible ? 1U : 0U,
        runtimeConfig.cameraSettings.viewRotate90 ? 1U : 0U
    );

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(HTTP_OK, "application/json", json);
}

static void handleNotFound() {
    server.send(HTTP_NOT_FOUND, "text/plain", "Not found");
}

#include "modules/admin_module.inc.h"

#include "modules/ota_module.inc.h"

static void streamServerTask(void *arg) {
  WebServer *srv = static_cast<WebServer *>(arg);
  for (;;) {
    srv->handleClient();
    vTaskDelay(1);
  }
}

static void transferServerTask(void *arg) {
  WebServer *srv = static_cast<WebServer *>(arg);
  for (;;) {
    srv->handleClient();
    vTaskDelay(1);
  }
}

static void startAuxHttpServers() {
  if (!streamServerTaskHandle) {
    streamServer.on("/stream", HTTP_GET, handleStreamWorker);
    streamServer.onNotFound([]() {
      streamServer.send(HTTP_NOT_FOUND, "text/plain", "Not found");
    });
    streamServer.begin();
    BaseType_t created = xTaskCreatePinnedToCore(
      streamServerTask,
      "http-stream",
      HTTP_STREAM_TASK_STACK,
      &streamServer,
      1,
      &streamServerTaskHandle,
      ARDUINO_RUNNING_CORE
    );
    if (created == pdPASS) {
      Logger.Log("[HTTP] Stream server ready on port %u\n", (unsigned int)HTTP_STREAM_PORT);
    } else {
      streamServerTaskHandle = nullptr;
      Logger.LogLine("[HTTP] Failed to start stream server task");
    }
  }

  if (!streamServerAltTaskHandle) {
    streamServerAlt.on("/stream", HTTP_GET, handleStreamWorkerAlt);
    streamServerAlt.onNotFound([]() {
      streamServerAlt.send(HTTP_NOT_FOUND, "text/plain", "Not found");
    });
    streamServerAlt.begin();
    BaseType_t created = xTaskCreatePinnedToCore(
      streamServerTask,
      "http-stream-2",
      HTTP_STREAM_TASK_STACK,
      &streamServerAlt,
      1,
      &streamServerAltTaskHandle,
      ARDUINO_RUNNING_CORE
    );
    if (created == pdPASS) {
      Logger.Log("[HTTP] Stream server ready on port %u\n", (unsigned int)HTTP_STREAM_PORT_ALT);
    } else {
      streamServerAltTaskHandle = nullptr;
      Logger.LogLine("[HTTP] Failed to start secondary stream server task");
    }
  }

  if (!transferServerTaskHandle) {
    registerOtaTransferRoutes();
    transferServer.onNotFound([]() {
      transferServer.send(HTTP_NOT_FOUND, "text/plain", "Not found");
    });
    transferServer.begin();
    BaseType_t created = xTaskCreatePinnedToCore(
      transferServerTask,
      "http-transfer",
      HTTP_TRANSFER_TASK_STACK,
      &transferServer,
      1,
      &transferServerTaskHandle,
      ARDUINO_RUNNING_CORE
    );
    if (created == pdPASS) {
      Logger.Log("[HTTP] Transfer server ready on port %u\n", (unsigned int)HTTP_TRANSFER_PORT);
    } else {
      transferServerTaskHandle = nullptr;
      Logger.LogLine("[HTTP] Failed to start transfer server task");
    }
  }
}

// ─── WiFi mode starters ───────────────────────────────────────────────────────
static void registerCameraRoutes() {
  server.on("/",              HTTP_GET,  handleCameraRoot);
  server.on("/stream",        HTTP_GET,  handleStreamMain);
  server.on("/stream/close",  HTTP_POST, handleStreamClose);
  server.on("/snapshot",      HTTP_GET,  handleCapture);
  server.on("/snapshot.jpg",  HTTP_GET,  handleCapture);
  server.on("/flashlight",    HTTP_GET,  handleFlashlight);
  server.on("/flashlight",    HTTP_POST, handleFlashlight);
  server.on("/control",       HTTP_GET,  handleControl);
  server.on("/status",        HTTP_GET,  handleStatus);
  server.on("/wifi/scan",     HTTP_GET,  handleWifiScan);

  registerAdminRoutes();
  registerOtaRoutes();

  server.onNotFound(handleNotFound);
}

static void startSetupAPMode() {
  setCpuClockActive("setup AP start");

  wifiModemSleepEnabled = false;
  bool ok = startSoftAPWithRetries(AP_SETUP_SSID, AP_SETUP_PASS);
  if (!ok) {
    Logger.LogLine("[WIFI] Setup AP start failed");
    return;
  }
    ledSetupAPSequence();
    Logger.Log("[WIFI] Protected setup AP started — SSID: %s  Password: %s  IP: %s\n",
    AP_SETUP_SSID, AP_SETUP_PASS, WiFi.softAPIP().toString().c_str());

    server.on("/",     HTTP_GET,  handleSetupRoot);
    server.on("/wifi/scan", HTTP_GET, handleSetupWifiScan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();
    Logger.Log("[HTTP] Setup server ready on port %u\n",
      (unsigned int)HTTP_MAIN_PORT);
    setCpuClockIdleIfPossible("setup AP idle");
}

static void startCameraAPMode() {
  setCpuClockActive("fallback AP start");

  wifiModemSleepEnabled = false;
  String apHostname = buildNetworkHostname(cfgDeviceName);
  String apSsid = buildFallbackApSsid();
  String apPassword = buildFallbackApPassword();
  bool ok = startSoftAPWithRetries(apSsid.c_str(), apPassword.c_str(), apHostname.c_str());
  if (!ok) {
    Logger.LogLine("[WIFI] Fallback AP start failed (check password length >= 8)");
    return;
  }

  Logger.Log("[WIFI] AP hostname set to: %s (device name: %s)\n",
    apHostname.c_str(),
    cfgDeviceName.c_str());

  // Reduce TX power — client is always nearby in fallback mode
  wifi_power_t apTxPower = validatedTxPowerValue((int)runtimeConfig.txPowerAp, DEFAULT_TX_POWER_AP, "AP");
  WiFi.setTxPower(apTxPower);
  Logger.Log("[WIFI] Fallback AP TX power set to %d (raw)\n", (int)apTxPower);

  if (AP_FALLBACK_EXTENDED_BEACON_ENABLED) {
    // Optional power optimization disabled by default for AP stability.
    wifi_config_t apCfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &apCfg) == ESP_OK) {
      apCfg.ap.beacon_interval = AP_FALLBACK_BEACON_INTERVAL_TU;
      if (esp_wifi_set_config(WIFI_IF_AP, &apCfg) == ESP_OK) {
        Logger.Log("[WIFI] Fallback AP: beacon_interval=%u TU\n",
                      AP_FALLBACK_BEACON_INTERVAL_TU);
      } else {
        Logger.LogLine("[WIFI] Failed to apply extended fallback AP config");
      }
    } else {
      Logger.LogLine("[WIFI] Failed to read fallback AP config");
    }
  }

  // Keep modem sleep disabled in fallback AP mode for better runtime stability.
  if (AP_FALLBACK_MODEM_SLEEP_ENABLED) {
    WiFi.setSleep(WIFI_PS_MAX_MODEM);
    wifiModemSleepEnabled = true;
    Logger.Log("[WIFI] Fallback AP power: reduced TX + modem sleep enabled\n");
  } else {
    WiFi.setSleep(false);
    wifiModemSleepEnabled = false;
    Logger.LogLine("[WIFI] Fallback AP stability mode: modem sleep disabled");
  }
  // LED feedback: triple blink when fallback AP activated
  ledFallbackAPSequence();

  Logger.Log("[WIFI] Fallback AP started — SSID: %s  IP: %s\n",
    apSsid.c_str(), WiFi.softAPIP().toString().c_str());

  registerCameraRoutes();
  server.begin();
  startAuxHttpServers();
  Logger.Log("[HTTP] Camera server ready on ports %u, %u, %u (AP mode)\n",
    (unsigned int)HTTP_MAIN_PORT,
    (unsigned int)HTTP_STREAM_PORT,
    (unsigned int)HTTP_TRANSFER_PORT);
  setCpuClockIdleIfPossible("fallback AP idle");
}

static void startSTAMode() {
  setCpuClockActive("STA start");

  if (runtimeConfig.wifiList.empty()) {
    Logger.LogLine("[WIFI] No saved STA networks — switching to fallback AP");
    startCameraAPMode();
    return;
  }

  if (connectToSavedStaNetworks(true, true)) {
    staConnectedAtBoot = true;
    lastStaReconnectAttemptAt = millis();
    return;
  }

  Logger.LogLine("[WIFI] All saved networks failed — switching to fallback AP");
  startCameraAPMode();
}

static void servicePendingFirmwareRestart() {
  if (!firmwareUploadSuccess || firmwareRestartAt == 0) {
    return;
  }

  unsigned long now = millis();
  if ((long)(now - firmwareRestartAt) < 0) {
    return;
  }

  Logger.LogLine("[OTA] Restarting after successful firmware update");
  delay(100);
  ESP.restart();
}

static void servicePendingAdminRestart() {
  if (!adminRestartPending || adminRestartAt == 0) {
    return;
  }

  unsigned long now = millis();
  if ((long)(now - adminRestartAt) < 0) {
    return;
  }

  adminRestartPending = false;
  adminRestartAt = 0;
  Logger.LogLine("[ADMIN] Restarting on admin request");
  delay(100);
  ESP.restart();
}

static bool requestFactoryResetAndReboot(const char *origin) {
  Preferences prefs;
  if (!prefs.begin(CONFIG_NVS_NAMESPACE, false)) {
    Logger.LogLine("[CFG] Failed to open NVS for factory reset");
    return false;
  }

  prefs.clear();
  prefs.end();

  adminRestartPending = true;
  adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
  Logger.Log("[CFG] Factory reset requested via %s. Rebooting shortly.\n",
             origin ? origin : "unknown");
  return true;
}

static void processSerialConsoleCommand(const String &rawCommand) {
  String command = rawCommand;
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP") {
    Logger.LogLine("[SERIAL] Commands:");
    Logger.LogLine("[SERIAL]   REBOOT");
    Logger.LogLine("[SERIAL]   FACTORY_RESET <boot_code>");
    Logger.LogLine("[SERIAL]   RESET <boot_code>");
    return;
  }

  if (upper == "REBOOT") {
    adminRestartPending = true;
    adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
    Logger.LogLine("[SERIAL] Reboot command accepted. Rebooting...");
    return;
  }

  String providedCode;
  const String prefixFull = "FACTORY_RESET ";
  const String prefixShort = "RESET ";
  if (upper.startsWith(prefixFull)) {
    providedCode = upper.substring(prefixFull.length());
  } else if (upper.startsWith(prefixShort)) {
    providedCode = upper.substring(prefixShort.length());
  } else {
    Logger.LogLine("[SERIAL] Unknown command. Use HELP.");
    return;
  }

  providedCode.trim();
  if (providedCode.isEmpty()) {
    Logger.LogLine("[SERIAL] Missing reset code. Use: FACTORY_RESET <boot_code>");
    return;
  }

  if (!serialFactoryResetArmed) {
    Logger.LogLine("[SERIAL] Factory reset code is no longer valid. Reboot for a new code.");
    return;
  }

  if (providedCode != serialFactoryResetCode) {
    Logger.LogLine("[SERIAL] Invalid factory reset code.");
    return;
  }

  serialFactoryResetArmed = false;
  if (!requestFactoryResetAndReboot("serial")) {
    serialFactoryResetArmed = true;
    Logger.LogLine("[SERIAL] Factory reset failed.");
    return;
  }

  Logger.LogLine("[SERIAL] Factory reset accepted. Rebooting...");
}

static void serviceSerialConsole() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    serialLastInputAt = millis();
    if (c == '\r' || c == '\n') {
      if (!serialCommandBuffer.isEmpty()) {
        processSerialConsoleCommand(serialCommandBuffer);
        serialCommandBuffer = "";
      }
      continue;
    }

    if (c < 32 || c > 126) {
      continue;
    }

    if (serialCommandBuffer.length() < SERIAL_COMMAND_MAX_LEN) {
      serialCommandBuffer += c;
    }
  }

  if (!serialCommandBuffer.isEmpty()) {
    unsigned long now = millis();
    if ((now - serialLastInputAt) >= SERIAL_COMMAND_IDLE_FLUSH_MS) {
      processSerialConsoleCommand(serialCommandBuffer);
      serialCommandBuffer = "";
    }
  }
}

static void initializeSerialFactoryResetChallenge() {
  char codeBuffer[9];
  snprintf(codeBuffer, sizeof(codeBuffer), "%08lX", (unsigned long)esp_random());
  serialFactoryResetCode = String(codeBuffer);
  serialCommandBuffer = "";
  serialLastInputAt = millis();
  serialFactoryResetArmed = true;

  Logger.Log("[SERIAL] Factory reset code for this boot: %s\n", serialFactoryResetCode.c_str());
  Logger.LogLine("[SERIAL] To factory reset send: FACTORY_RESET <boot_code>");
  Logger.LogLine("[SERIAL] Serial console ready. Type HELP for commands.");
}

[[noreturn]] static void haltBoot(const char *message) {
  Logger.LogLine(message);
  for (;;) {
    delay(1000);
  }
}

static bool initializeRuntimeMutexes() {
  cameraMutex = xSemaphoreCreateMutex();
  streamSlotMutex = xSemaphoreCreateMutex();
  return cameraMutex && streamSlotMutex;
}

static void initializeWakeupIndicator() {
  initLED();
  ledBootSequence(); // Always run boot sequence
}

static void initializeBootPins() {
  pinMode(LED_FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(LED_FLASH_GPIO_NUM, LOW);
  powerDownCameraHardware();
}


static void applyLoadedStartupConfig(const StoredConfig &cfg) {
  runtimeConfig = cfg;
  cfgAccessPass = cfg.adminPass;
  cfgDeviceName = cfg.deviceName;
  ledAccessBlinkEnabled = cfg.ledAccessBlink;
  if (cfgDeviceName.isEmpty()) {
    cfgDeviceName = "ESP32-CAM";
  }
  isConfigured = true;
}

static void applyDefaultStartupConfig() {
  runtimeConfig = StoredConfig();
  cfgDeviceName = "ESP32-CAM";
  ledAccessBlinkEnabled = false;
  isConfigured = false;
}

static void loadStartupConfig() {
  StoredConfig cfg;
  if (loadRuntimeConfigWithRetries(cfg)) {
    applyLoadedStartupConfig(cfg);
  } else {
    applyDefaultStartupConfig();
  }
}


static void initializeRouteAccessToken() {
  routeAccessToken = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX);
  Logger.Log("[HTTP] Shared route token initialized (%u chars)\n", (unsigned int)routeAccessToken.length());
}

static void startInitialNetworkServices() {
  Logger.Log("[CFG] Configured: %s\n", isConfigured ? "yes" : "no");
  Logger.Log("[CAM] Lazy init enabled with idle timeout %lu ms\n", cameraIdleTimeoutMs);

  if (isConfigured) {
    startSTAMode();
  } else {
    startSetupAPMode();
  }
}

// ─── Arduino entry points ─────────────────────────────────────────────────────
void setup() {
  Logger.begin(115200);
  Logger.LogLine("[BOOT] *** ESP32-CAM starting ***");
  setCpuClockActive("boot");
  initializeSerialFactoryResetChallenge();
  WiFi.onEvent(onWifiEvent);

  initializeWakeupIndicator();
  if (!initializeRuntimeMutexes()) {
    haltBoot("[BOOT] Failed to create runtime mutexes - halting");
  }

  initializeBootPins();

  loadStartupConfig();
  initializeRouteAccessToken();
  startInitialNetworkServices();
}

void loop() {
  serviceSerialConsole();
  cleanupExpiredStreamReservations(millis());
  server.handleClient();
  serviceStaReconnect();
  serviceCameraIdleTimeout();
  servicePendingFirmwareRestart();
  servicePendingAdminRestart();
  delay(2);
}
