/**
 * ESP32-CAM Multipurpose Firmware
 *
 * Hardware:
 *   - AI-Thinker ESP32-CAM with OV3660 camera sensor
 *   - microSD in 1-bit mode so SDMMC does not actively drive GPIO12/GPIO13
 *   - GPIO13 reserved for push button (shared with SD DAT3 pull network)
 *   - GPIO12 reserved for PIR input (shared with SD DAT2 pull network)
 *
 * First Boot (unconfigured or missing config file):
 *   Broadcasts protected WiFi AP "ESP32-CAM-Setup"
 *   with password "ESP32-CAM".
 *   Visit http://192.168.4.1 to enter WiFi credentials and an
 *   access password. Credentials are encrypted and stored on SD.
 *
 * Normal Operation:
 *   Port 80 — web UI with live MJPEG stream (/stream) and camera controls.
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
#include <Update.h>
#include <esp_bt.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>
#include <mbedtls/aes.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdarg>
#include "camera_pins.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// ─── Pin definitions ──────────────────────────────────────────────────────────
// Button wiring: one side to GPIO13, other side to GND (INPUT_PULLUP, active LOW).
// PIR wiring: VCC -> 5V (or module-supported rail), DATA -> GPIO12, GND -> GND.
// Note: GPIO12/GPIO13 remain electrically tied to SD DAT2/DAT3 while the card is mounted.
static constexpr int BUTTON_GPIO = 13;
static constexpr int PIR_GPIO    = 12;
static constexpr int LED_GPIO    = 33;  // Internal red LED on ESP32-CAM
static constexpr unsigned long BUTTON_DEBOUNCE_MS = 40;
static constexpr unsigned long BUTTON_SLEEP_DELAY_MS = 1000;
static constexpr unsigned long BUTTON_BLINK_ON_MS = 70;
static constexpr unsigned long BUTTON_BLINK_OFF_MS = 70;
static constexpr bool APP_UART_CONSOLE_ENABLED = true;

// ─── AP setup credentials ─────────────────────────────────────────────────────
#define AP_SETUP_SSID   "ESP32-CAM-Setup"
#define AP_SETUP_PASS   "ESP32-CAM"
#define AP_FALLBACK_SSID "ESP32-CAM"
static constexpr int AP_CHANNEL = 1;
static constexpr bool AP_HIDDEN = false;
static constexpr int AP_MAX_CONNECTIONS = 4;
// Default TX power levels (overridden by stored config if available)
static constexpr wifi_power_t DEFAULT_TX_POWER_STA = WIFI_POWER_19_5dBm;
static constexpr wifi_power_t DEFAULT_TX_POWER_AP  = WIFI_POWER_8_5dBm;
// Beacon interval for fallback AP in TU (1 TU = 1024 µs). Default is 100; Must be a multiple of 100, range 100–60000.
static constexpr uint16_t AP_FALLBACK_BEACON_INTERVAL_TU = 10000;
static constexpr const char *NTP_SERVER = "pool.ntp.org";
static constexpr const char *TIME_ZONE = "BRT3";
static constexpr int CONFIG_LOAD_RETRIES = 5;
static constexpr unsigned long CONFIG_LOAD_RETRY_DELAY_MS = 1000;
static constexpr int CAMERA_INIT_RETRIES = 8;
static constexpr unsigned long CAMERA_INIT_RETRY_DELAY_MS = 500;
static constexpr unsigned long BOOT_RECOVERY_RESTART_DELAY_MS = 5000;
static constexpr int AP_START_RETRIES = 3;
static constexpr unsigned long AP_START_RETRY_DELAY_MS = 1000;
static constexpr uint32_t CAMERA_XCLK_FREQS_HZ[] = {
  20000000UL,
  10000000UL,
  8000000UL,
  4000000UL
};

// ─── SD configuration storage ──────────────────────────────────────────────────
#define CONFIG_FILE_PATH "/config.enc"
#define CAPTURE_COUNTER_FILE_PATH "/capture_counter.txt"
#define SD_SORT_FILE_PATH "/.sort"

// ─── Globals ──────────────────────────────────────────────────────────────────
static constexpr uint16_t HTTP_MAIN_PORT = 80;
static constexpr uint16_t HTTP_STREAM_PORT = 81;
static constexpr uint16_t HTTP_TRANSFER_PORT = 82;
static constexpr uint32_t HTTP_STREAM_TASK_STACK = 8192;
static constexpr uint32_t HTTP_TRANSFER_TASK_STACK = 8192;

static WebServer   server(HTTP_MAIN_PORT);
static WebServer   streamServer(HTTP_STREAM_PORT);
static WebServer   transferServer(HTTP_TRANSFER_PORT);

static String cfgAccessPass;
static String cfgDeviceName;
static String routeAccessToken;
static bool   isConfigured = false;
static bool   recordingActive = false;
static volatile bool streamClientConnected = false;
static volatile bool streamClientAbortRequested = false;
static bool   flashEnabled = false;
static bool   cameraInitialized = false;
static bool   ledAccessBlinkEnabled = false;
static bool   wifiModemSleepEnabled = false;
static bool   staConnectedAtBoot = false;
static bool   buttonLastRawPressed = false;
static bool   buttonStablePressed = false;
static bool   buttonSleepArmed = true;
static bool   buttonSleepRequestPending = false;
static bool   motionRawHigh = false;
static bool   motionLatched = false;
static bool   motionBootEventPending = false;
static volatile bool motionEdgePending = false;
static volatile uint32_t motionEdgeCount = 0;
static uint8_t motionPendingImages = 0;
static bool   motionVideoManagedRecording = false;
static bool   motionActionWindowActive = false;
static volatile bool staLinkUp = false;
static unsigned long lastUrlAccessBlink = 0;
static unsigned long lastCameraActivityAt = 0;
static unsigned long lastStaReconnectAttemptAt = 0;
static unsigned long buttonLastChangeAt = 0;
static unsigned long buttonSleepRequestAt = 0;
static unsigned long motionHighSinceAt = 0;
static unsigned long motionLastDetectedAt = 0;
static unsigned long motionLastActivityAt = 0;
static unsigned long motionNextImageAt = 0;
static unsigned long motionRecordingStopAt = 0;
static unsigned long motionIgnoreUntilAt = 0;
static esp_sleep_wakeup_cause_t bootWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
static constexpr unsigned long LED_ACCESS_BLINK_INTERVAL_MS = 100;  // Minimum interval between access blinks
static constexpr unsigned long STA_RECONNECT_INTERVAL_MS = 30000;
static constexpr unsigned long STA_CONNECT_TIMEOUT_MS = 20000;
static unsigned long cameraIdleTimeoutMs = 3000;
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
static bool   sdUploadBlocked = false;
static String sdUploadPath;
static bool   firmwareUploadFailed = false;
static bool   firmwareUploadSuccess = false;
static unsigned long firmwareRestartAt = 0;
static bool   adminRestartPending = false;
static unsigned long adminRestartAt = 0;
static uint32_t captureSequence = 0;
static bool captureSequenceLoaded = false;
static SemaphoreHandle_t cameraMutex = nullptr;
static SemaphoreHandle_t recordingMutex = nullptr;
static TaskHandle_t streamServerTaskHandle = nullptr;
static TaskHandle_t transferServerTaskHandle = nullptr;
static constexpr unsigned long STREAM_FRAME_INTERVAL_MS = 100;
static constexpr unsigned long RECORDING_FRAME_INTERVAL_MS = 100;
static constexpr unsigned long FIRMWARE_RESTART_DELAY_MS = 1500;
static constexpr uint32_t AVI_HAS_INDEX_FLAG = 0x00000010UL;
static constexpr uint32_t AVI_KEYFRAME_FLAG = 0x00000010UL;
static constexpr size_t AVI_HEADER_SIZE = 224;
static constexpr uint32_t AVI_MOVI_LIST_HEADER_SIZE = 4;
static constexpr const char *CAPTURE_DIRECTORY = "/capture";
static constexpr const char *AVI_VIDEO_CHUNK_ID = "00dc";
static constexpr const char *SERIAL_LOG_FILE_PATH = "/log.txt";
static constexpr const char *FIRMWARE_VERSION_TEXT = FIRMWARE_VERSION;
static constexpr const char *FIRMWARE_BUILD_TEXT = __DATE__ " " __TIME__;

static bool initSDCard();

static bool gLogWriteInProgress = false;
static bool gLogSdReady = false;
static bool gLogSdFailureReported = false;
static bool gLogFileFailureReported = false;
static bool gLoggingEnabled = true;
static bool gLogSdMirrorEnabled = false;
static bool gSdCardMounted = false;

static bool appendSerialLogChunk(const uint8_t *data, size_t len) {
  if (!gLoggingEnabled || !gLogSdMirrorEnabled || !data || len == 0 || gLogWriteInProgress) {
    return false;
  }

  gLogWriteInProgress = true;

  if (!gLogSdReady) {
    gLogSdReady = initSDCard();
    if (!gLogSdReady) {
      gLogSdFailureReported = true;
      gLogWriteInProgress = false;
      return false;
    }
  }

  if (!SD_MMC.exists(SERIAL_LOG_FILE_PATH)) {
    File createFile = SD_MMC.open(SERIAL_LOG_FILE_PATH, FILE_WRITE);
    if (!createFile) {
      gLogFileFailureReported = true;
      gLogWriteInProgress = false;
      return false;
    }
    createFile.close();
  }

  File file = SD_MMC.open(SERIAL_LOG_FILE_PATH, FILE_APPEND);
  if (!file) {
    gLogFileFailureReported = true;
    gLogWriteInProgress = false;
    return false;
  }

  size_t written = file.write(data, len);
  file.flush();
  file.close();

  if (written != len && !gLogFileFailureReported) {
    gLogFileFailureReported = true;
  }

  gLogWriteInProgress = false;
  return written == len;
}

class SerialMirror : public Print {
 public:
  void begin(unsigned long baud) {
    if (APP_UART_CONSOLE_ENABLED) {
      ::Serial.begin(baud);
    } else {
      (void)baud;
      ::Serial.end();
    }
  }

  size_t write(uint8_t b) override {
    if (!gLoggingEnabled) {
      return 1;
    }
    appendSerialLogChunk(&b, 1);
    if (APP_UART_CONSOLE_ENABLED) {
      return ::Serial.write(b);
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (!gLoggingEnabled) {
      return size;
    }
    appendSerialLogChunk(buffer, size);
    if (APP_UART_CONSOLE_ENABLED) {
      return ::Serial.write(buffer, size);
    }
    return size;
  }

  int printf(const char *format, ...) {
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
      write((const uint8_t *)buffer.data(), (size_t)written);
    }

    return written;
  }
};

static SerialMirror LogSerial;

// Redirect this translation unit's Serial prints to a mirrored logger.
#define Serial LogSerial

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

struct MotionSettings {
  bool enabled = false;
  bool captureImage = false;
  uint8_t imageCount = 1;            // 1..10
  uint8_t imageDelayDs = 1;          // deciseconds: 1..20 (0.1s..2.0s)
  bool captureVideo = false;
  uint8_t videoDurationSec = 5;      // 1..30
  bool wakeOnMotion = false;
  bool autoStandby = false;
  uint16_t standbyAfterSec = 30;     // 5..120
  uint16_t detectionIntervalSec = 0; // 0,5,10,30,60,600
};

struct StoredConfig {
  std::vector<WifiCredential> wifiList;
  String adminPass;
  String deviceName;
  bool hasCameraSettings = false;
  CameraSettings cameraSettings;
  MotionSettings motionSettings;
  bool ledAccessBlink = false;  // LED blink on URL access
  bool loggingEnabled = true;
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
static bool syncClockWithNtp();
static camera_fb_t *lockAndCaptureFrame(TickType_t timeoutTicks = pdMS_TO_TICKS(1000));
static void unlockCameraFrame(camera_fb_t *fb);
static bool ensureCameraReady(TickType_t timeoutTicks = pdMS_TO_TICKS(5000));
static void serviceCameraIdleTimeout();
static bool isRecordingFrameDue(unsigned long now);
static bool recordFrameIfDue(camera_fb_t *fb, unsigned long now);
static bool appendRecordingFrame(camera_fb_t *fb);
static void stopRecordingSession(bool keepFile);
static bool loadRuntimeConfigWithRetries(StoredConfig &cfg);
static bool initCameraWithRetries();
static bool waitForIO0Released(unsigned long timeoutMs);
static void servicePendingFirmwareRestart();
static void servicePendingAdminRestart();
static void configureButtonWakeup();
static void configureMotionWakeup(bool enabled);
static void applyPirInputMode();
static void restoreInputPinsAfterSDInit();
static void logSharedPinCaveats();
static void updateSdLoggingState();
static bool pirSupportsRtcWakeup();
static void IRAM_ATTR onPirEdgeInterrupt();
static void handleWakeupIndicator();
static void serviceButtonSleepRequest();
static void serviceMotionDetection();
static void serviceMotionActions();
static void serviceMotionAutoStandby();
static void triggerMotionEvent(const char *source);
static void closeMotionActionWindow();
static bool captureImageToSD(String &savedPath);
static bool startRecordingSessionInternal(String &message);
static bool stopRecordingSessionInternal(String &message);
static void handleMotionGraphPage();
static void handleMotionReadings();
static bool isValidMotionIntervalSec(uint16_t seconds);
static void clampMotionSettings(MotionSettings &settings);
static void handleMotionPage();
static void handleMotionConfigGet();
static void handleMotionConfigSet();
static void prepareDeviceForDeepSleep();
[[noreturn]] static void enterDeepSleepFromButton();
[[noreturn]] static void enterDeepSleepNow(const char *reason, int blinkCount, bool allowMotionWake);
static void setWifiModemSleep(bool enabled, const char *reason = nullptr);
static void registerCameraRoutes();
static void startAuxHttpServers();

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WIFI] STA associated with AP");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staLinkUp = true;
      Serial.printf("[WIFI] STA got IP: %s\n", WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staLinkUp = false;
      Serial.printf("[WIFI] STA disconnected (reason=%u)\n",
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
    Serial.printf("%s: invalid framesize %d\n", prefix, sensor ? sensor->status.framesize : -1);
    return;
  }

  const resolution_info_t &info = resolution[sensor->status.framesize];
  Serial.printf("%s: %s (%ux%u, enum=%d)\n",
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

  digitalWrite(LED_FLASH_GPIO_NUM, LOW);
  flashEnabled = false;
}

static bool initSDCard() {
  if (gSdCardMounted) {
    return true;
  }

  // 1-bit mode stops SDMMC from actively using DAT1/DAT2/DAT3, but the socket
  // and the card still keep GPIO12/GPIO13 electrically tied to DAT2/DAT3.
  // Retry several times: SD cards can be slow to respond on cold boot.
  for (int attempt = 1; attempt <= 5; ++attempt) {
    bool mounted = SD_MMC.begin("/sdcard", true);
    restoreInputPinsAfterSDInit();
    if (mounted) {
      if (SD_MMC.cardType() != CARD_NONE) {
        gSdCardMounted = true;
        Serial.printf("[SD] Mounted in 1-bit mode (attempt %d)\n", attempt);
        Serial.printf("[SD] GPIO%d/GPIO%d remain shared with SD DAT2/DAT3 pull-ups while a card is inserted\n",
          BUTTON_GPIO, PIR_GPIO);
        return true;
      }
      SD_MMC.end();
      gSdCardMounted = false;
      restoreInputPinsAfterSDInit();
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

static bool ensureCameraReady(TickType_t timeoutTicks) {
  if (!cameraMutex) {
    return false;
  }

  if (xSemaphoreTake(cameraMutex, timeoutTicks) != pdTRUE) {
    return false;
  }

  bool ok = true;
  if (!cameraInitialized) {
    Serial.println("[CAM] Powering up camera on demand");
    ok = initCameraWithRetries();
    if (ok) {
      cameraInitialized = true;
      Serial.println("[CAM] Camera ready");
    } else {
      Serial.println("[CAM] Camera init failed");
    }
  }

  if (ok) {
    lastCameraActivityAt = millis();
  }

  xSemaphoreGive(cameraMutex);
  return ok;
}

static void serviceCameraIdleTimeout() {
  if (!cameraInitialized || streamClientConnected || recordingActive || cameraIdleTimeoutMs == 0UL) {
    return;
  }

  unsigned long now = millis();
  if (lastCameraActivityAt != 0 && (now - lastCameraActivityAt) < cameraIdleTimeoutMs) {
    return;
  }

  if (!cameraMutex || xSemaphoreTake(cameraMutex, 0) != pdTRUE) {
    return;
  }

  if (cameraInitialized && !streamClientConnected && !recordingActive) {
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
      Serial.printf("[CAM] Deinit failed: 0x%x\n", err);
    } else {
      cameraInitialized = false;
      powerDownCameraHardware();
      Serial.printf("[CAM] Camera powered down after %lu ms idle\n", cameraIdleTimeoutMs);
    }
  }

  xSemaphoreGive(cameraMutex);
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

static bool connectToSavedStaNetworks(bool showLedFeedback, bool initializeCameraHttpServices) {
  if (runtimeConfig.wifiList.empty()) {
    Serial.println("[WIFI] No saved STA networks");
    return false;
  }

  WiFi.persistent(false);
  wifiModemSleepEnabled = false;
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);

  for (size_t i = 0; i < runtimeConfig.wifiList.size(); ++i) {
    const WifiCredential &wifi = runtimeConfig.wifiList[i];

    // Hard reset STA state between credential attempts so each SSID starts
    // from a clean state machine and scan context.
    WiFi.disconnect(true, false);
    delay(200);
    WiFi.mode(WIFI_OFF);
    delay(150);
    WiFi.mode(WIFI_STA);
    delay(150);
    WiFi.setSleep(false);

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

    if (showLedFeedback) {
      // LED feedback: double blink when testing WiFi credentials
      ledWifiTestSequence();
    }

    Serial.printf("[WIFI] Trying network %u/%u: %s\n",
      (unsigned int)(i + 1),
      (unsigned int)runtimeConfig.wifiList.size(),
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

      WiFi.setTxPower((wifi_power_t)runtimeConfig.txPowerSta);
      Serial.printf("[WIFI] STA TX power set to %d (raw)\n", (int)runtimeConfig.txPowerSta);
      Serial.printf("[WIFI] Connected to %s — IP: %s\n", wifi.ssid.c_str(), WiFi.localIP().toString().c_str());
      syncClockWithNtp();

      if (initializeCameraHttpServices) {
        registerCameraRoutes();
        server.begin();
        startAuxHttpServers();
        Serial.println("[HTTP] Camera server ready on port 80");
      }

      setWifiModemSleep(true, "idle");
      return true;
    }

    if (showLedFeedback) {
      // LED feedback: long blink on failure
      ledWifiFailureSequence();
    }
    Serial.printf("[WIFI] Failed to connect to %s (status=%d)\n", wifi.ssid.c_str(), (int)result);
  }

  staLinkUp = false;

  // Make sure STA is still armed for the next periodic reconnect cycle.
  if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
    delay(50);
    WiFi.setSleep(false);
  }

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
  Serial.println("[WIFI] STA link lost — attempting reconnect to saved networks");

  if (connectToSavedStaNetworks(false, false)) {
    Serial.println("[WIFI] STA reconnect successful");
  } else {
    Serial.println("[WIFI] STA reconnect failed; will retry");
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

  if (!WiFi.setSleep(enabled)) {
    if (reason && reason[0] != '\0') {
      Serial.printf("[WIFI] Failed to %s modem sleep (%s)\n",
        enabled ? "enable" : "disable",
        reason);
    } else {
      Serial.printf("[WIFI] Failed to %s modem sleep\n",
        enabled ? "enable" : "disable");
    }
    return;
  }

  wifiModemSleepEnabled = enabled;
  if (reason && reason[0] != '\0') {
    Serial.printf("[WIFI] Modem sleep %s (%s)\n",
      enabled ? "enabled" : "disabled",
      reason);
  } else {
    Serial.printf("[WIFI] Modem sleep %s\n",
      enabled ? "enabled" : "disabled");
  }
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

static bool writeAviChunkHeader(File &file, const char *chunkId, uint32_t chunkSize) {
  return writeFourCC(file, chunkId) && writeU32LE(file, chunkSize);
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

static void resetRecordingState() {
  recordingActive = false;
  recordingStartTime = 0;
  recordingDurationMs = 0;
  recordingLastFrameAt = 0;
  recordingFrameCount = 0;
  recordingMaxFrameSize = 0;
  recordingWidth = 0;
  recordingHeight = 0;
  recordingMoviListSize = AVI_MOVI_LIST_HEADER_SIZE;
  recordingPath = "";
  recordingIndex.clear();
}

static bool ensureCaptureDirectory() {
  if (SD_MMC.exists(CAPTURE_DIRECTORY)) {
    return true;
  }
  return SD_MMC.mkdir(CAPTURE_DIRECTORY);
}

static bool beginRecordingFile(const String &path) {
  if (recordingFile) {
    recordingFile.close();
  }

  resetRecordingState();

  recordingFile = SD_MMC.open(path, FILE_WRITE);
  if (!recordingFile) {
    return false;
  }

  uint8_t aviHeader[AVI_HEADER_SIZE] = {0};
  if (recordingFile.write(aviHeader, sizeof(aviHeader)) != sizeof(aviHeader)) {
    recordingFile.close();
    SD_MMC.remove(path);
    return false;
  }

  recordingPath = path;
  recordingActive = true;
  recordingStartTime = millis();
  recordingMoviListSize = AVI_MOVI_LIST_HEADER_SIZE;
  recordingIndex.clear();
  return true;
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

static void appendU16(std::vector<uint8_t> &buf, uint16_t value) {
  buf.push_back((uint8_t)(value & 0xFF));
  buf.push_back((uint8_t)((value >> 8) & 0xFF));
}

static bool readU16(const std::vector<uint8_t> &buf, size_t &offset, uint16_t &out) {
  if (offset + 2 > buf.size()) {
    return false;
  }

  out = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
  offset += 2;
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

  int framesizeResult = 0;
  if (!isValidFrameSizeValue(sensor, cfg.cameraSettings.framesize)) {
    Serial.printf("[CAM] Stored framesize %d is not supported by this sensor\n", cfg.cameraSettings.framesize);
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

static String urlEncode(const String &value) {
  static const char hex[] = "0123456789ABCDEF";
  String escaped;
  escaped.reserve(value.length() * 3);

  for (unsigned int i = 0; i < value.length(); ++i) {
    uint8_t c = (uint8_t)value[i];
    bool safe = (c >= 'A' && c <= 'Z')
             || (c >= 'a' && c <= 'z')
             || (c >= '0' && c <= '9')
             || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      escaped += (char)c;
    } else {
      escaped += '%';
      escaped += hex[(c >> 4) & 0x0F];
      escaped += hex[c & 0x0F];
    }
  }

  return escaped;
}

static void sendHtmlWithToken(String page) {
  page.replace("__ROUTE_TOKEN__", routeAccessToken);
  server.send(200, "text/html", page);
}

static void sendHtmlWithToken(const char *html) {
  sendHtmlWithToken(String(html));
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
  plain.reserve(cfg.adminPass.length() + cfg.deviceName.length() + cfg.wifiList.size() * 32 + 64);

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
  appendU8(plain, cfg.ledAccessBlink ? 1 : 0);
  appendU8(plain, cfg.loggingEnabled ? 1 : 0);
  appendU8(plain, (uint8_t)cfg.txPowerSta);
  appendU8(plain, (uint8_t)cfg.txPowerAp);

  appendU8(plain, cfg.motionSettings.enabled ? 1 : 0);
  appendU8(plain, cfg.motionSettings.captureImage ? 1 : 0);
  appendU8(plain, cfg.motionSettings.imageCount);
  appendU8(plain, cfg.motionSettings.imageDelayDs);
  appendU8(plain, cfg.motionSettings.captureVideo ? 1 : 0);
  appendU8(plain, cfg.motionSettings.videoDurationSec);
  appendU8(plain, cfg.motionSettings.wakeOnMotion ? 1 : 0);
  appendU8(plain, cfg.motionSettings.autoStandby ? 1 : 0);
  appendU16(plain, cfg.motionSettings.standbyAfterSec);
  appendU16(plain, cfg.motionSettings.detectionIntervalSec);

  return encryptPayload(plain, ivHex, cipherHex);
}

static bool decryptConfigV6(const String &ivHex, const String &cipherHex, StoredConfig &cfg) {
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

  uint8_t ledAccessBlink = 0;
  if (!readU8(plain, offset, ledAccessBlink)) {
    cfg.ledAccessBlink = false;
    return false;
  }
  cfg.ledAccessBlink = (ledAccessBlink != 0);

  cfg.loggingEnabled = true;
  if (offset < plain.size()) {
    uint8_t loggingEnabled = 1;
    if (!readU8(plain, offset, loggingEnabled)) {
      return false;
    }
    cfg.loggingEnabled = (loggingEnabled != 0);
  }

  uint8_t txPowerSta = (uint8_t)DEFAULT_TX_POWER_STA;
  if (offset >= plain.size()) {
    cfg.txPowerSta = (int8_t)DEFAULT_TX_POWER_STA;
    cfg.txPowerAp = (int8_t)DEFAULT_TX_POWER_AP;
    clampMotionSettings(cfg.motionSettings);
    return true;
  }
  if (!readU8(plain, offset, txPowerSta)) {
    return false;
  }
  cfg.txPowerSta = (int8_t)txPowerSta;

  uint8_t txPowerAp = (uint8_t)DEFAULT_TX_POWER_AP;
  if (offset >= plain.size()) {
    cfg.txPowerAp = (int8_t)DEFAULT_TX_POWER_AP;
    clampMotionSettings(cfg.motionSettings);
    return true;
  }
  if (!readU8(plain, offset, txPowerAp)) {
    return false;
  }
  cfg.txPowerAp = (int8_t)txPowerAp;

  if (offset < plain.size()) {
    uint8_t b = 0;
    if (!readU8(plain, offset, b)) return false;
    cfg.motionSettings.enabled = (b != 0);

    if (offset < plain.size()) {
      if (!readU8(plain, offset, b)) return false;
      cfg.motionSettings.captureImage = (b != 0);
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, cfg.motionSettings.imageCount)) return false;
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, cfg.motionSettings.imageDelayDs)) return false;
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, b)) return false;
      cfg.motionSettings.captureVideo = (b != 0);
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, cfg.motionSettings.videoDurationSec)) return false;
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, b)) return false;
      cfg.motionSettings.wakeOnMotion = (b != 0);
    }
    if (offset < plain.size()) {
      if (!readU8(plain, offset, b)) return false;
      cfg.motionSettings.autoStandby = (b != 0);
    }
    if (offset + 2 <= plain.size()) {
      if (!readU16(plain, offset, cfg.motionSettings.standbyAfterSec)) return false;
    }
    if (offset + 2 <= plain.size()) {
      if (!readU16(plain, offset, cfg.motionSettings.detectionIntervalSec)) return false;
    }
    // Backward compatibility: older configs may include a trailing sensitivity byte.
    if (offset < plain.size()) {
      uint8_t legacySensitivity = 0;
      if (!readU8(plain, offset, legacySensitivity)) return false;
    }
    // Backward compatibility: older configs may include a trailing PIR input mode byte.
    if (offset < plain.size()) {
      uint8_t legacyPirInputMode = 0;
      if (!readU8(plain, offset, legacyPirInputMode)) return false;
    }
  }

  clampMotionSettings(cfg.motionSettings);
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

  file.println("ESP32CAMCFG6");
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

  if (magic == "ESP32CAMCFG6" || magic == "ESP32CAMCFG5") {
    if (!decryptConfigV6(ivHex, cipherHex, cfg)) {
      Serial.println("[CFG] Failed to decrypt config");
      return false;
    }
  }

  clampMotionSettings(cfg.motionSettings);
  return !cfg.adminPass.isEmpty();
}

static bool persistRuntimeConfig(const StoredConfig &cfg) {
  StoredConfig updated = cfg;
  syncCameraSettingsFromSensor(updated);
  clampMotionSettings(updated.motionSettings);

  if (!saveConfigToSD(updated)) {
    return false;
  }

  runtimeConfig = updated;
  cfgAccessPass = updated.adminPass;
  cfgDeviceName = updated.deviceName;
  isConfigured = !cfgAccessPass.isEmpty();
  return true;
}

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
}

static void applyPirInputMode() {
  pinMode(PIR_GPIO, INPUT_PULLDOWN);
  Serial.printf("[GPIO] PIR mode applied on GPIO%d: INPUT_PULLDOWN\n", PIR_GPIO);
}

static void restoreInputPinsAfterSDInit() {
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  applyPirInputMode();
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
}

static void logSharedPinCaveats() {
  if (PIR_GPIO == 12) {
    Serial.println("[GPIO] Warning: PIR on GPIO12 shares SD DAT2 and the ESP32 strap/pulldown network; weak HIGH outputs may remain LOW with an SD card inserted");
  } else if (PIR_GPIO == 13) {
    Serial.println("[GPIO] Warning: PIR on GPIO13 shares SD DAT3 and its pull-up network; the line can read HIGH or edge on card insert/remove");
  }

  if (BUTTON_GPIO == 12) {
    Serial.println("[GPIO] Note: button on GPIO12 shares SD DAT2; a strong switch to GND usually works, but the line is not isolated from the SD socket");
  } else if (BUTTON_GPIO == 13) {
    Serial.println("[GPIO] Note: button on GPIO13 shares SD DAT3; a strong switch to GND usually works, but the line is not isolated from the SD socket");
  }
}

static void updateSdLoggingState() {
  gLogSdMirrorEnabled = gLoggingEnabled;
}

static bool pirSupportsRtcWakeup() {
  return rtc_gpio_is_valid_gpio((gpio_num_t)PIR_GPIO);
}

static void IRAM_ATTR onPirEdgeInterrupt() {
  motionEdgePending = true;
  ++motionEdgeCount;
}

// ─── HTML pages (stored in flash) ─────────────────────────────────────────────

// Navigation bar HTML (reused across pages)
static const char NAV_HTML[] PROGMEM = R"html(
<nav style="background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap">
  <a href="/" style="color:#e94560;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573">📷 Camera</a>
  <a href="/motion" style="color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573">🚶 Motion</a>
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
  <a href="/motion">🚶 Motion</a>
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
      <select id="framesize">__FRAME_SIZE_OPTIONS__</select>
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
var streamUrl='http://'+window.location.hostname+':81/stream?t='+encodeURIComponent('__ROUTE_TOKEN__');
function id(n){return document.getElementById(n);}
function chk(el){return el.checked?1:0;}
function ctrl(v,val,persist){
  var url='/control?var='+encodeURIComponent(v)+'&val='+encodeURIComponent(val);
  if(persist===false){url+='&persist=0';}
  else{url+='&persist=1';}
  return fetch(url).then(function(r){
    return r.text().then(function(text){
      return {ok:r.ok,text:text||''};
    });
  });
}
function closeStreamConnection(){
  if(!navigator.sendBeacon){
    fetch('/stream/close',{method:'POST',keepalive:true}).catch(function(){});
    return;
  }
  navigator.sendBeacon('/stream/close',new Blob(['close'],{type:'text/plain'}));
}
function releaseStream(notifyServer){
  var img=id('stream');
  if(img.src){
    img.dataset.src=img.dataset.src||img.src;
    img.removeAttribute('src');
  }
  if(notifyServer){closeStreamConnection();}
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
    if(!img.dataset.src){img.dataset.src=streamUrl;}
    if(img.src!==img.dataset.src){img.src=img.dataset.src;}
  }else{
    releaseStream(true);
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
function bindFrameSizeControl(){
  var el=id('framesize');
  if(!el)return;
  el.addEventListener('change',function(){
    var shouldResumeStream=streamVisible;
    setRecordingState(recordingMode,'Applying resolution change...');
    if(shouldResumeStream){releaseStream(true);}
    ctrl('framesize',el.value,true).then(function(result){
      return loadStatus().catch(function(){}).then(function(){
        if(!result.ok){setRecordingState(recordingMode,result.text||'Failed to change resolution');}
        else if(recordingMode){setRecordingState(true,'Recording...');}
        else{id('rec_status').textContent='';}
        if(shouldResumeStream){setTimeout(function(){setStreamVisibility(true);},150);}
      });
    }).catch(function(){
      if(shouldResumeStream){setStreamVisibility(true);}
      setRecordingState(recordingMode,'Failed to change resolution');
    });
  });
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
function applyStatus(s){
  ['framesize','brightness','contrast','saturation','quality','special_effect','wb_mode'].forEach(function(k){
    if(s[k]!==undefined){var e=id(k);if(e)e.value=s[k];var v=id(k+'_v');if(v)v.innerText=s[k];}
  });
  ['awb','aec','hmirror','vflip','lenc'].forEach(function(k){if(s[k]!==undefined){var e=id(k);if(e)e.checked=!!s[k];}});
  setStreamVisibility(s.stream_visible!==undefined?!!s.stream_visible:true);
  if(s.recording_active!==undefined){setRecordingState(!!s.recording_active,s.recording_active?'Recording...':'');}
}
function loadStatus(){
  return fetch('/status').then(function(r){return r.json();}).then(function(s){
    applyStatus(s);
    return s;
  });
}
bindFrameSizeControl();
['special_effect','wb_mode'].forEach(bindSelectControl);
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
id('stream').dataset.src=streamUrl;
id('ip_label').innerText=h;
window.addEventListener('pagehide',function(){releaseStream(true);});
window.addEventListener('beforeunload',function(){releaseStream(true);});
setStreamVisibility(false);
loadStatus().catch(function(){
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
  <a href="/motion">🚶 Motion</a>
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
  <div class="panel">
    <h3>LED Control</h3>
    <form class="form" id="led_form">
      <div style="display:flex;align-items:center;gap:10px">
        <label for="led_access_blink" style="margin:0">Blink on URL access</label>
        <input id="led_access_blink" type="checkbox" style="width:auto">
      </div>
      <button type="submit">Save</button>
    </form>
    <div id="led_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">When enabled, LED blinks briefly on each URL request. Boot sequences are unaffected.</div>
  </div>
  <div class="panel">
    <h3>Logging</h3>
    <form class="form" id="logging_form">
      <div style="display:flex;align-items:center;gap:10px">
        <label for="logging_enabled" style="margin:0">Enable serial + file logging</label>
        <input id="logging_enabled" type="checkbox" style="width:auto">
      </div>
      <button type="submit">Save</button>
    </form>
    <div id="logging_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Disables all firmware logs globally, including serial output and /log.txt writes.</div>
  </div>
  <div class="panel">
    <h3>TX Power</h3>
    <form class="form" id="txpower_form">
      <div>
        <label>STA Mode (dBm)</label>
        <select id="txpower_sta">
          <option value="-4">-1 dBm</option>
          <option value="8">2 dBm</option>
          <option value="20">5 dBm</option>
          <option value="28">7 dBm</option>
          <option value="34">8.5 dBm</option>
          <option value="44">11 dBm</option>
          <option value="52">13 dBm</option>
          <option value="60">15 dBm</option>
          <option value="68">17 dBm</option>
          <option value="72">18 dBm</option>
          <option value="76">19 dBm</option>
          <option value="78">19.5 dBm (max)</option>
        </select>
      </div>
      <div>
        <label>AP Fallback Mode (dBm)</label>
        <select id="txpower_ap">
          <option value="-4">-1 dBm</option>
          <option value="8">2 dBm</option>
          <option value="20">5 dBm</option>
          <option value="28">7 dBm</option>
          <option value="34">8.5 dBm</option>
          <option value="44">11 dBm</option>
          <option value="52">13 dBm</option>
          <option value="60">15 dBm</option>
          <option value="68">17 dBm</option>
          <option value="72">18 dBm</option>
          <option value="76">19 dBm</option>
          <option value="78">19.5 dBm (max)</option>
        </select>
      </div>
      <button type="submit">Save</button>
    </form>
    <div id="txpower_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Lower TX power reduces consumption. AP fallback clients are nearby so 8.5 dBm is a reasonable default. Changes apply immediately.</div>
  </div>
  <div class="panel">
    <h3>Firmware Update</h3>
    <div class="status" style="color:#bbb;margin-bottom:10px">Current version: <strong style="color:#eee">__FIRMWARE_VERSION__</strong><br>Build: __FIRMWARE_BUILD__</div>
    <form class="form" id="firmware_form">
      <div>
        <label>Firmware Binary (.bin)</label>
        <input id="firmware_file" type="file" accept=".bin,application/octet-stream" required>
      </div>
      <button type="submit">Upload Firmware</button>
    </form>
    <div id="firmware_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Upload the compiled firmware binary. The device will reboot automatically after a successful update.</div>
  </div>
  <div class="panel">
    <h3>System Reset</h3>
    <form class="form" id="reset_form">
      <button type="submit">Restart Device</button>
    </form>
    <div id="reset_status" class="status"></div>
    <div style="font-size:.85em;color:#bbb;margin-top:10px">Restarts the ESP32-CAM without changing saved settings.</div>
    <form class="form" id="factory_reset_form" style="margin-top:10px">
      <button type="submit">Factory Reset (Delete Config)</button>
    </form>
    <div id="factory_reset_status" class="status"></div>
  </div>
</div>
<script>
function id(n){return document.getElementById(n);}
var transferBase='http://'+window.location.hostname+':82';
var transferToken=encodeURIComponent('__ROUTE_TOKEN__');
function setWiFiStatus(msg,err){var e=id('wifi_status');e.textContent=msg;e.className=err?'status error':'status';}
function setAdminStatus(msg,err){var e=id('admin_status');e.textContent=msg;e.className=err?'status error':'status';}
function setNameStatus(msg,err){var e=id('name_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTimeStatus(msg,err){var e=id('time_status');e.textContent=msg;e.className=err?'status error':'status';}
function setLedStatus(msg,err){var e=id('led_status');e.textContent=msg;e.className=err?'status error':'status';}
function setLoggingStatus(msg,err){var e=id('logging_status');e.textContent=msg;e.className=err?'status error':'status';}
function setTxPowerStatus(msg,err){var e=id('txpower_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFirmwareStatus(msg,err){var e=id('firmware_status');e.textContent=msg;e.className=err?'status error':'status';}
function setResetStatus(msg,err){var e=id('reset_status');e.textContent=msg;e.className=err?'status error':'status';}
function setFactoryResetStatus(msg,err){var e=id('factory_reset_status');e.textContent=msg;e.className=err?'status error':'status';}
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
function refreshLedStatus(){
  fetch('/admin/led').then(function(r){
    if(!r.ok){throw new Error('Failed to load LED settings');}
    return r.json();
  }).then(function(d){
    id('led_access_blink').checked=d.ledAccessBlink||false;
  }).catch(function(){});
}
id('led_form').addEventListener('submit',function(e){
  e.preventDefault();
  fetch('/admin/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({ledAccessBlink:id('led_access_blink').checked?'1':'0'})}).then(function(r){r.text().then(function(msg){setLedStatus(msg||'Saved',!r.ok);refreshLedStatus();});});
});
function refreshLoggingStatus(){
  fetch('/admin/logging').then(function(r){
    if(!r.ok){throw new Error('Failed to load logging settings');}
    return r.json();
  }).then(function(d){
    id('logging_enabled').checked=d.loggingEnabled!==false;
  }).catch(function(){});
}
id('logging_form').addEventListener('submit',function(e){
  e.preventDefault();
  fetch('/admin/logging',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({loggingEnabled:id('logging_enabled').checked?'1':'0'})}).then(function(r){r.text().then(function(msg){setLoggingStatus(msg||'Saved',!r.ok);refreshLoggingStatus();});});
});
function refreshTxPower(){
  fetch('/admin/txpower').then(function(r){
    if(!r.ok){throw new Error('Failed to load TX power settings');}
    return r.json();
  }).then(function(d){
    var sSel=id('txpower_sta');var aSel=id('txpower_ap');
    for(var i=0;i<sSel.options.length;i++){if(parseInt(sSel.options[i].value)===d.txPowerSta){sSel.selectedIndex=i;break;}}
    for(var i=0;i<aSel.options.length;i++){if(parseInt(aSel.options[i].value)===d.txPowerAp){aSel.selectedIndex=i;break;}}
  }).catch(function(){});
}
id('txpower_form').addEventListener('submit',function(e){
  e.preventDefault();
  fetch('/admin/txpower',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:formData({txPowerSta:id('txpower_sta').value,txPowerAp:id('txpower_ap').value})}).then(function(r){r.text().then(function(msg){setTxPowerStatus(msg||'Saved',!r.ok);});});
});
id('firmware_form').addEventListener('submit',function(e){
  e.preventDefault();
  var input=id('firmware_file');
  if(!input.files.length){setFirmwareStatus('Choose a firmware .bin file first',true);return;}
  var file=input.files[0];
  setFirmwareStatus('Uploading '+file.name+'...',false);
  var fd=new FormData();
  fd.append('firmware',file);
  fetch(transferBase+'/admin/update?t='+transferToken,{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(msg){
      setFirmwareStatus(msg||'Firmware upload finished',!r.ok);
      if(r.ok){input.value='';}
    });
  }).catch(function(err){setFirmwareStatus(err.message||'Firmware upload failed',true);});
});
id('reset_form').addEventListener('submit',function(e){
  e.preventDefault();
  if(!confirm('Restart device now?')){return;}
  setResetStatus('Scheduling restart...',false);
  fetch('/admin/reset',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setResetStatus(msg||'Restart requested',!r.ok);
    });
  }).catch(function(err){setResetStatus(err.message||'Restart failed',true);});
});
id('factory_reset_form').addEventListener('submit',function(e){
  e.preventDefault();
  if(!confirm('Delete stored configuration and restart to setup mode?')){return;}
  setFactoryResetStatus('Deleting configuration and scheduling restart...',false);
  fetch('/admin/factory-reset',{method:'POST'}).then(function(r){
    return r.text().then(function(msg){
      setFactoryResetStatus(msg||'Factory reset requested',!r.ok);
    });
  }).catch(function(err){setFactoryResetStatus(err.message||'Factory reset failed',true);});
});
refreshWiFiList();
refreshDeviceName();
refreshTimeStatus();
refreshLedStatus();
refreshLoggingStatus();
refreshTxPower();
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
.folder-create{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.folder-create input{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px;min-width:200px}
.folder-create button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.folder-create button:hover{background:#234573}
.dropzone{border:2px dashed #234573;border-radius:8px;padding:14px 12px;margin-bottom:10px;text-align:center;color:#9ec5ff;background:#111c38;transition:background .15s,border-color .15s,color .15s}
.dropzone strong{color:#dbeafe}
.dropzone.active{border-color:#7dd3fc;background:#0d2f47;color:#dbeafe}
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
  <a href="/motion">🚶 Motion</a>
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
  <div class="folder-create">
    <input id="new_folder_name" type="text" placeholder="New folder name" maxlength="64">
    <button onclick="createFolder()">Create Folder</button>
  </div>
  <div id="dropzone" class="dropzone"><strong>Drag and drop files here</strong> to upload into the current folder</div>
  <div class="pathbar">
    <button onclick="goUp()">Up</button>
    <div id="crumbs" class="crumbs"></div>
  </div>
  <div class="sortbar">
    <label for="sort_by">Sort by</label>
    <select id="sort_by" onchange="onSortChanged()">
      <option value="name">Name</option>
      <option value="size">Size</option>
      <option value="type">Type</option>
    </select>
    <label for="sort_dir">Direction</label>
    <select id="sort_dir" onchange="onSortChanged()">
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
var transferBase='http://'+window.location.hostname+':82';
var transferToken=encodeURIComponent('__ROUTE_TOKEN__');
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
function validFolderName(name){
  var n=String(name||'').trim();
  if(!n||n==='.'||n==='..')return false;
  if(n.indexOf('/')!==-1||n.indexOf('\\')!==-1||n.indexOf('..')!==-1)return false;
  return true;
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
        +'<div class="file-left" ondblclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')"><div class="thumb"></div><div class="meta"><strong>'+esc(item.name)+'</strong><span>Folder • '+esc(item.path)+'</span></div></div>'
        +'<div class="file-actions"><button onclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Open</button><button onclick="deleteFolder(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
        +'</div>';
    }

    var preview=isImage(item.path)
      ?'<img class="thumb" loading="lazy" src="'+transferBase+'/sd/view?file='+encodeURIComponent(item.path)+'&t='+transferToken+'" alt="preview">'
      :'<div class="thumb"></div>';
    return '<div class="file-item">'
      +'<div class="file-left" ondblclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">'+preview+'<div class="meta"><strong>'+esc(item.name)+'</strong><span>'+formatSize(item.size)+' • '+esc(item.path)+'</span></div></div>'
      +'<div class="file-actions"><a href="'+transferBase+'/sd/download?file='+encodeURIComponent(item.path)+'&t='+transferToken+'">Download</a><a href="#" onclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">Open</a><button onclick="deleteFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
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
function openFile(path){
  var p=String(path||'').toLowerCase();
  var isVideo=p.endsWith('.avi')||p.endsWith('.mp4')||p.endsWith('.mjpg')||p.endsWith('.mov')||p.endsWith('.webm');
  if(isVideo){
    window.location.assign('/sd/player?file='+encodeURIComponent(path));
    return;
  }
  window.location.assign(transferBase+'/sd/view?file='+encodeURIComponent(path)+'&t='+transferToken);
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
function createFolder(){
  var input=document.getElementById('new_folder_name');
  var name=String(input.value||'').trim();
  if(!validFolderName(name)){
    setStatus('Invalid folder name',true);
    return;
  }
  setStatus('Creating folder '+name+'...',false);
  var body='dir='+encodeURIComponent(currentDir)+'&name='+encodeURIComponent(name);
  fetch('/sd/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to create folder');}
      setStatus(t||'Folder created',false);
      input.value='';
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to create folder',true);});
}
function deleteFolder(path){
  if(path==='/'||!path){
    setStatus('Cannot delete root folder',true);
    return;
  }
  if(!confirm('Delete folder '+path+' and all contents?'))return;
  setStatus('Deleting folder '+path+'...',false);
  fetch('/sd/rmdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'dir='+encodeURIComponent(path)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to delete folder');}
      setStatus(t||'Folder deleted',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to delete folder',true);});
}
function uploadFileObject(file){
  if(!file)return;
  setStatus('Uploading '+file.name+'...',false);
  var fd=new FormData();fd.append('file',file);
  fetch(transferBase+'/sd/upload?t='+transferToken+'&dir='+encodeURIComponent(currentDir),{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Upload failed');}
      setStatus(t||'Upload complete',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Upload failed',true);});
}
function uploadFile(input){
  if(!input.files.length)return;
  uploadFileObject(input.files[0]);
  input.value='';
}
function isValidSortBy(v){
  return v==='name'||v==='size'||v==='type';
}
function isValidSortDir(v){
  return v==='asc'||v==='desc';
}
function saveSortPreference(){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value;
  fetch('/sd/sort',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'by='+encodeURIComponent(by)+'&dir='+encodeURIComponent(dir)}).catch(function(){});
}
function loadSortPreference(){
  return fetch('/sd/sort').then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load sort preference');}
      return d;
    });
  }).then(function(d){
    document.getElementById('sort_by').value=isValidSortBy(d.by)?d.by:'name';
    document.getElementById('sort_dir').value=isValidSortDir(d.dir)?d.dir:'asc';
  }).catch(function(){
    document.getElementById('sort_by').value='name';
    document.getElementById('sort_dir').value='asc';
  });
}
function onSortChanged(){
  applySortAndRender();
  saveSortPreference();
}
function setupDropzone(){
  var zone=document.getElementById('dropzone');
  if(!zone)return;
  function over(e){
    e.preventDefault();
    zone.classList.add('active');
  }
  function leave(e){
    e.preventDefault();
    zone.classList.remove('active');
  }
  zone.addEventListener('dragenter',over);
  zone.addEventListener('dragover',over);
  zone.addEventListener('dragleave',leave);
  zone.addEventListener('drop',function(e){
    e.preventDefault();
    zone.classList.remove('active');
    var files=e.dataTransfer&&e.dataTransfer.files;
    if(!files||!files.length){
      setStatus('No file dropped',true);
      return;
    }
    uploadFileObject(files[0]);
  });
}
setupDropzone();
loadSortPreference().then(function(){loadFiles();});
</script>
</body>
</html>)html";

// ─── Camera initialisation ────────────────────────────────────────────────────
static bool initCamera(uint32_t xclkFreqHz) {
  // Keep camera powered up and give the sensor time to stabilize.
    if (PWDN_GPIO_NUM >= 0) {
        pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(300);
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
    config.xclk_freq_hz  = xclkFreqHz;
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

    sensor_t *s = esp_camera_sensor_get();
    if (s && s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
    }

    applyStoredCameraSettings(runtimeConfig);

    // Initialize LED flash pin
    pinMode(LED_FLASH_GPIO_NUM, OUTPUT);
    digitalWrite(LED_FLASH_GPIO_NUM, LOW);
    flashEnabled = false;

    return true;
}

static bool initCameraWithRetries() {
    const size_t xclkCount = sizeof(CAMERA_XCLK_FREQS_HZ) / sizeof(CAMERA_XCLK_FREQS_HZ[0]);

    for (int attempt = 1; attempt <= CAMERA_INIT_RETRIES; ++attempt) {
      uint32_t xclkHz = CAMERA_XCLK_FREQS_HZ[(size_t)(attempt - 1) % xclkCount];

        Serial.printf("[CAM] Init attempt %d/%d using XCLK=%lu Hz\n",
          attempt,
          CAMERA_INIT_RETRIES,
          (unsigned long)xclkHz);

        if (initCamera(xclkHz)) {
            if (attempt > 1) {
                Serial.printf("[CAM] Init succeeded on attempt %d (XCLK=%lu Hz)\n",
                  attempt,
                  (unsigned long)xclkHz);
            }
            return true;
        }

        esp_camera_deinit();
        Serial.printf("[CAM] Retry %d/%d\n", attempt, CAMERA_INIT_RETRIES);
        delay(CAMERA_INIT_RETRY_DELAY_MS);
    }

    return false;
}

static bool loadRuntimeConfigWithRetries(StoredConfig &cfg) {
  for (int attempt = 1; attempt <= CONFIG_LOAD_RETRIES; ++attempt) {
    if (loadConfigFromSD(cfg)) {
      if (attempt > 1) {
        Serial.printf("[CFG] Loaded config on attempt %d\n", attempt);
      }
      return true;
    }

    if (attempt < CONFIG_LOAD_RETRIES) {
      Serial.printf("[CFG] Load attempt %d/%d failed, retrying in %lu ms\n",
        attempt,
        CONFIG_LOAD_RETRIES,
        (unsigned long)CONFIG_LOAD_RETRY_DELAY_MS);
      delay(CONFIG_LOAD_RETRY_DELAY_MS);
    }
  }

  return false;
}



static bool startSoftAPWithRetries(const char *ssid, const char *password) {
  for (int attempt = 1; attempt <= AP_START_RETRIES; ++attempt) {
    WiFi.mode(WIFI_AP);
    if (WiFi.softAP(ssid, password, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS)) {
      if (attempt > 1) {
        Serial.printf("[WIFI] AP start succeeded on attempt %d\n", attempt);
      }
      return true;
    }

    Serial.printf("[WIFI] AP start attempt %d/%d failed\n", attempt, AP_START_RETRIES);
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
static void handleSDDownloadMain();
static void handleSDDownloadWorker();
static void handleSDViewMain();
static void handleSDViewWorker();
static void handleSDUploadMain();
static void handleSDUploadWorker();
static void handleSDUploadDataWorker();
static void handleFirmwareUploadMain();
static void handleFirmwareUploadWorker();
static void handleFirmwareUploadDataWorker();
static void streamServerTask(void *arg);
static void transferServerTask(void *arg);
static void startAuxHttpServers();

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

static void handleStreamWorker() {
    if (!checkAuth(streamServer, true)) return;

    if (!ensureCameraReady()) {
      streamServer.send(503, "text/plain", "Camera unavailable");
      return;
    }

    setWifiModemSleep(false, "active stream");

    WiFiClient client = streamServer.client();
    Serial.println("[STREAM] Client connected");
    streamClientAbortRequested = false;
    streamClientConnected = true;
    unsigned long lastFrameAt = 0;

    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--jpgbound\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n"
    );

    while (client.connected()) {
        if (streamClientAbortRequested) {
            break;
        }

        unsigned long now = millis();
        if (lastFrameAt != 0) {
            unsigned long elapsed = now - lastFrameAt;
            if (elapsed < STREAM_FRAME_INTERVAL_MS) {
                delay(STREAM_FRAME_INTERVAL_MS - elapsed);
                continue;
            }
        }

        camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
        if (!fb) {
            delay(10);
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            unlockCameraFrame(fb);
            delay(10);
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

    streamClientAbortRequested = false;
    streamClientConnected = false;
    client.stop();
    setWifiModemSleep(true, "idle");
    Serial.println("[STREAM] Client disconnected");
}

static void handleStreamMain() {
  if (!checkAuth(server)) {
    return;
  }

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Location", buildLocalUrl(HTTP_STREAM_PORT, "/stream", true));
  server.send(302, "text/plain", "Redirecting to stream server");
}

static void handleStreamClose() {
  if (!checkAuth(server)) {
    return;
  }

  if (streamClientConnected) {
    streamClientAbortRequested = true;
    Serial.println("[STREAM] Close requested by UI");
  }

  server.send(204, "text/plain", "");
}

// ─── Authentication helper ────────────────────────────────────────────────────
static bool checkAuth() {
    return checkAuth(server);
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
    server.send_P(200, "text/html", SETUP_HTML);
}

static void handleSave() {
    handleUrlAccess();
    
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
    framesize_t selected = FRAMESIZE_VGA;
    if (runtimeConfig.hasCameraSettings && runtimeConfig.cameraSettings.framesize >= 0) {
      selected = (framesize_t)runtimeConfig.cameraSettings.framesize;
    }

    String page(MAIN_HTML);
    page.replace("__FRAME_SIZE_OPTIONS__", buildFrameSizeOptionsHtml(selected));
    sendHtmlWithToken(page);
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

  if (!ensureCameraReady()) {
    server.send(503, "text/plain", "Camera unavailable");
    return;
  }

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

      if (!ensureCameraReady()) {
        server.send(503, "text/plain", "Camera unavailable");
        return;
      }

    if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        server.send(503, "text/plain", "Camera busy");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!cameraInitialized || !s) {
        xSemaphoreGive(cameraMutex);
        server.send(503, "text/plain", "Camera sensor not available");
        return;
    }

    int res = 0;
    int statusCode = 200;
    const char *message = "OK";
    if (varName == "framesize") {
        if (recordingActive) {
            statusCode = 409;
            message = "Stop recording before changing resolution";
        } else if (!isValidFrameSizeValue(s, val)) {
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
        xSemaphoreGive(cameraMutex);
        server.send(400, "text/plain", "Unknown variable");
        return;
    }

    if (statusCode == 200 && res == 0) {
      lastCameraActivityAt = millis();
    }

    xSemaphoreGive(cameraMutex);

    if (statusCode != 200) {
      server.send(statusCode, "text/plain", message);
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

  if (!ensureCameraReady()) {
    server.send(503, "text/plain", "Camera unavailable");
    return;
  }

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
  SD_MMC.remove(SD_SORT_FILE_PATH);
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

static void handleAdminLedGet() {
  if (!checkAuth()) return;

  String json = "{\"ledAccessBlink\":" + String(ledAccessBlinkEnabled ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

static void handleAdminLedSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("ledAccessBlink")) {
    server.send(400, "text/plain", "Missing ledAccessBlink parameter");
    return;
  }

  String value = server.arg("ledAccessBlink");
  bool newValue = (value == "1" || value == "true");

  // Update runtime config and save to SD
  runtimeConfig.ledAccessBlink = newValue;
  ledAccessBlinkEnabled = newValue;

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(500, "text/plain", "Failed to save LED configuration");
    return;
  }

  server.send(200, "text/plain", "LED configuration saved");
}

static void handleAdminLoggingGet() {
  if (!checkAuth()) return;

  String json = "{\"loggingEnabled\":" + String(gLoggingEnabled ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

static void handleAdminLoggingSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("loggingEnabled")) {
    server.send(400, "text/plain", "Missing loggingEnabled parameter");
    return;
  }

  bool newValue = (server.arg("loggingEnabled") == "1" || server.arg("loggingEnabled") == "true");

  runtimeConfig.loggingEnabled = newValue;
  gLoggingEnabled = newValue;
  updateSdLoggingState();

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(500, "text/plain", "Failed to save logging configuration");
    return;
  }

  server.send(200, "text/plain", "Logging configuration saved");
}

// Valid wifi_power_t raw values accepted from the UI
static bool isValidTxPowerValue(int v) {
  switch (v) {
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

static void handleAdminTxPowerGet() {
  if (!checkAuth()) return;

  String json = "{\"txPowerSta\":" + String((int)runtimeConfig.txPowerSta) +
                ",\"txPowerAp\":"  + String((int)runtimeConfig.txPowerAp) + "}";
  server.send(200, "application/json", json);
}

static void handleAdminTxPowerSet() {
  if (!checkAuth()) return;

  if (!server.hasArg("txPowerSta") || !server.hasArg("txPowerAp")) {
    server.send(400, "text/plain", "Missing txPowerSta or txPowerAp");
    return;
  }

  int newSta = server.arg("txPowerSta").toInt();
  int newAp  = server.arg("txPowerAp").toInt();

  if (!isValidTxPowerValue(newSta) || !isValidTxPowerValue(newAp)) {
    server.send(400, "text/plain", "Invalid TX power value");
    return;
  }

  runtimeConfig.txPowerSta = (int8_t)newSta;
  runtimeConfig.txPowerAp  = (int8_t)newAp;

  // Apply immediately to the active interface
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_STA || mode == WIFI_AP_STA) {
    WiFi.setTxPower((wifi_power_t)newSta);
  } else if (mode == WIFI_AP) {
    WiFi.setTxPower((wifi_power_t)newAp);
  }

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(500, "text/plain", "Failed to save TX power configuration");
    return;
  }

  server.send(200, "text/plain", "TX power saved");
}

static void handleAdminReset() {
  if (!checkAuth()) {
    return;
  }

  handleUrlAccess();
  adminRestartPending = true;
  adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
  server.send(200, "text/plain", "Restart requested. Device will reboot shortly.");
}

static void handleAdminFactoryReset() {
  if (!checkAuth()) {
    return;
  }

  handleUrlAccess();

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  if (SD_MMC.exists(CONFIG_FILE_PATH) && !SD_MMC.remove(CONFIG_FILE_PATH)) {
    server.send(500, "text/plain", "Failed to delete configuration file");
    return;
  }

  adminRestartPending = true;
  adminRestartAt = millis() + FIRMWARE_RESTART_DELAY_MS;
  server.send(200, "text/plain", "Configuration deleted. Rebooting to setup mode shortly.");
}

static void handleAdminPage() {
  if (!checkAuth()) return;
  String page(ADMIN_HTML);
  page.replace("__FIRMWARE_VERSION__", FIRMWARE_VERSION_TEXT);
  page.replace("__FIRMWARE_BUILD__", FIRMWARE_BUILD_TEXT);
  sendHtmlWithToken(page);
}

static void handleMotionPage() {
  if (!checkAuth()) return;

  static const char MOTION_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Motion</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px}
header h1{color:#e94560;font-size:1.3em}
.wrap{max-width:760px;margin:0 auto;padding:14px}
.panel{background:#16213e;border-radius:8px;padding:14px}
h3{color:#7dd3fc;margin-bottom:10px}
.cg{margin-bottom:12px}
.cg label{display:block;font-size:.9em;color:#bbb;margin-bottom:6px}
input[type=range],select{width:100%}
select,input{background:#0f3460;color:#eee;border:1px solid #234573;border-radius:4px;padding:8px}
.row{display:grid;grid-template-columns:1fr 120px;gap:10px;align-items:center}
.small{font-size:.82em;color:#9fb3d1;margin-top:4px}
.status{min-height:20px;margin-top:10px;color:#7dd3fc}
.status.error{color:#ff8a8a}
.btn{background:#e94560;color:#fff;border:none;border-radius:4px;padding:9px 14px;cursor:pointer}
.btn:hover{background:#c73652}
</style>
</head>
<body>
<nav>
  <a href="/">📷 Camera</a>
  <a href="/motion" style="color:#e94560">🚶 Motion</a>
  <a href="/sd">💾 SD Browser</a>
  <a href="/admin">⚙️ Admin</a>
</nav>
<header><h1>🚶 Motion Detection</h1></header>
<div class="wrap">
  <div class="panel">
    <h3>Settings</h3>
    <div class="cg"><a href="/motion/graph" style="color:#7dd3fc;text-decoration:none">Open Motion Graph</a></div>
    <div class="cg"><label><input id="enabled" type="checkbox"> Enable motion detection</label></div>
    <div class="cg"><label><input id="wake_on_motion" type="checkbox"> Wake up on motion</label></div>
    <div class="cg"><label><input id="auto_standby" type="checkbox"> Automatic stand-by</label></div>

    <div class="cg row">
      <label for="standby_after_sec">No activity before stand-by (seconds)</label>
      <input id="standby_after_sec" type="number" min="5" max="120" step="1">
    </div>

    <div class="cg"><label><input id="capture_image" type="checkbox"> Capture image(s) on motion</label></div>
    <div class="cg row">
      <label for="image_count">Number of images (1-10)</label>
      <input id="image_count" type="number" min="1" max="10" step="1">
    </div>
    <div class="cg row">
      <label for="image_delay_ds">Delay between images (0.1-2.0 sec)</label>
      <input id="image_delay_ds" type="number" min="0.1" max="2.0" step="0.1">
    </div>

    <div class="cg"><label><input id="capture_video" type="checkbox"> Capture video on motion</label></div>
    <div class="cg row">
      <label for="video_duration_sec">Video duration (1-30 sec)</label>
      <input id="video_duration_sec" type="number" min="1" max="30" step="1">
    </div>

    <div class="cg row">
      <label for="detection_interval_sec">Interval between detections</label>
      <select id="detection_interval_sec">
        <option value="0">Soon after capture</option>
        <option value="5">+5 seconds</option>
        <option value="10">+10 seconds</option>
        <option value="30">+30 seconds</option>
        <option value="60">+1 minute</option>
        <option value="600">+10 minutes</option>
      </select>
    </div>

    <button class="btn" id="save_btn">Save Motion Settings</button>
    <div class="status" id="status"></div>
  </div>
</div>
<script>
function id(n){return document.getElementById(n);}
function setStatus(msg,err){var e=id('status');e.textContent=msg||'';e.className=err?'status error':'status';}
function asInt(v,d){var n=parseInt(v,10);return isNaN(n)?d:n;}
function loadConfig(){
  fetch('/motion/config').then(function(r){
    if(!r.ok){throw new Error('Failed to load motion config');}
    return r.json();
  }).then(function(c){
    id('enabled').checked=!!c.enabled;
    id('wake_on_motion').checked=!!c.wakeOnMotion;
    id('auto_standby').checked=!!c.autoStandby;
    id('standby_after_sec').value=c.standbyAfterSec;
    id('capture_image').checked=!!c.captureImage;
    id('image_count').value=c.imageCount;
    id('image_delay_ds').value=((c.imageDelayDs||1)/10).toFixed(1);
    id('capture_video').checked=!!c.captureVideo;
    id('video_duration_sec').value=c.videoDurationSec;
    id('detection_interval_sec').value=String(c.detectionIntervalSec||0);
  }).catch(function(e){setStatus(e.message,true);});
}
id('save_btn').addEventListener('click',function(){
  var payload={
    enabled:id('enabled').checked?1:0,
    wakeOnMotion:id('wake_on_motion').checked?1:0,
    autoStandby:id('auto_standby').checked?1:0,
    standbyAfterSec:asInt(id('standby_after_sec').value,30),
    captureImage:id('capture_image').checked?1:0,
    imageCount:asInt(id('image_count').value,1),
    imageDelayDs:Math.round((parseFloat(id('image_delay_ds').value)||0.1)*10),
    captureVideo:id('capture_video').checked?1:0,
    videoDurationSec:asInt(id('video_duration_sec').value,5),
    detectionIntervalSec:asInt(id('detection_interval_sec').value,0)
  };
  setStatus('Saving...',false);
  fetch('/motion/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:Object.keys(payload).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(payload[k]);}).join('&')})
    .then(function(r){return r.text().then(function(t){setStatus(t||'Saved',!r.ok);if(r.ok){loadConfig();}});})
    .catch(function(e){setStatus(e.message,true);});
});
loadConfig();
</script>
</body>
</html>)html";

  sendHtmlWithToken(MOTION_HTML);
}

static void handleMotionGraphPage() {
  if (!checkAuth()) return;

  static const char MOTION_GRAPH_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM - Motion Graph</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px}
header h1{color:#e94560;font-size:1.2em}
.wrap{max-width:920px;margin:0 auto;padding:14px}
.panel{background:#16213e;border-radius:8px;padding:14px}
#graph{width:100%;height:260px;border:1px solid #234573;border-radius:6px;background:#0e1b3a}
.meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px;margin-top:10px;font-size:.88em;color:#b9c7dd}
.status{min-height:20px;margin-top:8px;color:#7dd3fc;font-size:.9em}
.status.error{color:#ff8a8a}
</style>
</head>
<body>
<nav>
  <a href="/">📷 Camera</a>
  <a href="/motion" style="color:#e94560">🚶 Motion</a>
  <a href="/sd">💾 SD Browser</a>
  <a href="/admin">⚙️ Admin</a>
</nav>
<header><h1>🚶 Motion Graph (Live PIR Readings)</h1></header>
<div class="wrap">
  <div class="panel">
    <canvas id="graph"></canvas>
    <div class="meta">
      <div>Raw: <span id="raw">-</span></div>
      <div>Latched: <span id="latched">-</span></div>
      <div>Edge Count: <span id="edgecount">-</span></div>
      <div>Signal: <span id="signal">-</span></div>
      <div>High(ms): <span id="highms">-</span></div>
      <div>Last Trigger Ago(ms): <span id="lastms">-</span></div>
    </div>
    <div class="status" id="status"></div>
  </div>
</div>
<script>
var points=[];
var maxPoints=180;
var lastFetchOk=true;
var lastEdgeCount=0;
var edgePulseFrames=0;
function id(n){return document.getElementById(n);} 
function setStatus(msg,err){var e=id('status');e.textContent=msg||'';e.className=err?'status error':'status';}
function draw(){
  var c=id('graph');
  var ctx=c.getContext('2d');
  var w=c.clientWidth,h=c.clientHeight;
  if(c.width!==w||c.height!==h){c.width=w;c.height=h;}
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#234573';
  for(var i=0;i<=5;i++){var y=(h/5)*i;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke();}
  if(points.length<2){return;}
  ctx.strokeStyle='#7dd3fc';
  ctx.lineWidth=2;
  ctx.beginPath();
  for(var j=0;j<points.length;j++){
    var x=(j/(maxPoints-1))*w;
    var y=h-(points[j]/100)*h;
    if(j===0)ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.stroke();
}
function poll(){
  fetch('/motion/readings').then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();}).then(function(m){
    id('raw').textContent=m.rawHigh?'HIGH':'LOW';
    id('latched').textContent=m.latched?'YES':'NO';
    id('edgecount').textContent=String(m.edgeCount||0);
    id('signal').textContent=String(m.signal);
    id('highms').textContent=String(m.highDurationMs);
    id('lastms').textContent=String(m.sinceLastDetectedMs);
    var edgeCount=Number(m.edgeCount||0);
    if(edgeCount>lastEdgeCount){
      edgePulseFrames=4;
    }
    lastEdgeCount=edgeCount;
    var plottedSignal=Math.max(0,Math.min(100,m.signal||0));
    if(edgePulseFrames>0){
      plottedSignal=Math.max(plottedSignal,95);
      edgePulseFrames--;
    }
    points.push(plottedSignal);
    if(points.length>maxPoints)points.shift();
    draw();
    if(!lastFetchOk){setStatus('Connection restored',false);} else {setStatus('',false);} 
    lastFetchOk=true;
  }).catch(function(err){
    lastFetchOk=false;
    setStatus('Failed to fetch motion readings: '+(err.message||'network error'),true);
  });
}
setInterval(poll,250);
window.addEventListener('resize',draw);
poll();
</script>
</body>
</html>)html";

  sendHtmlWithToken(MOTION_GRAPH_HTML);
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
  server.send(200, "application/json", json);
}

static void handleMotionConfigGet() {
  if (!checkAuth()) return;

  clampMotionSettings(runtimeConfig.motionSettings);
  const MotionSettings &m = runtimeConfig.motionSettings;
  String json = "{";
  json += "\"enabled\":" + String(m.enabled ? "true" : "false") + ",";
  json += "\"captureImage\":" + String(m.captureImage ? "true" : "false") + ",";
  json += "\"imageCount\":" + String((int)m.imageCount) + ",";
  json += "\"imageDelayDs\":" + String((int)m.imageDelayDs) + ",";
  json += "\"captureVideo\":" + String(m.captureVideo ? "true" : "false") + ",";
  json += "\"videoDurationSec\":" + String((int)m.videoDurationSec) + ",";
  json += "\"wakeOnMotion\":" + String(m.wakeOnMotion ? "true" : "false") + ",";
  json += "\"autoStandby\":" + String(m.autoStandby ? "true" : "false") + ",";
  json += "\"standbyAfterSec\":" + String((int)m.standbyAfterSec) + ",";
  json += "\"detectionIntervalSec\":" + String((int)m.detectionIntervalSec);
  json += "}";
  server.send(200, "application/json", json);
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
  if (server.hasArg("wakeOnMotion")) updated.wakeOnMotion = server.arg("wakeOnMotion") == "1" || server.arg("wakeOnMotion") == "true";
  if (server.hasArg("autoStandby")) updated.autoStandby = server.arg("autoStandby") == "1" || server.arg("autoStandby") == "true";
  if (server.hasArg("standbyAfterSec")) updated.standbyAfterSec = (uint16_t)server.arg("standbyAfterSec").toInt();
  if (server.hasArg("detectionIntervalSec")) updated.detectionIntervalSec = (uint16_t)server.arg("detectionIntervalSec").toInt();

  clampMotionSettings(updated);
  bool wakeDisabledForPin = false;
  if (updated.wakeOnMotion && !pirSupportsRtcWakeup()) {
    updated.wakeOnMotion = false;
    wakeDisabledForPin = true;
  }
  runtimeConfig.motionSettings = updated;

  if (!persistRuntimeConfig(runtimeConfig)) {
    server.send(500, "text/plain", "Failed to save motion configuration");
    return;
  }

  configureMotionWakeup(runtimeConfig.motionSettings.wakeOnMotion);
  applyPirInputMode();
  if (wakeDisabledForPin) {
    server.send(200, "text/plain", "Motion configuration saved; wake on motion is unavailable on the selected PIR pin");
    return;
  }
  server.send(200, "text/plain", "Motion configuration saved");
}

static void handleSDPage() {
  if (!checkAuth()) return;
  sendHtmlWithToken(SD_HTML);
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
  server.send(200, "application/json", json);
}

static void handleSDDownloadWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!transferServer.hasArg("file")) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg("file"), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(403, "text/plain", "Access denied");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(404, "text/plain", "File not found");
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

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "file parameter required");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg("file"), filePath)) {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (isProtectedSDPath(filePath)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/download?file=") + urlEncode(filePath), true));
  server.send(302, "text/plain", "Redirecting to transfer server");
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
    transferServer.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!transferServer.hasArg("file")) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg("file"), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(403, "text/plain", "Access denied");
    return;
  }

  String lower = filePath;
  lower.toLowerCase();
  if (!lower.endsWith(".avi")) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(415, "text/plain", "Playback stream currently supports AVI MJPEG files only");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(404, "text/plain", "File not found");
    return;
  }

  uint32_t fileSize = (uint32_t)file.size();
  if (fileSize < 16U || !file.seek(12U)) {
    file.close();
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "Invalid AVI file");
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
    transferServer.send(400, "text/plain", "Could not locate AVI movi data");
    return;
  }

  WiFiClient client = transferServer.client();
  transferServer.sendHeader("Access-Control-Allow-Origin", "*");
  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=--jpgbound\r\n"
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

  if (isProtectedSDPath(filePath)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  file.close();

  if (!sdIsVideoPath(filePath)) {
    server.sendHeader("Location", String("/sd/view?file=") + urlEncode(filePath));
    server.send(302, "text/plain", "Redirecting to file view");
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
<nav>
  <a href="/">📷 Camera</a>
  <a href="/sd">💾 SD Browser</a>
  <a href="/admin">⚙️ Admin</a>
</nav>
<div class="wrap">
  <div class="panel">
    <h2 class="title">SD Video Viewer</h2>
    <div class="meta">File: __FILENAME__</div>
    __PLAYER_MEDIA__
    <div class="actions">
      <a href="__MEDIA_URL__">Open Raw</a>
      <a href="__DOWNLOAD_URL__">Download</a>
    </div>
    <div class="hint">__PLAYER_HINT__</div>
  </div>
</div>
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
  server.send(200, "text/html", page);
}

static void handleSDViewWorker() {
  if (!checkAuth(transferServer, true)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!transferServer.hasArg("file")) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "file parameter required");
    return;
  }

  if (!initSDCard()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(500, "text/plain", "SD card not available");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(transferServer.arg("file"), filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (isProtectedSDPath(filePath)) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(403, "text/plain", "Access denied");
    return;
  }

  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file || file.isDirectory()) {
    transferServer.sendHeader("Access-Control-Allow-Origin", "*");
    transferServer.send(404, "text/plain", "File not found");
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

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "file parameter required");
    return;
  }

  String filePath;
  if (!normalizeAndValidateSDPath(server.arg("file"), filePath)) {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }

  if (isProtectedSDPath(filePath)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, String("/sd/view?file=") + urlEncode(filePath), true));
  server.send(302, "text/plain", "Redirecting to transfer server");
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

  if (isProtectedSDPath(filePath)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  if (SD_MMC.remove(filePath)) {
    server.send(200, "text/plain", "File deleted");
  } else {
    server.send(500, "text/plain", "Failed to delete file");
  }
}

static void handleSDSortGet() {
  if (!checkAuth()) {
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "application/json", "{\"error\":\"SD card not available\"}");
    return;
  }

  String sortBy;
  String sortDir;
  loadSDSortPreferences(sortBy, sortDir);

  String json = "{\"by\":\"" + jsonEscape(sortBy) + "\",\"dir\":\"" + jsonEscape(sortDir) + "\"}";
  server.send(200, "application/json", json);
}

static void handleSDSortSet() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("by") || !server.hasArg("dir")) {
    server.send(400, "text/plain", "by and dir parameters are required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String sortBy = server.arg("by");
  String sortDir = server.arg("dir");
  sortBy.trim();
  sortDir.trim();

  if (!isValidSortByValue(sortBy) || !isValidSortDirValue(sortDir)) {
    server.send(400, "text/plain", "Invalid sort values");
    return;
  }

  if (!saveSDSortPreferences(sortBy, sortDir)) {
    server.send(500, "text/plain", "Failed to save sort preferences");
    return;
  }

  server.send(200, "text/plain", "Sort preferences saved");
}

static void handleSDMakeDir() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("dir") || !server.hasArg("name")) {
    server.send(400, "text/plain", "dir and name parameters are required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String dirPath;
  if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
    server.send(400, "text/plain", "Invalid directory path");
    return;
  }

  String folderName = server.arg("name");
  folderName.trim();
  if (!validateNewSDName(folderName)) {
    server.send(400, "text/plain", "Invalid folder name");
    return;
  }

  File parent = SD_MMC.open(dirPath, FILE_READ);
  if (!parent || !parent.isDirectory()) {
    server.send(404, "text/plain", "Parent directory not found");
    return;
  }
  parent.close();

  String targetPath = (dirPath == "/") ? ("/" + folderName) : (dirPath + "/" + folderName);
  String normalizedTarget;
  if (!normalizeAndValidateSDPath(targetPath, normalizedTarget)) {
    server.send(400, "text/plain", "Invalid target path");
    return;
  }

  if (isProtectedSDPath(normalizedTarget)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  if (SD_MMC.exists(normalizedTarget)) {
    server.send(409, "text/plain", "A file or folder with this name already exists");
    return;
  }

  if (!SD_MMC.mkdir(normalizedTarget)) {
    server.send(500, "text/plain", "Failed to create folder");
    return;
  }

  server.send(200, "text/plain", "Folder created");
}

static void handleSDRemoveDir() {
  if (!checkAuth()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (!server.hasArg("dir")) {
    server.send(400, "text/plain", "dir parameter required");
    return;
  }

  if (!initSDCard()) {
    server.send(500, "text/plain", "SD card not available");
    return;
  }

  String dirPath;
  if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
    server.send(400, "text/plain", "Invalid directory path");
    return;
  }

  if (dirPath == "/") {
    server.send(400, "text/plain", "Cannot delete root folder");
    return;
  }

  if (isProtectedSDPath(dirPath)) {
    server.send(403, "text/plain", "Access denied");
    return;
  }

  File dir = SD_MMC.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    server.send(404, "text/plain", "Folder not found");
    return;
  }
  dir.close();

  bool blockedProtectedPath = false;
  if (!removeSDDirectoryRecursive(dirPath, blockedProtectedPath)) {
    if (blockedProtectedPath) {
      server.send(403, "text/plain", "Folder contains protected content");
    } else {
      server.send(500, "text/plain", "Failed to delete folder contents");
    }
    return;
  }

  if (!SD_MMC.rmdir(dirPath)) {
    server.send(500, "text/plain", "Failed to delete folder");
    return;
  }

  server.send(200, "text/plain", "Folder deleted");
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

  if (!cfgAccessPass.isEmpty() && !hasSharedAccessToken(transferServer)
      && !transferServer.authenticate("admin", cfgAccessPass.c_str())) {
    sdUploadFailed = true;
    return;
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
    transferServer.send(401, "text/plain", "Unauthorized");
    return;
  }

  if (sdUploadFile) {
    sdUploadFile.close();
  }

  transferServer.sendHeader("Access-Control-Allow-Origin", "*");

  if (sdUploadBlocked) {
    transferServer.send(403, "text/plain", "Access denied");
  } else if (sdUploadFailed) {
    transferServer.send(500, "text/plain", "Upload failed");
  } else if (sdUploadPath.isEmpty()) {
    transferServer.send(400, "text/plain", "No file provided");
  } else {
    transferServer.send(200, "text/plain", "Uploaded: " + sdUploadPath);
  }

  sdUploadPath = "";
  sdUploadFailed = false;
  sdUploadBlocked = false;
}

static void handleSDUploadMain() {
  if (!checkAuth(server)) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  String uploadPath = "/sd/upload";
  if (server.hasArg("dir")) {
    String dirPath;
    if (!normalizeAndValidateSDPath(server.arg("dir"), dirPath)) {
      server.send(400, "text/plain", "Invalid directory path");
      return;
    }
    uploadPath += "?dir=" + urlEncode(dirPath);
  }

  server.sendHeader("Location", buildLocalUrl(HTTP_TRANSFER_PORT, uploadPath, true));
  server.send(307, "text/plain", "Redirecting to transfer server");
}

static void handleFirmwareUploadDataWorker() {
  if (!cfgAccessPass.isEmpty() && !hasSharedAccessToken(transferServer)
      && !transferServer.authenticate("admin", cfgAccessPass.c_str())) {
    firmwareUploadFailed = true;
    return;
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

static bool captureImageToSD(String &savedPath) {
  if (!initSDCard()) {
    return false;
  }

  if (!ensureCaptureDirectory()) {
    return false;
  }

  uint32_t sequence = 0;
  if (!nextCaptureSequence(sequence)) {
    return false;
  }

  savedPath = buildCapturePath(sequence, "jpg");

  if (!ensureCameraReady()) {
    return false;
  }

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return false;
  }

  File file = SD_MMC.open(savedPath, FILE_WRITE);
  if (!file) {
    unlockCameraFrame(fb);
    return false;
  }

  bool ok = file.write(fb->buf, fb->len) == fb->len;
  file.close();
  unlockCameraFrame(fb);

  if (!ok) {
    SD_MMC.remove(savedPath);
  }
  return ok;
}

static void handleCaptureSD() {
  if (!checkAuth()) return;

  String photoPath;
  if (!captureImageToSD(photoPath)) {
    server.send(500, "text/plain", "Failed to capture to SD");
    return;
  }

  server.send(200, "text/plain", String("Saved: ") + photoPath);
}

static bool writeAviHeader(File &file, uint32_t riffSize, uint32_t durationMs, uint32_t frameCount, uint32_t maxFrameSize, uint16_t width, uint16_t height, uint32_t moviListSize) {
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

  resetRecordingState();
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

  if (!writeAviChunkHeader(recordingFile, AVI_VIDEO_CHUNK_ID, entry.size)) {
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

  if (!ensureCameraReady(pdMS_TO_TICKS(1000))) {
    return;
  }

  camera_fb_t *fb = lockAndCaptureFrame(pdMS_TO_TICKS(1000));
  if (!fb) {
    return;
  }

  recordFrameIfDue(fb, now);
  unlockCameraFrame(fb);
}

static bool startRecordingSessionInternal(String &message) {
  if (!initSDCard()) {
    message = "SD card not available";
    return false;
  }

  if (!ensureCameraReady()) {
    message = "Camera unavailable";
    return false;
  }

  if (!recordingMutex || xSemaphoreTake(recordingMutex, portMAX_DELAY) != pdTRUE) {
    message = "Recording lock unavailable";
    return false;
  }

  bool ok = false;
  if (recordingActive) {
    message = "Recording already in progress";
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

  xSemaphoreGive(recordingMutex);
  return ok;
}

static bool stopRecordingSessionInternal(String &message) {
  if (!recordingMutex || xSemaphoreTake(recordingMutex, portMAX_DELAY) != pdTRUE) {
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
    ok = true;
  }

  xSemaphoreGive(recordingMutex);
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
      streamServer.send(404, "text/plain", "Not found");
    });
    streamServer.begin();
    xTaskCreatePinnedToCore(
      streamServerTask,
      "http-stream",
      HTTP_STREAM_TASK_STACK,
      &streamServer,
      1,
      &streamServerTaskHandle,
      ARDUINO_RUNNING_CORE
    );
    Serial.printf("[HTTP] Stream server ready on port %u\n", (unsigned int)HTTP_STREAM_PORT);
  }

  if (!transferServerTaskHandle) {
    transferServer.on("/sd/download", HTTP_GET, handleSDDownloadWorker);
    transferServer.on("/sd/view", HTTP_GET, handleSDViewWorker);
    transferServer.on("/sd/playback", HTTP_GET, handleSDPlaybackWorker);
    transferServer.on("/sd/upload", HTTP_POST, handleSDUploadWorker, handleSDUploadDataWorker);
    transferServer.on("/admin/update", HTTP_POST, handleFirmwareUploadWorker, handleFirmwareUploadDataWorker);
    transferServer.onNotFound([]() {
      transferServer.send(404, "text/plain", "Not found");
    });
    transferServer.begin();
    xTaskCreatePinnedToCore(
      transferServerTask,
      "http-transfer",
      HTTP_TRANSFER_TASK_STACK,
      &transferServer,
      1,
      &transferServerTaskHandle,
      ARDUINO_RUNNING_CORE
    );
    Serial.printf("[HTTP] Transfer server ready on port %u\n", (unsigned int)HTTP_TRANSFER_PORT);
  }
}

// ─── WiFi mode starters ───────────────────────────────────────────────────────
static void registerCameraRoutes() {
  server.on("/",              HTTP_GET,  handleCameraRoot);
  server.on("/stream",        HTTP_GET,  handleStreamMain);
  server.on("/stream/close",  HTTP_POST, handleStreamClose);
  server.on("/motion",        HTTP_GET,  handleMotionPage);
  server.on("/motion/graph",  HTTP_GET,  handleMotionGraphPage);
  server.on("/motion/config", HTTP_GET,  handleMotionConfigGet);
  server.on("/motion/config", HTTP_POST, handleMotionConfigSet);
  server.on("/motion/readings", HTTP_GET, handleMotionReadings);
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
  server.on("/admin/led",     HTTP_GET,  handleAdminLedGet);
  server.on("/admin/led",     HTTP_POST, handleAdminLedSet);
  server.on("/admin/logging", HTTP_GET,  handleAdminLoggingGet);
  server.on("/admin/logging", HTTP_POST, handleAdminLoggingSet);
  server.on("/admin/txpower", HTTP_GET,  handleAdminTxPowerGet);
  server.on("/admin/txpower", HTTP_POST, handleAdminTxPowerSet);
  server.on("/admin/reset",   HTTP_POST, handleAdminReset);
  server.on("/admin/factory-reset", HTTP_POST, handleAdminFactoryReset);
  server.on("/admin/update",  HTTP_POST, handleFirmwareUploadMain);
  server.on("/wifi/list",     HTTP_GET,  handleWifiList);
  server.on("/wifi/add",      HTTP_POST, handleWifiAdd);
  server.on("/wifi/delete",   HTTP_POST, handleWifiDelete);
  server.on("/wifi/move",     HTTP_POST, handleWifiMove);
  server.on("/sd/list",       HTTP_GET,  handleSDList);
  server.on("/sd/download",   HTTP_GET,  handleSDDownloadMain);
  server.on("/sd/view",       HTTP_GET,  handleSDViewMain);
  server.on("/sd/player",     HTTP_GET,  handleSDPlayerMain);
  server.on("/sd/delete",     HTTP_POST, handleSDDelete);
  server.on("/sd/sort",       HTTP_GET,  handleSDSortGet);
  server.on("/sd/sort",       HTTP_POST, handleSDSortSet);
  server.on("/sd/mkdir",      HTTP_POST, handleSDMakeDir);
  server.on("/sd/rmdir",      HTTP_POST, handleSDRemoveDir);
  server.on("/sd/upload",     HTTP_POST, handleSDUploadMain);
  server.on("/record/start",  HTTP_POST, handleRecordStart);
  server.on("/record/stop",   HTTP_POST, handleRecordStop);
  server.onNotFound(handleNotFound);
}

static void startSetupAPMode() {
  wifiModemSleepEnabled = false;
  bool ok = startSoftAPWithRetries(AP_SETUP_SSID, AP_SETUP_PASS);
  if (!ok) {
    Serial.println("[WIFI] Setup AP start failed");
    return;
  }
    ledSetupAPSequence();
    Serial.printf("[WIFI] Protected setup AP started — SSID: %s  Password: %s  IP: %s\n",
    AP_SETUP_SSID, AP_SETUP_PASS, WiFi.softAPIP().toString().c_str());

    server.on("/",     HTTP_GET,  handleSetupRoot);
    server.on("/wifi/scan", HTTP_GET, handleSetupWifiScan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[HTTP] Setup server ready on port 80");
}

static void startCameraAPMode() {
  wifiModemSleepEnabled = false;
  if (!cfgDeviceName.isEmpty()) {
    if (!WiFi.softAPsetHostname(cfgDeviceName.c_str())) {
      Serial.println("[WIFI] Failed to set AP hostname");
    } else {
      Serial.printf("[WIFI] AP hostname set to: %s\n", cfgDeviceName.c_str());
    }
  }

  bool ok = startSoftAPWithRetries(AP_FALLBACK_SSID, cfgAccessPass.c_str());
  if (!ok) {
    Serial.println("[WIFI] Fallback AP start failed (check password length >= 8)");
    return;
  }

  // Reduce TX power — client is always nearby in fallback mode
  WiFi.setTxPower((wifi_power_t)runtimeConfig.txPowerAp);
  Serial.printf("[WIFI] Fallback AP TX power set to %d (raw)\n", (int)runtimeConfig.txPowerAp);

  // Increase beacon interval: fewer beacon TX events.
  {
    wifi_config_t apCfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &apCfg) == ESP_OK) {
      apCfg.ap.beacon_interval = AP_FALLBACK_BEACON_INTERVAL_TU;
      if (esp_wifi_set_config(WIFI_IF_AP, &apCfg) == ESP_OK) {
        Serial.printf("[WIFI] Fallback AP: beacon_interval=%u TU\n",
                      AP_FALLBACK_BEACON_INTERVAL_TU);
      } else {
        Serial.println("[WIFI] Failed to apply extended fallback AP config");
      }
    }
  }

  // Enable modem sleep so idle periods between frames/requests save power
  WiFi.setSleep(true);
  wifiModemSleepEnabled = true;
  Serial.printf("[WIFI] Fallback AP power: reduced TX + modem sleep enabled\n");

  // LED feedback: triple blink when fallback AP activated
  ledFallbackAPSequence();

  Serial.printf("[WIFI] Fallback AP started — SSID: %s  IP: %s\n",
    AP_FALLBACK_SSID, WiFi.softAPIP().toString().c_str());

  registerCameraRoutes();
  server.begin();
  startAuxHttpServers();
  Serial.println("[HTTP] Camera server ready on port 80 (AP mode)");
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

  Serial.println("[NTP] Time sync failed; clock may be incorrect");
  return false;
}

static void startSTAMode() {
  if (runtimeConfig.wifiList.empty()) {
    Serial.println("[WIFI] No saved STA networks — switching to fallback AP");
    startCameraAPMode();
    return;
  }

  if (connectToSavedStaNetworks(true, true)) {
    staConnectedAtBoot = true;
    lastStaReconnectAttemptAt = millis();
    return;
  }

  Serial.println("[WIFI] All saved networks failed — switching to fallback AP");
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

  Serial.println("[OTA] Restarting after successful firmware update");
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
  Serial.println("[ADMIN] Restarting on admin request");
  delay(100);
  ESP.restart();
}

static void configureButtonWakeup() {
  esp_err_t err = esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_GPIO, 0);
  if (err != ESP_OK) {
    Serial.printf("[SLEEP] Failed to enable EXT0 wakeup on GPIO%d (err=0x%x)\n", BUTTON_GPIO, err);
    return;
  }

  rtc_gpio_pullup_en((gpio_num_t)BUTTON_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_GPIO);
  Serial.printf("[SLEEP] Wakeup source configured: button GPIO%d LOW\n", BUTTON_GPIO);
}

static void configureMotionWakeup(bool enabled) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
  if (!enabled) {
    Serial.println("[SLEEP] Motion wakeup disabled");
    return;
  }

  if (!pirSupportsRtcWakeup()) {
    Serial.printf("[SLEEP] Motion wakeup unavailable on GPIO%d; an RTC-capable GPIO is required\n", PIR_GPIO);
    return;
  }

  uint64_t mask = (1ULL << PIR_GPIO);
  esp_err_t err = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (err != ESP_OK) {
    Serial.printf("[SLEEP] Failed to enable EXT1 wakeup on GPIO%d (err=0x%x)\n", PIR_GPIO, err);
    return;
  }

  rtc_gpio_pullup_dis((gpio_num_t)PIR_GPIO);
  rtc_gpio_pulldown_en((gpio_num_t)PIR_GPIO);
  Serial.printf("[SLEEP] Wakeup source configured: motion GPIO%d HIGH\n", PIR_GPIO);
}

static void handleWakeupIndicator() {
  if (bootWakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.printf("[BOOT] Wakeup from deep sleep via button GPIO%d\n", BUTTON_GPIO);
    ledBlinkCount(2, BUTTON_BLINK_ON_MS, BUTTON_BLINK_OFF_MS);
    return;
  }

  if (bootWakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    if ((mask & (1ULL << PIR_GPIO)) != 0ULL) {
      Serial.printf("[BOOT] Wakeup from deep sleep via motion GPIO%d\n", PIR_GPIO);
      motionBootEventPending = true;
    }
  }

  ledBootSequence();
}

static void prepareDeviceForDeepSleep() {
  streamClientAbortRequested = true;

  if (recordingMutex && xSemaphoreTake(recordingMutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
    if (recordingActive) {
      Serial.println("[SLEEP] Stopping active recording before deep sleep");
      stopRecordingSession(true);
    }
    xSemaphoreGive(recordingMutex);
  }

  digitalWrite(LED_FLASH_GPIO_NUM, LOW);
  flashEnabled = false;

  if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
    if (cameraInitialized) {
      esp_err_t err = esp_camera_deinit();
      if (err != ESP_OK) {
        Serial.printf("[SLEEP] Camera deinit failed: 0x%x\n", err);
      } else {
        cameraInitialized = false;
      }
    }
    xSemaphoreGive(cameraMutex);
  }

  powerDownCameraHardware();
  setWifiModemSleep(false, "deep sleep");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
}

[[noreturn]] static void enterDeepSleepNow(const char *reason, int blinkCount, bool allowMotionWake) {
  Serial.printf("[SLEEP] %s\n", reason ? reason : "Entering deep sleep");
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
  Serial.printf("[MOTION] Triggered (%s)\n", source ? source : "runtime");

  if (runtimeConfig.motionSettings.captureImage) {
    motionPendingImages = runtimeConfig.motionSettings.imageCount;
    motionNextImageAt = now;
  }

  if (runtimeConfig.motionSettings.captureVideo) {
    String message;
    if (recordingActive) {
      motionVideoManagedRecording = true;
      motionRecordingStopAt = now + ((unsigned long)runtimeConfig.motionSettings.videoDurationSec * 1000UL);
      Serial.printf("[MOTION] Extended recording stop deadline by motion to %lus\n", (unsigned long)runtimeConfig.motionSettings.videoDurationSec);
    } else if (startRecordingSessionInternal(message)) {
      motionVideoManagedRecording = true;
      motionRecordingStopAt = now + ((unsigned long)runtimeConfig.motionSettings.videoDurationSec * 1000UL);
      Serial.printf("[MOTION] %s\n", message.c_str());
    } else {
      Serial.printf("[MOTION] Failed to start recording: %s\n", message.c_str());
    }
  }

  if (motionPendingImages == 0 && !motionVideoManagedRecording) {
    closeMotionActionWindow();
  }
}

static void serviceButtonSleepRequest() {
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

    if (buttonSleepArmed && !buttonSleepRequestPending) {
      buttonSleepRequestPending = true;
      buttonSleepRequestAt = now;
      buttonSleepArmed = false;
      Serial.printf("[BUTTON] Sleep requested, entering deep sleep in %lu ms\n", BUTTON_SLEEP_DELAY_MS);
    }
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
    motionActionWindowActive = false;
    motionIgnoreUntilAt = 0;
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
      Serial.printf("[MOTION] Image captured: %s\n", path.c_str());
    } else {
      Serial.println("[MOTION] Failed to capture image");
    }

    --motionPendingImages;
    motionNextImageAt = now + ((unsigned long)runtimeConfig.motionSettings.imageDelayDs * 100UL);
  }

  if (motionVideoManagedRecording && motionRecordingStopAt != 0 && now >= motionRecordingStopAt) {
    String message;
    if (stopRecordingSessionInternal(message)) {
      Serial.printf("[MOTION] %s\n", message.c_str());
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

  if (recordingActive || motionPendingImages > 0 || motionVideoManagedRecording) {
    return;
  }

  unsigned long now = millis();
  if ((now - motionLastActivityAt) >= ((unsigned long)runtimeConfig.motionSettings.standbyAfterSec * 1000UL)) {
    enterDeepSleepNow("Auto stand-by timeout", 0, true);
  }
}

// ─── Arduino entry points ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP32-CAM starting");
  WiFi.onEvent(onWifiEvent);

  bootWakeCause = esp_sleep_get_wakeup_cause();

  // Initialize LED and provide boot feedback
  initLED();
  handleWakeupIndicator();

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
  attachInterrupt(digitalPinToInterrupt(PIR_GPIO), onPirEdgeInterrupt, CHANGE);
  pinMode(LED_FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(LED_FLASH_GPIO_NUM, LOW);
  powerDownCameraHardware();
  configureButtonWakeup();

  buttonLastRawPressed = (digitalRead(BUTTON_GPIO) == LOW);
  buttonStablePressed = buttonLastRawPressed;
  buttonLastChangeAt = millis();
  buttonSleepArmed = !buttonStablePressed;
  buttonSleepRequestPending = false;

  Serial.printf("[GPIO] Button: GPIO%d (to GND, active LOW), PIR DATA: GPIO%d\n", BUTTON_GPIO, PIR_GPIO);
  Serial.println("[GPIO] PIR power: VCC -> 5V (or compatible rail), GND -> GND");
  logSharedPinCaveats();

  motionLastActivityAt = millis();
  motionLastDetectedAt = 0;
  motionPendingImages = 0;
  motionRecordingStopAt = 0;
  motionVideoManagedRecording = false;
  motionActionWindowActive = false;
  motionIgnoreUntilAt = 0;
  motionBootEventPending = false;

  // Load stored encrypted configuration from SD card.
  StoredConfig cfg;
  if (loadRuntimeConfigWithRetries(cfg)) {
    runtimeConfig = cfg;
    cfgAccessPass = cfg.adminPass;
    cfgDeviceName = cfg.deviceName;
    ledAccessBlinkEnabled = cfg.ledAccessBlink;
    gLoggingEnabled = cfg.loggingEnabled;
    if (cfgDeviceName.isEmpty()) {
      cfgDeviceName = "ESP32-CAM";
    }
    isConfigured = true;
  } else {
    cfgDeviceName = "ESP32-CAM";
    ledAccessBlinkEnabled = false;
    gLoggingEnabled = true;
    isConfigured = false;
  }

    clampMotionSettings(runtimeConfig.motionSettings);
    if (runtimeConfig.motionSettings.wakeOnMotion && !pirSupportsRtcWakeup()) {
      runtimeConfig.motionSettings.wakeOnMotion = false;
      Serial.printf("[CFG] Disabled wake on motion because GPIO%d is not RTC-capable\n", PIR_GPIO);
    }
    applyPirInputMode();
    configureMotionWakeup(runtimeConfig.motionSettings.wakeOnMotion);
    updateSdLoggingState();
    if (bootWakeCause == ESP_SLEEP_WAKEUP_EXT1) {
      uint64_t mask = esp_sleep_get_ext1_wakeup_status();
      motionBootEventPending = ((mask & (1ULL << PIR_GPIO)) != 0ULL);
    }

    routeAccessToken = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX);
    Serial.printf("[HTTP] Shared route token initialized (%u chars)\n", (unsigned int)routeAccessToken.length());
    Serial.printf("[CFG] Configured: %s\n", isConfigured ? "yes" : "no");

    Serial.printf("[CAM] Lazy init enabled with idle timeout %lu ms\n", cameraIdleTimeoutMs);

    if (isConfigured) {
        startSTAMode();
    } else {
      startSetupAPMode();
    }
}

void loop() {
  server.handleClient();
  serviceNtpSync();
  serviceStaReconnect();
  serviceRecording();
  serviceMotionDetection();
  serviceMotionActions();
  serviceCameraIdleTimeout();
  serviceMotionAutoStandby();
  serviceButtonSleepRequest();
  servicePendingFirmwareRestart();
  servicePendingAdminRestart();
  delay(2);
}
