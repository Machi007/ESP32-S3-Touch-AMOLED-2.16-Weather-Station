# ESP32-S3-Touch-AMOLED-2.16 Weather App — English UI, 90° Rotation

This Arduino IDE project provides current weather and a six-day forecast for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (480 × 480, CO5300 display, CST9220 touch controller).

## Features

- Current temperature, feels-like temperature, humidity, wind speed and direction, cloud cover, and current precipitation.
- Six-day forecast, including today's high/low temperatures and maximum precipitation probability.
- Weather icons drawn by the sketch; no external image assets are required.
- Wi-Fi status indicator and battery icon with percentage.
- Last successful weather-update time.
- Automatic refresh every 15 minutes, with retries every 30 seconds if data has not loaded successfully.
- A short tap on the touchscreen triggers a manual refresh; horizontal swipes switch between the current-weather and forecast screens.
- `+/KEY` (GPIO18) increases brightness; `BOOT/-` (GPIO0) decreases it. `PWR` toggles the display off/on.
- Brightness steps (30%, 50%, 75%, 100%) are saved in NVS and restored after reboot.
- The UI and touch coordinates are rotated 90 degrees clockwise in software.

## Before compiling

### 1. Configure Wi-Fi and location

Open `ESP32S3_AMOLED_Weather_EN.ino` and edit the following values near the top of the sketch:

```cpp
const char *WIFI_SSID     = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *CITY_NAME = "PRAGUE";
const float LATITUDE  = 50.0755f;
const float LONGITUDE = 14.4378f;
```

Replace the Wi-Fi placeholders with your actual network credentials. Change `CITY_NAME`, `LATITUDE`, and `LONGITUDE` if you want weather for another location. The board has no GPS, so location is configured manually.

### 2. Install required libraries

Install these libraries using Arduino IDE's Library Manager:

- **GFX Library for Arduino**
- **SensorLib** (version 0.4.1 or newer; provides `TouchDrv.hpp` / CST9220 support)
- **ArduinoJson** (version 6.x or 7.x)

The sketch uses the standard ESP32 libraries `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`, `Wire`, and `time`.

### 3. Select board settings

- Board: **ESP32S3 Dev Module** (or the matching Waveshare board profile, if installed).
- Enable PSRAM; **OPI PSRAM** is typically required for this board. The 480 × 480 software canvas needs a substantial contiguous memory allocation.
- Upload the sketch and open Serial Monitor at **115200 baud**.

The Serial Monitor should show a startup line similar to:

```text
[WEATHER] Running build: ROT90-ENGLISH-2026-10-18-A
```

## Button behaviour

| Control | Function |
|---|---|
| `+/KEY` | Increase brightness by one step: 30 → 50 → 75 → 100% |
| `BOOT/-` | Decrease brightness by one step: 100 → 75 → 50 → 30% |
| `PWR` | Turn the display off; press again to restore the saved brightness |
| Short touchscreen tap | Fetch current weather immediately |
| Swipe left/right | Switch between current weather and forecast |

**BOOT/- hardware note:** On some board revisions, GPIO0 is also related to the `CHIP_PU`/reset signal. If pressing BOOT/- restarts the board, the running firmware cannot process that press as a normal button. In that case, assign brightness-down to another available GPIO or use a different input method.

## Weather service and security note

Weather data is retrieved from the public Open-Meteo API, without an API key. The sketch uses `WiFiClientSecure::setInsecure()` to keep HTTPS setup simple. This disables certificate verification; for a deployment that requires certificate validation, replace it with CA certificate validation.

The forecast timezone is set to `Europe/Prague` by default. The application also configures NTP using the Prague/Central European timezone rules.

## Troubleshooting

- If Wi-Fi does not connect, verify the SSID/password and use a **2.4 GHz** Wi-Fi network.
- If the API request fails, the Serial Monitor reports the HTTP status, content type, and the beginning of the response body.
- If display/canvas initialization fails, verify that PSRAM is enabled and that the board profile is suitable.
- If the screen works but touch does not, verify that the installed SensorLib version supports the CST9220 controller.

This project was translated and prepared in the current environment, but it was not compiled with Arduino IDE or tested on a physical board here. Confirm compilation and hardware behaviour on the target device.
