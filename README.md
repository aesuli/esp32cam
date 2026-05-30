# ESP32 multipurpose CAM

Firmware for AI-Thinker ESP32-CAM (OV3660) with web streaming, camera controls, motion actions, and SD-backed secure configuration.

## Hardware and pins

- SD runs in 1-bit SD_MMC mode.
- PIR sensor: DATA to GPIO13, VCC to 3.3V.
- PIR input uses pulldown while awake and during deep sleep wake setup (RTC pulldown), so idle should remain LOW.

## Configuration storage

- SD card is required to save configuration. 
- Configuration is stored in encrypted SD file `/config.enc`. 
- If `/config.enc` does not exist, the device enters first-setup mode.
- When changing the SD card, copy `/config.enc` to the new card to preserve the configuration.

## First-setup mode

- Device starts AP:
  - SSID: ESP32-CAM-Setup
  - Security: WPA2-PSK
  - Password: ESP32-CAM
- Open http://192.168.4.1 and set:
  - First WiFi SSID/password
  - Admin password (minimum 8 characters)
  - Device name (defaults to "ESP32-CAM")
- A scan button lists nearby SSIDs so you can pick one instead of typing it manually.

## Normal operation

- Device tries each stored WiFi in priority order until one connects.
- Main web UI runs on port 80 with Basic Auth:
  - Username: admin
  - Password: configured admin password
- MJPEG stream is served from `/stream` on port 81.
- File transfer and firmware upload worker endpoints run on port 82.

## WiFi connection failure fallback

- If all stored STA credentials fail, device starts fallback AP:
  - SSID: ESP32-CAM
  - Password: admin password

## Web interface

The web interface is composed of four main pages:

### Camera page
- Live MJPEG stream
- Camera settings (resolution, brightness, contrast, saturation, JPEG quality, effects, white balance, mirror/flip, lens correction)
- Flash on/off button
- **Capture button**: saves a JPEG to `/capture/SEQUENCE-YYYYMMDD_HHMMSS.jpg`
- **Record button**: starts/stops MJPEG AVI recording to `/capture/SEQUENCE-YYYYMMDD_HHMMSS.avi`
- **RX button integration**: short press toggles motion detection when idle; long press starts recording; any short press while recording stops the recording
- Stream visibility toggle (show/hide stream)

### Motion page
- Enable or disable motion detection
- Fixed 10-second activation delay after motion detection is enabled
- Optional capture of images on motion (count + interval)
- Optional video recording on motion (duration)
- Detection interval (cooldown) between motion events while the device is awake
- Optional Notify URL (HTTP GET sent when motion is detected)
- Live motion graph page with current PIR readings
- Optional standby after 2 minutes of inactivity (no authenticated URL requests and no motion triggers)
- Manual "Enter Standby Now" button (confirmation required); wake source is PIR GPIO13 HIGH

### SD Browser page
- Browse files and folders on SD
- Create and delete folders (recursive delete supported)
- Upload files to current folder
- Download files
- Open supported files in browser
- Built-in media playback page for recordings
- Sort options for listing

### Admin page
- Manage WiFi credentials (reorder, add, update, delete, scan)
- Change admin password
- Change device name
- Set or sync device time
- Enable or disable LED URL-access blink
- Enable or disable `/log.txt` SD logging
- Set WiFi TX power for STA and fallback AP
- Upload firmware binary for OTA update
- Restart device
- Factory reset (delete config and return to setup mode)

## License

© 2026 Andrea Esuli

[BSD 3-Clause License](LICENSE).
