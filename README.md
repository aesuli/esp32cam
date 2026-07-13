# ESP32 multipurpose CAM

Firmware for AI-Thinker ESP32-CAM (OV3660) with web streaming and camera controls.

## Hardware and Assembly

### Hardware parts

- AI-Thinker ESP32-CAM board with OV3660 camera.
- Stable 5V power source suitable for ESP32-CAM current peaks.

### Wiring

None required

## First setup

On first boot (or if no saved config is found), the device starts in setup mode.

- Setup WiFi AP:
  - SSID: ESP32-CAM-Setup
  - Password: ESP32-CAM
- Open https://192.168.4.1 and accept the browser warning for the self-signed certificate, then complete setup:
  - WiFi network SSID and password
  - Admin password (min 8 characters)
  - Device name (default: ESP32-CAM)
  - You can use the scan button to list nearby WiFi networks.

## Normal Operation

After setup, the device boots into normal operation.

- It tries saved WiFi networks in priority order until one connects.
- Main web interface: port 443 (self-signed HTTPS, HTTP Basic Auth, user: `admin`, password: configured admin password).
- Live MJPEG stream endpoint: `/stream` on HTTPS port 444. This accepts HTTP Basic Auth and also the internal shared route token used by the camera page.
- Direct JPEG snapshot URLs: `/snapshot` or `/snapshot.jpg` on HTTPS port 443. These require HTTP Basic Auth.
- Firmware upload endpoint: HTTPS port 445.
- Plain HTTP remains available internally on ports 80, 81, and 82 for the HTTPS forwarding layer.
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
- Configure WiFi TX power for STA mode and fallback AP mode.
- Upload new firmware (`.bin`) for OTA update.
- Restart device.
- Run factory reset (deletes config and returns to setup mode).

### Camera page

Use this page for live view and manual capture.

- Live MJPEG stream view.
- Use `/stream` for clients that need a direct authenticated stream URL.
- Show/hide stream without leaving the page.
- Use `/snapshot` or `/snapshot.jpg` for a one-shot authenticated JPEG.
- Flash LED on/off.
- Camera tuning controls, including:
  - resolution
  - brightness, contrast, saturation
  - JPEG quality
  - special effects
  - white balance options
  - mirror/flip
  - lens correction
  - optional 90-degree view rotation in the browser

### Admin page

Use this page for system-wide settings and maintenance.

- Configure WiFi priority list and credentials.
- Manage admin password and device name.
- Set time manually or request NTP sync.
- Configure LED blink-on-access behavior.


## License

© 2026 Andrea Esuli

[BSD 3-Clause License](LICENSE).
