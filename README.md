# ESP32 multipurpose CAM

Firmware for AI-Thinker ESP32-CAM (OV3660) with web streaming, camera controls, and SD-backed secure configuration.

## Hardware and pins

- microSD runs in 1-bit SD_MMC mode, but GPIO12/GPIO13 remain physically tied to the SD socket/card DAT2/DAT3 pull-up network.
- Push button: connect between GPIO13 and GND (configured as INPUT_PULLUP, active LOW).
- PIR sensor: DATA to GPIO12, GND to GND, VCC to 5V (or the voltage accepted by your PIR module).
- Important: GPIO13 can show edges when the card is inserted or removed because it is SD DAT3, and GPIO12 is less PIR-friendly because it is SD DAT2 plus an ESP32 bootstrap/pulldown pin.
- Temporary test mode: the Motion page now includes a button to disable the SD stack until reboot so you can test whether PIR detection improves when the SD card is no longer mounted.
- Current button behavior: on press, firmware waits 1 second, blinks the status LED 3 fast times, then enters deep sleep.
- Wake-up source: pressing the same button wakes from deep sleep (EXT0 on GPIO13) and blinks the status LED 2 fast times.

## Configuration storage

- WiFi credentials are saved as an ordered list of SSID/password entries, together with the admin password, in the encrypted microSD file /config.enc.
- The firmware can still read the older single-network config format and will migrate it on the next save.
- If the SD card is missing or /config.enc does not exist, the device enters first-setup mode.

## First-setup mode

- Device starts AP:
	- SSID: ESP32-CAM-Setup
	- Security: WPA2-PSK
	- Password: ESP32-CAM
- Open http://192.168.4.1 and set:
	- First WiFi SSID/password
	- Admin password (minimum 8 characters)
	- Device name (e.g., "Living Room Cam", defaults to "ESP32-CAM")
- A scan button lists nearby SSIDs so you can pick one instead of typing it manually.
- Admin password must be at least 8 characters (required for fallback AP security).
- Device name is used as the network hostname (published via WiFi.setHostname())

## Device name management

- Device name is set during initial setup and can be changed from the Admin page (/admin)
- When the device connects to WiFi, it advertises itself on the network with the configured device name as the hostname
- This allows easy discovery of the device on the network (e.g., via hostname lookup or network scanning tools)
- Device names can be up to 32 characters and must contain only printable ASCII characters

## WiFi management after setup

- After login to the main camera UI, you can add more WiFi credentials.
- The WiFi panel can scan and list nearby SSIDs, including signal strength and security type.
- Stored networks can be reordered with Up/Down controls; boot will try them in that order.
- Stored networks can be deleted individually.
- Adding an SSID that already exists updates its saved password without changing its position.

## Admin password management

- After login to the main camera UI, you can change the admin password by providing the current password and the new password twice.
- The new admin password is applied immediately for HTTP Basic Auth.
- If the device is currently running in fallback AP mode, the AP password updates the next time that AP is started.

## Normal operation

- Device tries each stored WiFi in priority order until one connects.
- Web UI on port 80 with Basic Auth:
	- Username: admin
	- Password: configured admin password
- MJPEG stream endpoint is served on port 80 at /stream and uses the same Basic Auth as the rest of the web UI.
- Admin page supports browser-based firmware upload. After a successful upload, the ESP32 reboots into the new image automatically.

## WiFi connection failure fallback

- If all stored STA credentials fail, config is kept.
- Device starts fallback AP with camera features enabled (same standard functionalities):
	- SSID: ESP32-CAM
	- Password: admin password
- Web UI, capture, controls, status, and stream remain available while in fallback AP mode.

## Multi-page web interface

The firmware provides a three-page web interface (all require authentication):

### Camera page
- Live MJPEG stream from the camera
- Camera settings (resolution, brightness, contrast, saturation, JPEG quality, effects, white balance)
- **Capture button**: Takes a snapshot and saves it to `/capture/IMG_YYYYMMDD_HHMMSS.jpg` on the microSD card
- **Record button**: Starts/stops Motion JPEG AVI recording to `/capture/VID_YYYYMMDD_HHMMSS.avi` (button toggles between ⏺️ Record and ⏹️ Stop)

### Admin page
- Manage WiFi credentials (reorder, add, update, delete, scan)
- Change admin password
- Change the device name
- Set or sync device time
- Enable or disable LED URL-access blink
- Enable or disable global logging (serial output and `/log.txt` SD logging)
- Upload a new firmware binary for OTA update

### SD Browser page
- Browse files on the microSD card
- Create folders in the current directory
- Download any file to your computer
- Delete files from the card
- Delete folders from the card (recursive)
- Upload new files to the currently open folder
- Double-click folders to open and files to view (Open buttons remain available)
- Sorting preferences are persisted in hidden `/.sort` on SD (default: Name + Ascending when missing)

## Capture and recording

- **Still captures** are saved to `/capture/IMG_*.jpg` with automatic timestamp-based filenames
- **Video recordings** are saved to `/capture/VID_*.avi` in Motion JPEG AVI format with automatic timestamp-based filenames and timing based on the real capture duration
- Both capture and record operations save to the microSD card
- File-based storage allows later retrieval and analysis via the SD Browser page

## Firmware OTA update

- Build the firmware normally with PlatformIO.
- The compiled image is generated at `.pio/build/esp32cam/firmware.bin`.
- Open the Admin page, choose that `.bin` file in the Firmware Update panel, and upload it.
- The firmware is transferred over the existing authenticated web interface and the device reboots automatically after a successful update.
- The project now uses `partitions_ota.csv` so the flash has two application slots, which is required for reliable self-update on ESP32.
