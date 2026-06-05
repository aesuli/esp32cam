# ESP32 multipurpose CAM

Firmware for AI-Thinker ESP32-CAM (OV3660) with web streaming, camera controls, motion actions, and SD-backed secure configuration.

## Hardware and Assembly

### Hardware parts

- AI-Thinker ESP32-CAM board with OV3660 camera.
- microSD card (required for setup, settings persistence, captures, recordings, and optional logs).
- PIR motion sensor (digital output).
- Optional push button for hardware control.
- Stable 5V power source suitable for ESP32-CAM current peaks.

### Wiring

- PIR sensor:
  - VCC -> 3.3V
  - GND -> GND
  - DATA/OUT -> GPIO13
- Optional physical button:
  - One side -> GPIO3 (RX pin)
  - Other side -> 3.3V
  - The firmware keeps GPIO3 pulled down internally, so idle state is LOW and press is HIGH.
- microSD:
  - Insert card in the ESP32-CAM slot.
  - Firmware uses SD_MMC 1-bit mode.

## First setup

On first boot (or if no saved config is found), the device starts in setup mode.

- Setup WiFi AP:
  - SSID: ESP32-CAM-Setup
  - Password: ESP32-CAM
- Open http://192.168.4.1 and complete setup:
  - WiFi network SSID and password
  - Admin password (min 8 characters)
  - Device name (default: ESP32-CAM)
  - You can use the scan button to list nearby WiFi networks.

### Storage behavior

- SD card is required.
- Configuration is encrypted and stored as `/config.enc` on the SD card.
- If `/config.enc` is missing, the device returns to first setup.
- To migrate to another SD card while keeping settings, copy `/config.enc` to the new card.
- Captures and recordings are stored under `/capture`.
- If enabled in Admin, runtime logs are written to `/log.txt`.

## Normal Operation

After setup, the device boots into normal operation.

- It tries saved WiFi networks in priority order until one connects.
- Main web interface: port 80 (HTTP Basic Auth, user: `admin`, password: configured admin password).
- Live stream endpoint: `/stream` on port 81.
- SD transfer and firmware upload endpoints: port 82.
- If no saved WiFi connects, it starts fallback AP:
  - SSID: ESP32-CAM
  - Password: current admin password

### Admin page

Use this page for system-level configuration.

- Manage WiFi profiles: add, edit, reorder priority, enable/disable, delete, and scan nearby networks.
- Switch each profile between DHCP and static addressing (IP, gateway, mask, DNS).
- Change admin password.
- Change device name.
- Set time manually or sync via NTP.
- Enable or disable LED blink on URL access.
- Enable or disable writing logs to `/log.txt`.
- Configure WiFi TX power for STA mode and fallback AP mode.
- Upload new firmware (`.bin`) for OTA update.
- Restart device.
- Run factory reset (deletes config and returns to setup mode).

### Camera page

Use this page for live view and manual capture.

- Live MJPEG stream view.
- Show/hide stream without leaving the page.
- Flash LED on/off.
- Capture still image to `/capture/SEQUENCE-YYYYMMDD_HHMMSS.jpg`.
- Start/stop manual video recording to `/capture/SEQUENCE-YYYYMMDD_HHMMSS.avi`.
- Camera tuning controls, including:
  - resolution
  - brightness, contrast, saturation
  - JPEG quality
  - special effects
  - white balance options
  - mirror/flip
  - lens correction
  - optional 90-degree view rotation in the browser

### Motion page

Use this page to configure PIR-triggered behavior.

- Enable/disable motion detection.
- After enabling, arming starts after a fixed 10-second delay.
- On motion, optionally:
  - capture one or more images (with configurable interval)
  - record a video (configurable duration)
- Set detection cooldown interval between triggers.
- Optional notify URL (HTTP GET) on motion event.
- Open live Motion Graph page for PIR signal visualization.
- Optional auto-standby after 2 minutes without authenticated requests and without motion events.
- Manual Enter Standby Now command from the page.
- Deep standby wake source is PIR HIGH on GPIO13.

### SD browser page

Use this page for SD file management.

- Browse folders and files.
- Create folders.
- Delete files and folders (folder delete is recursive).
- Upload files to the current folder (file picker or drag and drop).
- Download single or multiple selected files.
- Open supported files in browser.
- Built-in playback page for recorded media.
- Sort by name, size, or type (ascending/descending).

## Physical button

The optional button connected to GPIO3 (RX) provides quick local control.

- Single press: toggles motion detection on/off.
- Double press: toggles WiFi on/off.
- Long press (~1 second): controls recording:
  - starts manual recording when idle
  - stops manual recording when already recording
  - if motion-triggered recording is active, takes over into manual recording

Notes:

- During recording, short/double click actions are suppressed; long press remains active.
- Button input is active-HIGH, with internal pulldown bias.

## License

© 2026 Andrea Esuli

[BSD 3-Clause License](LICENSE).
