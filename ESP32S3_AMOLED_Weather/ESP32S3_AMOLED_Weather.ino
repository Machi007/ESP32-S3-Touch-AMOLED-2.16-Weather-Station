/*
  Weather app for Waveshare ESP32-S3-Touch-AMOLED-2.16 (480x480, CO5300, CST9220)
  Arduino IDE, ESP32 core 3.x
  Includes managed Wi-Fi connection, detailed Open-Meteo diagnostics,
  persistent brightness settings, button controls, and software UI rotation by 90 degrees clockwise.

  Libraries:
    - GFX Library for Arduino
    - SensorLib (>= 0.4.1; TouchDrv.hpp / CST9220)
    - ArduinoJson (6.x or 7.x)

  Configure Wi-Fi and location in the USER CONFIGURATION section.
  Weather data: Open-Meteo, no API key required. Refresh interval: 15 minutes.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TouchDrv.hpp>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <time.h>
#include <Preferences.h>

static const char *APP_BUILD_TAG = "ROT90-ENGLISH-2026-10-18-A";

// ======================= USER CONFIGURATION =======================
const char *WIFI_SSID     = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Default location is Prague. Change the city name and coordinates for your location.
// The board has no GPS, so it cannot determine its location automatically.
const char *CITY_NAME = "PRAGUE";
const float LATITUDE  = 50.0755f;
const float LONGITUDE = 14.4378f;

const uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
const uint32_t RETRY_REFRESH_MS   = 30UL * 1000UL;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000UL;
// Brightness: BOOT/- decreases and +/KEY increases it (30 -> 50 -> 75 -> 100%).
// PWR turns the display off/on; the selected brightness is saved in NVS.
const uint8_t DISPLAY_BRIGHTNESS_LEVELS[] = {77, 128, 191, 255};
const uint8_t DISPLAY_BRIGHTNESS_STEPS = sizeof(DISPLAY_BRIGHTNESS_LEVELS) / sizeof(DISPLAY_BRIGHTNESS_LEVELS[0]);
static constexpr uint8_t DEFAULT_BRIGHTNESS_STEP = 3; // 100% on first boot
// ==================================================================

// Display / touch pinout for ESP32-S3-Touch-AMOLED-2.16
static constexpr int LCD_SDIO0 = 4;
static constexpr int LCD_SDIO1 = 5;
static constexpr int LCD_SDIO2 = 6;
static constexpr int LCD_SDIO3 = 7;
static constexpr int LCD_SCLK  = 38;
static constexpr int LCD_CS    = 12;
static constexpr int LCD_RESET = 39;
static constexpr int LCD_WIDTH = 480;
static constexpr int LCD_HEIGHT = 480;

static constexpr int IIC_SDA = 15;
static constexpr int IIC_SCL = 14;
static constexpr int TP_RESET = 40;
static constexpr int TP_INT = 11;
static constexpr int KEY_BUTTON = 18;  // +/KEY button
static constexpr int BOOT_BUTTON = 0; // BOOT/- button (GPIO0; see the warning in README)

// AXP2101 PMU on the shared I2C bus
static constexpr uint8_t PMU_ADDRESS = 0x34;
static constexpr uint8_t PMU_REG_POWER_KEY_IRQ_STATUS = 0x49;
static constexpr uint8_t PMU_REG_BATTERY_PERCENT = 0xA4;
static constexpr uint8_t PMU_POWER_KEY_IRQ_MASK = 0x0C;
static constexpr uint8_t PMU_POWER_KEY_SHORT_IRQ = 0x08;

// RGB565 palette
static constexpr uint16_t C_BG       = 0x0820;
static constexpr uint16_t C_PANEL    = 0x10A3;
static constexpr uint16_t C_PANEL2   = 0x1928;
static constexpr uint16_t C_WHITE    = 0xFFFF;
static constexpr uint16_t C_MUTED    = 0x9D3A;
static constexpr uint16_t C_CYAN     = 0x4DFF;
static constexpr uint16_t C_BLUE     = 0x45BF;
static constexpr uint16_t C_YELLOW   = 0xFEA0;
static constexpr uint16_t C_ORANGE   = 0xFCA0;
static constexpr uint16_t C_GREEN    = 0x4E69;
static constexpr uint16_t C_RED      = 0xF986;
static constexpr uint16_t C_CLOUD    = 0xD6FB;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *display = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
// We draw to a canvas rotated 90 degrees clockwise; the physical display controller
// remains in its default configuration so the CO5300 orientation is not overwritten by an incompatible MADCTL setting.
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, display, 0, 0, 1);
TouchDrvCST92xx touch;
Preferences brightnessPrefs;
bool brightnessPrefsReady = false;

void renderActivePage();

struct ForecastDay {
  char date[11];
  float tMax;
  float tMin;
  int weatherCode;
  int precipProbability;
};

struct WeatherData {
  float temperature = NAN;
  float apparentTemperature = NAN;
  float humidity = NAN;
  float windSpeed = NAN;
  float windDirection = NAN;
  float cloudCover = NAN;
  float precipitation = NAN;
  int weatherCode = -1;
  int isDay = 1;
  char updateTime[20] = "--:--";
  ForecastDay days[6];
  int dayCount = 0;
  bool valid = false;
};

WeatherData weather;
int batteryPercent = -1;
bool touchAvailable = false;
bool displayOn = true;
uint8_t brightnessStep = DEFAULT_BRIGHTNESS_STEP; // index: 30%, 50%, 75%, 100%
wl_status_t lastUiWifiStatus = WL_DISCONNECTED;
int activePage = 0; // 0 = current weather, 1 = forecast
uint32_t lastWeatherAttempt = 0;
uint32_t lastBatteryRead = 0;
uint32_t lastTouchPoll = 0;

bool keyRawLast = HIGH;
bool keyStable = HIGH;
uint32_t keyDebounceAt = 0;
bool bootRawLast = HIGH;
bool bootStable = HIGH;
uint32_t bootDebounceAt = 0;

bool touchFingerDown = false;
int touchStartX = 0, touchStartY = 0;
int touchLastX = 0, touchLastY = 0;
uint32_t touchStartAt = 0;
int16_t touchX[2] = {0, 0};
int16_t touchY[2] = {0, 0};

// ------------------------------ I2C / PMU ------------------------------
bool pmuRead(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(PMU_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)PMU_ADDRESS, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

bool pmuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PMU_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void readBattery() {
  uint8_t value = 0xFF;
  if (pmuRead(PMU_REG_BATTERY_PERCENT, value) && value <= 100) {
    batteryPercent = (int)value;
  } else {
    batteryPercent = -1;
  }
}

const char *brightnessLabel(uint8_t step) {
  static const char *labels[] = {"30%", "50%", "75%", "100%"};
  if (step >= DISPLAY_BRIGHTNESS_STEPS) step = DEFAULT_BRIGHTNESS_STEP;
  return labels[step];
}

void saveBrightnessSetting() {
  if (brightnessPrefsReady) {
    brightnessPrefs.putUChar("bright", brightnessStep);
  }
}

void applyBrightnessSetting(bool redraw = true) {
  const uint8_t brightness = displayOn ? DISPLAY_BRIGHTNESS_LEVELS[brightnessStep] : 0;
  display->setBrightness(brightness);
  if (redraw && displayOn) renderActivePage();
}

void adjustBrightness(int delta) {
  int next = (int)brightnessStep + delta;
  if (next < 0) next = 0;
  if (next >= DISPLAY_BRIGHTNESS_STEPS) next = DISPLAY_BRIGHTNESS_STEPS - 1;
  if ((uint8_t)next == brightnessStep) {
    Serial.printf("Brightness is already at the limit: %s.\n", brightnessLabel(brightnessStep));
    return;
  }
  brightnessStep = (uint8_t)next;
  saveBrightnessSetting();
  applyBrightnessSetting(true);
  Serial.printf("Display brightness set to %s (%u/255) and saved.\n",
                brightnessLabel(brightnessStep), DISPLAY_BRIGHTNESS_LEVELS[brightnessStep]);
}

void toggleDisplayPower() {
  displayOn = !displayOn;
  if (!displayOn) {
    display->setBrightness(0);
    Serial.printf("PWR: display off; saved brightness is %s.\n", brightnessLabel(brightnessStep));
  } else {
    applyBrightnessSetting(false);
    Serial.printf("PWR: display on; restored brightness %s.\n", brightnessLabel(brightnessStep));
    renderActivePage();
  }
}

void pollPwrButton() {
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 90) return;
  lastPoll = now;

  uint8_t irqStatus = 0;
  if (!pmuRead(PMU_REG_POWER_KEY_IRQ_STATUS, irqStatus)) return;
  const uint8_t keyFlags = irqStatus & PMU_POWER_KEY_IRQ_MASK;
  if (keyFlags == 0) return;

  // AXP2101 status flags are write-1-to-clear. Clear only power-key flags.
  pmuWrite(PMU_REG_POWER_KEY_IRQ_STATUS, keyFlags);
  if (keyFlags & PMU_POWER_KEY_SHORT_IRQ) {
    toggleDisplayPower();
  }
}

// ------------------------------ Wi-Fi / weather ------------------------------
bool wifiConfigured() {
  return WIFI_SSID != nullptr && strlen(WIFI_SSID) > 0 &&
         strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

bool connectWiFi(uint32_t timeoutMs) {
  if (!wifiConfigured()) {
    Serial.println("Wi-Fi: set your SSID and password in the configuration first.");
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) return true;

  // First stop any previous attempt. WiFi.begin() must not be
  // called repeatedly while the ESP32 is still handling a connection attempt.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  delay(150);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Wi-Fi: connecting to SSID '%s'...\n", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(100);
    pollPwrButton();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.printf("Wi-Fi: connection timed out, status=%d. Check the 2.4 GHz network, password, and router security.\n",
                (int)WiFi.status());
  // Cancel the unfinished attempt; a new one starts at the next scheduled retry.
  WiFi.disconnect(false, false);
  return false;
}

const char *weatherDescription(int code) {
  if (code == 0) return "CLEAR";
  if (code == 1) return "MAINLY CLEAR";
  if (code == 2) return "PARTLY CLOUDY";
  if (code == 3) return "OVERCAST";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOWFALL";
  if (code >= 80 && code <= 82) return "RAIN SHOWERS";
  if (code == 85 || code == 86) return "SNOW SHOWERS";
  if (code >= 95 && code <= 99) return "THUNDERSTORM";
  return "WEATHER";
}

int weekdayFromIsoDate(const char *date) {
  if (date == nullptr || strlen(date) < 10) return 0;
  int y = atoi(date);
  int m = atoi(date + 5);
  int d = atoi(date + 8);
  static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) --y;
  return (y + y / 4 - y / 100 + y / 400 + offsets[m - 1] + d) % 7;
}

const char *weekdayShort(int weekday) {
  static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  if (weekday < 0 || weekday > 6) return "--";
  return names[weekday];
}

bool fetchWeather() {
  lastWeatherAttempt = millis();
  if (!connectWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("Wi-Fi is not connected; weather data could not be loaded.");
    return false;
  }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(LATITUDE, 4);
  url += "&longitude=";
  url += String(LONGITUDE, 4);
  url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,cloud_cover,wind_speed_10m,wind_direction_10m";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max";
  url += "&forecast_days=6&timezone=Europe%2FPrague&temperature_unit=celsius&wind_speed_unit=ms";

  WiFiClientSecure client;
  // Open-Meteo is a public endpoint without a token; this keeps TLS setup simple.
  // For deployments requiring certificate validation, replace setInsecure() with a CA certificate.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(14000);
  const char *responseHeaderKeys[] = {"Content-Type"};
  http.collectHeaders(responseHeaderKeys, 1);
  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed; could not start the HTTPS connection.");
    http.end();
    return false;
  }

  Serial.print("Open-Meteo URL: ");
  Serial.println(url);
  const int httpCode = http.GET();
  // Read the response body into a String so the Serial Monitor can help
  // distinguish an API response from an HTML login page or captive portal.
  const String contentType = http.header("Content-Type");
  const String payload = http.getString();
  http.end();

  Serial.printf("Open-Meteo HTTP: %d, Content-Type: %s, body length: %u B\n",
                httpCode, contentType.c_str(), (unsigned int)payload.length());
  if (httpCode != HTTP_CODE_OK) {
    Serial.println("Server response (first 300 characters):");
    Serial.println(payload.substring(0, 300));
    return false;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON error: %s. Response body (first 300 characters):\n", err.c_str());
    Serial.println(payload.substring(0, 300));
    Serial.println("If the body contains HTML or a login page, Wi-Fi may be connected to a network without internet access.");
    return false;
  }

  if (doc["error"] | false) {
    const char *reason = doc["reason"] | "unknown reason";
    Serial.printf("Open-Meteo returned an API error: %s\n", reason);
    return false;
  }

  JsonObject current = doc["current"].as<JsonObject>();
  JsonObject daily = doc["daily"].as<JsonObject>();
  if (current.isNull() || daily.isNull() || current["temperature_2m"].isNull()) {
    Serial.println("Open-Meteo response is missing expected data.");
    return false;
  }

  weather.temperature = current["temperature_2m"].as<float>();
  weather.apparentTemperature = current["apparent_temperature"] | weather.temperature;
  weather.humidity = current["relative_humidity_2m"] | NAN;
  weather.windSpeed = current["wind_speed_10m"] | NAN;
  weather.windDirection = current["wind_direction_10m"] | NAN;
  weather.cloudCover = current["cloud_cover"] | NAN;
  weather.precipitation = current["precipitation"] | NAN;
  weather.weatherCode = current["weather_code"] | -1;
  weather.isDay = current["is_day"] | 1;

  const char *apiTime = current["time"] | "";
  // Fallback time: measurement time provided by the server. After successful
  // NTP synchronization, this is replaced below with the local update completion time.
  if (strlen(apiTime) >= 16) {
    snprintf(weather.updateTime, sizeof(weather.updateTime), "%c%c:%c%c",
             apiTime[11], apiTime[12], apiTime[14], apiTime[15]);
  } else {
    snprintf(weather.updateTime, sizeof(weather.updateTime), "--:--");
  }

  JsonArray dates = daily["time"].as<JsonArray>();
  JsonArray codes = daily["weather_code"].as<JsonArray>();
  JsonArray maxTemps = daily["temperature_2m_max"].as<JsonArray>();
  JsonArray minTemps = daily["temperature_2m_min"].as<JsonArray>();
  JsonArray rain = daily["precipitation_probability_max"].as<JsonArray>();
  weather.dayCount = min((int)dates.size(), 6);
  for (int i = 0; i < weather.dayCount; ++i) {
    const char *dateText = dates[i] | "";
    snprintf(weather.days[i].date, sizeof(weather.days[i].date), "%s", dateText);
    weather.days[i].weatherCode = codes[i] | -1;
    weather.days[i].tMax = maxTemps[i] | NAN;
    weather.days[i].tMin = minTemps[i] | NAN;
    weather.days[i].precipProbability = rain[i] | -1;
  }

  // Time of the last successful fetch. Prefer the device clock
  // (NTP); if it is not synchronized yet, keep the fallback time from the API.
  const time_t nowEpoch = time(nullptr);
  if (nowEpoch > 1700000000) {
    struct tm localTimeInfo;
    if (localtime_r(&nowEpoch, &localTimeInfo) != nullptr) {
      snprintf(weather.updateTime, sizeof(weather.updateTime), "%02d/%02d %02d:%02d",
               localTimeInfo.tm_mon + 1, localTimeInfo.tm_mday,
               localTimeInfo.tm_hour, localTimeInfo.tm_min);
    }
  }

  weather.valid = true;
  Serial.printf("Weather updated: %s, %.1f C; last updated %s\n",
                CITY_NAME, weather.temperature, weather.updateTime);
  if (displayOn) renderActivePage();
  return true;
}

// ------------------------------ Drawing helpers ------------------------------
void drawBattery(int x, int y, int percent) {
  // Large, high-contrast battery icon (46 x 26 px body, about 52 px including the terminal).
  gfx->fillRoundRect(x, y, 46, 26, 5, C_PANEL2);
  if (percent >= 0 && percent <= 100) {
    const uint16_t fill = (percent <= 20) ? C_RED : ((percent <= 45) ? C_YELLOW : C_GREEN);
    const int barWidth = (percent * 38) / 100;
    if (barWidth > 0) gfx->fillRoundRect(x + 4, y + 4, barWidth, 18, 3, fill);
  } else {
    gfx->drawLine(x + 13, y + 13, x + 32, y + 13, C_MUTED);
  }
  // Draw the white battery outline last so it stays crisp and visible over the fill.
  gfx->drawRoundRect(x, y, 46, 26, 5, C_WHITE);
  gfx->drawRoundRect(x + 47, y + 8, 5, 10, 2, C_WHITE);
}

void drawSun(int cx, int cy, int size, uint16_t color) {
  const int core = max(5, size / 6);
  const int ring = core + max(2, size / 24);
  const int inner = core + max(5, size / 12);
  const int outer = inner + max(5, size / 10);

  gfx->fillCircle(cx, cy, core + 2, C_ORANGE);
  gfx->fillCircle(cx, cy, core, color);
  gfx->drawCircle(cx, cy, ring, color);
  for (int deg = 0; deg < 360; deg += 45) {
    const float a = deg * 0.01745329252f;
    const int x1 = cx + (int)(cosf(a) * inner);
    const int y1 = cy + (int)(sinf(a) * inner);
    const int x2 = cx + (int)(cosf(a) * outer);
    const int y2 = cy + (int)(sinf(a) * outer);
    gfx->drawLine(x1, y1, x2, y2, color);
    gfx->drawLine(x1 + (deg == 90 || deg == 270 ? 1 : 0), y1,
                  x2 + (deg == 90 || deg == 270 ? 1 : 0), y2, color);
  }
}

void drawCloud(int cx, int cy, int size, uint16_t color) {
  const int r = max(5, size / 5);
  gfx->fillRoundRect(cx - 2 * r, cy + 3, 4 * r, r + 4, r / 2, C_PANEL2);
  gfx->fillCircle(cx - r, cy + 3, r, C_PANEL2);
  gfx->fillCircle(cx, cy - r / 2 + 3, r + 2, C_PANEL2);
  gfx->fillCircle(cx + r, cy + 3, r, C_PANEL2);
  gfx->fillRoundRect(cx - 2 * r, cy, 4 * r, r + 4, r / 2, color);
  gfx->fillCircle(cx - r, cy, r, color);
  gfx->fillCircle(cx, cy - r / 2, r + 2, color);
  gfx->fillCircle(cx + r, cy, r, color);
}

void drawWeatherIcon(int cx, int cy, int size, int code, bool isDay) {
  const uint16_t cloudColor = C_CLOUD;
  const int r = max(6, size / 5);
  const int halo = max(18, size * 3 / 5);
  gfx->fillCircle(cx, cy, halo, C_PANEL);
  gfx->drawCircle(cx, cy, halo, C_PANEL2);

  if (code == 0) {
    drawSun(cx, cy, size, C_YELLOW);
    return;
  }
  if (code == 1 && isDay) {
    drawSun(cx - size / 5, cy - size / 6, size * 3 / 5, C_YELLOW);
    drawCloud(cx + size / 8, cy + size / 6, size * 3 / 5, cloudColor);
    return;
  }
  if (code == 1 || code == 2) {
    if (isDay) drawSun(cx - size / 5, cy - size / 6, size * 3 / 5, C_YELLOW);
    drawCloud(cx, cy + size / 10, size, cloudColor);
    return;
  }
  if (code == 45 || code == 48) {
    drawCloud(cx, cy - size / 8, size, cloudColor);
    for (int i = 0; i < 3; ++i) {
      const int yy = cy + size / 3 + i * max(4, size / 8);
      gfx->drawLine(cx - size / 3, yy, cx + size / 3, yy, C_MUTED);
      gfx->drawLine(cx - size / 3, yy + 1, cx + size / 3, yy + 1, C_MUTED);
    }
    return;
  }

  drawCloud(cx, cy - size / 8, size, cloudColor);
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    for (int i = -1; i <= 1; ++i) {
      const int x = cx + i * r;
      const int y = cy + size / 3 + (i == 0 ? 2 : 0);
      gfx->drawLine(x, y, x - 4, y + max(6, size / 9), C_BLUE);
      gfx->drawLine(x + 1, y, x - 3, y + max(6, size / 9), C_BLUE);
      gfx->fillCircle(x - 4, y + max(6, size / 9), max(2, size / 28), C_BLUE);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    for (int i = -1; i <= 1; ++i) {
      const int x = cx + i * r;
      const int y = cy + size / 3 + (i == 0 ? 4 : 0);
      const int arm = max(3, size / 16);
      gfx->drawLine(x - arm, y, x + arm, y, C_WHITE);
      gfx->drawLine(x, y - arm, x, y + arm, C_WHITE);
      gfx->drawLine(x - arm + 2, y - arm + 2, x + arm - 2, y + arm - 2, C_WHITE);
      gfx->drawLine(x - arm + 2, y + arm - 2, x + arm - 2, y - arm + 2, C_WHITE);
    }
  } else if (code >= 95 && code <= 99) {
    gfx->fillTriangle(cx + size / 10, cy + size / 5,
                      cx - size / 8, cy + size / 2,
                      cx + size / 20, cy + size / 2, C_YELLOW);
    gfx->fillTriangle(cx + size / 20, cy + size / 3,
                      cx - size / 7, cy + size / 2 + size / 8,
                      cx + size / 5, cy + size / 2 + size / 8, C_ORANGE);
  }
}

void drawStatusHeader(const char *title) {
  gfx->setTextWrap(false);

  // Row 1: screen title, prominent Wi-Fi status, and battery status.
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(2);
  const int titleX = (strcmp(title, CITY_NAME) == 0) ? 30 : 34;
  gfx->setCursor(titleX, 12);
  gfx->print(title);

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  gfx->fillRoundRect(150, 8, 132, 28, 7, C_PANEL2);
  gfx->drawRoundRect(150, 8, 132, 28, 7, wifiConnected ? C_GREEN : C_RED);
  gfx->fillCircle(162, 22, 5, wifiConnected ? C_GREEN : C_RED);
  gfx->setTextSize(2);
  gfx->setTextColor(wifiConnected ? C_GREEN : C_RED);
  gfx->setCursor(172, 14);
  gfx->print(wifiConnected ? "WI-FI OK" : "WI-FI OFF");

  gfx->setTextSize(2);
  gfx->setTextColor(C_CLOUD);
  gfx->setCursor(296, 14);
  gfx->print("BAT");
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(338, 14);
  if (batteryPercent >= 0) {
    gfx->print(batteryPercent);
    gfx->print('%');
  } else {
    gfx->print("--%");
  }
  // The icon is inset from the corner; its white outline is drawn in drawBattery().
  drawBattery(397, 14, batteryPercent);

  // Row 2: a clearly visible timestamp with an explicit label.
  gfx->setTextSize(2);
  gfx->setTextColor(C_CYAN);
  gfx->setCursor(22, 46);
  gfx->print("LAST UPDATED:");
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(280, 46);
  gfx->print(weather.valid ? weather.updateTime : "NOT YET");

  gfx->drawFastHLine(20, 68, 440, C_PANEL2);
}

void drawInfoCard(int x, int y, const char *label, const String &value, uint16_t accent) {
  gfx->fillRoundRect(x, y, 208, 78, 12, C_PANEL);
  gfx->drawRoundRect(x, y, 208, 78, 12, C_PANEL2);
  gfx->fillRoundRect(x + 12, y + 12, 4, 16, 2, accent);
  // The label uses text size 2 so it is readable on the 2.16-inch display.
  gfx->setTextSize(2);
  gfx->setTextColor(C_CLOUD);
  gfx->setCursor(x + 24, y + 12);
  gfx->print(label);
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(x + 14, y + 43);
  gfx->print(value);
}

String numberOrDash(float value, int decimals) {
  if (isnan(value) || isinf(value)) return "--";
  return String(value, decimals);
}

const char *windDirectionText(float deg) {
  if (isnan(deg)) return "";
  static const char *points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = ((int)floor((deg + 22.5f) / 45.0f)) % 8;
  return points[index];
}

void drawCurrentPage() {
  gfx->fillScreen(C_BG);
  drawStatusHeader(CITY_NAME);

  if (!weather.valid) {
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(28, 118);
    gfx->print("WAITING FOR WEATHER DATA");
    gfx->setTextColor(C_MUTED);
    gfx->setTextSize(1);
    gfx->setCursor(28, 154);
    gfx->print(wifiConfigured() ? "Waiting for data; retrying automatically." : "Set up Wi-Fi in the configuration first.");
    drawWeatherIcon(240, 222, 70, 2, true);
    drawInfoCard(22, 260, "HUMIDITY", "-- %", C_CYAN);
    drawInfoCard(250, 260, "WIND SPEED", "-- m/s", C_BLUE);
    drawInfoCard(22, 350, "CLOUD COVER", "-- %", C_MUTED);
    drawInfoCard(250, 350, "PRECIP. NOW", "-- mm", C_YELLOW);
  } else {
    const int temp = (int)lroundf(weather.temperature);
    gfx->setTextColor(C_CLOUD);
    gfx->setTextSize(2);
    gfx->setCursor(30, 77);
    gfx->print("CURRENT TEMP");
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(7);
    gfx->setCursor(28, 101);
    gfx->print(temp);
    const int tempDigits = (temp < 0) ? (int)String(-temp).length() + 1 : (int)String(temp).length();
    const int degreeX = 28 + tempDigits * 42 + 2;
    gfx->drawCircle(degreeX, 113, 5, C_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(degreeX + 10, 143);
    gfx->print('C');

    gfx->setTextSize(2);
    gfx->setTextColor(C_CYAN);
    gfx->setCursor(30, 190);
    gfx->print(weatherDescription(weather.weatherCode));
    gfx->setTextSize(2);
    gfx->setTextColor(C_CYAN);
    gfx->setCursor(30, 220);
    gfx->print("FEELS LIKE: ");
    gfx->print(numberOrDash(weather.apparentTemperature, 1));
    gfx->print(" C");

    drawWeatherIcon(365, 151, 108, weather.weatherCode, weather.isDay != 0);

    drawInfoCard(22, 260, "HUMIDITY", numberOrDash(weather.humidity, 0) + " %", C_CYAN);
    String wind = numberOrDash(weather.windSpeed, 1) + " m/s";
    const char *dir = windDirectionText(weather.windDirection);
    if (dir[0] != '\0') wind += String(" ") + dir;
    drawInfoCard(250, 260, "WIND SPEED", wind, C_BLUE);
    drawInfoCard(22, 350, "CLOUD COVER", numberOrDash(weather.cloudCover, 0) + " %", C_MUTED);
    drawInfoCard(250, 350, "PRECIP. NOW", numberOrDash(weather.precipitation, 1) + " mm", C_YELLOW);
  }

  // Page indicator and swipe hint
  gfx->fillCircle(232, 454, 4, C_CYAN);
  gfx->drawCircle(248, 454, 4, C_MUTED);
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(272, 449);
  gfx->print("SWIPE LEFT / RIGHT");
}

void drawForecastPage() {
  gfx->fillScreen(C_BG);
  drawStatusHeader("FORECAST");

  if (!weather.valid || weather.dayCount == 0) {
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(35, 155);
    gfx->print("FORECAST UNAVAILABLE");
    gfx->setTextSize(1);
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(35, 190);
    gfx->print("Check Wi-Fi; retrying automatically.");
  } else {
    for (int i = 0; i < weather.dayCount; ++i) {
      const int y = 72 + i * 64;
      gfx->fillRoundRect(18, y, 444, 56, 10, C_PANEL);
      gfx->drawRoundRect(18, y, 444, 56, 10, C_PANEL2);

      const char *dayLabel;
      if (i == 0) dayLabel = "TODAY";
      else if (i == 1) dayLabel = "TOMORROW";
      else dayLabel = weekdayShort(weekdayFromIsoDate(weather.days[i].date));

      gfx->setTextColor(C_WHITE);
      gfx->setTextSize(2);
      gfx->setCursor(32, y + 18);
      gfx->print(dayLabel);

      drawWeatherIcon(185, y + 27, 36, weather.days[i].weatherCode, true);

      gfx->setTextSize(1);
      gfx->setTextColor(C_MUTED);
      gfx->setTextSize(2);
      gfx->setTextColor(C_CLOUD);
      gfx->setCursor(246, y + 4);
      gfx->print("HIGH/LOW");
      gfx->setCursor(386, y + 4);
      gfx->print("RAIN");

      gfx->setTextSize(2);
      gfx->setTextColor(C_WHITE);
      gfx->setCursor(246, y + 26);
      gfx->print((int)lroundf(weather.days[i].tMax));
      gfx->setTextColor(C_MUTED);
      gfx->print("/");
      gfx->setTextColor(C_WHITE);
      gfx->print((int)lroundf(weather.days[i].tMin));
      gfx->setTextSize(1);
      gfx->setTextColor(C_MUTED);
      gfx->setCursor(332, y + 31);
      gfx->print("C");

      gfx->setTextSize(2);
      gfx->setTextColor(C_CYAN);
      gfx->setCursor(386, y + 26);
      if (weather.days[i].precipProbability >= 0) {
        gfx->print(weather.days[i].precipProbability);
        gfx->print('%');
      } else {
        gfx->print("--");
      }
    }
  }

  gfx->fillCircle(232, 454, 4, C_MUTED);
  gfx->drawCircle(248, 454, 4, C_CYAN);
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(272, 449);
  gfx->print("SWIPE LEFT / RIGHT");
}

void renderActivePage() {
  if (!displayOn) return;
  if (activePage == 0) drawCurrentPage();
  else drawForecastPage();
  // Arduino_Canvas rotates the entire interface 90 degrees clockwise in software.
  gfx->flush();
}

// ------------------------------ Controls ------------------------------
void pollKeyButton() {
  // +/KEY (GPIO18): increase brightness by one step.
  const uint32_t now = millis();
  const bool raw = digitalRead(KEY_BUTTON);
  if (raw != keyRawLast) {
    keyRawLast = raw;
    keyDebounceAt = now;
  }
  if (now - keyDebounceAt > 35 && raw != keyStable) {
    keyStable = raw;
    if (keyStable == LOW) {
      Serial.println("+/KEY: increase brightness.");
      adjustBrightness(+1);
    }
  }
}

void pollBootMinusButton() {
  // BOOT/- (GPIO0): decrease brightness by one step. On some board revisions, this button
  // may also pull CHIP_PU/reset low. If pressing it restarts the board,
  // this pin cannot be used as a normal runtime button on that revision.
  const uint32_t now = millis();
  const bool raw = digitalRead(BOOT_BUTTON);
  if (raw != bootRawLast) {
    bootRawLast = raw;
    bootDebounceAt = now;
  }
  if (now - bootDebounceAt > 35 && raw != bootStable) {
    bootStable = raw;
    if (bootStable == LOW) {
      Serial.println("BOOT/-: decrease brightness.");
      adjustBrightness(-1);
    }
  }
}

void pollTouch() {
  if (!touchAvailable) return;
  const uint32_t now = millis();
  if (now - lastTouchPoll < 30) return;
  lastTouchPoll = now;

  const uint8_t count = touch.getPoint(touchX, touchY, touch.getSupportTouchPoint());
  if (count > 0) {
    // The image is rotated clockwise (x', y') = (479 - y, x); apply the same transform to touch.
    const int16_t rawX = touchX[0];
    const int16_t rawY = touchY[0];
    touchX[0] = (LCD_WIDTH - 1) - rawY;
    touchY[0] = rawX;
    if (!touchFingerDown) {
      touchFingerDown = true;
      touchStartAt = millis();
      touchStartX = touchX[0];
      touchStartY = touchY[0];
    }
    touchLastX = touchX[0];
    touchLastY = touchY[0];
  } else if (touchFingerDown) {
    const int dx = touchLastX - touchStartX;
    const int dy = touchLastY - touchStartY;
    const uint32_t pressDuration = millis() - touchStartAt;
    if (abs(dx) > 70 && abs(dx) > abs(dy)) {
      const int newPage = (dx < 0) ? 1 : 0;
      if (newPage != activePage) {
        activePage = newPage;
        renderActivePage();
      }
    } else if (abs(dx) < 35 && abs(dy) < 35 && pressDuration <= 900) {
      // A short tap that is not a swipe manually refreshes the current weather.
      Serial.println("Touch: manually refreshing weather.");
      if (displayOn) {
        gfx->setTextColor(C_CYAN);
        gfx->setTextSize(2);
        gfx->fillRoundRect(90, 427, 300, 30, 8, C_PANEL);
        gfx->setCursor(110, 434);
        gfx->print("UPDATING WEATHER DATA...");
        gfx->flush();
      }
      fetchWeather();
      if (displayOn) renderActivePage();
    }
    touchFingerDown = false;
  }
}

// ------------------------------ Arduino setup/loop ------------------------------
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.printf("\n[WEATHER] Running build: %s\n", APP_BUILD_TAG);

  pinMode(KEY_BUTTON, INPUT_PULLUP);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  brightnessPrefsReady = brightnessPrefs.begin("weatherui", false);
  if (brightnessPrefsReady) {
    brightnessStep = brightnessPrefs.getUChar("bright", DEFAULT_BRIGHTNESS_STEP);
    if (brightnessStep >= DISPLAY_BRIGHTNESS_STEPS) brightnessStep = DEFAULT_BRIGHTNESS_STEP;
    Serial.printf("Loaded brightness: %s (index %u).\n", brightnessLabel(brightnessStep), brightnessStep);
  } else {
    brightnessStep = DEFAULT_BRIGHTNESS_STEP;
    Serial.println("Warning: could not open NVS; brightness settings will not be saved.");
  }

  if (!gfx->begin()) {
    Serial.println("ERROR: display/canvas initialization failed. Check PSRAM in the board settings.");
    while (true) delay(1000);
  }
  // Keep the panel hardware configuration unchanged; the software canvas handles 90-degree rotation.
  bus->writeC8D8(0x36, 0xA0);
  gfx->fillScreen(C_BG);
  display->setBrightness(DISPLAY_BRIGHTNESS_LEVELS[brightnessStep]);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(28, 175);
  gfx->print("WEATHER - STARTING");
  gfx->flush();

  touch.setPins(TP_RESET, TP_INT);
  touchAvailable = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchAvailable) {
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
  } else {
    Serial.println("Warning: CST9220 touch initialization failed.");
  }

  // Clear any old PMU power-key interrupt flags.
  uint8_t oldIrq = 0;
  if (pmuRead(PMU_REG_POWER_KEY_IRQ_STATUS, oldIrq)) {
    const uint8_t oldKeyFlags = oldIrq & PMU_POWER_KEY_IRQ_MASK;
    if (oldKeyFlags) pmuWrite(PMU_REG_POWER_KEY_IRQ_STATUS, oldKeyFlags);
  }
  readBattery();

  // Wi-Fi is started only by connectWiFi(), which is called by fetchWeather().
  // Do not call WiFi.begin() again while a connection attempt is in progress.

  // The API returns local time for Europe/Prague; NTP is also configured as a fallback.
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  fetchWeather();
  lastUiWifiStatus = WiFi.status();
  renderActivePage();
}

void loop() {
  pollPwrButton();
  pollKeyButton();
  pollBootMinusButton();
  pollTouch();

  const wl_status_t currentWifiStatus = WiFi.status();
  if (currentWifiStatus != lastUiWifiStatus) {
    lastUiWifiStatus = currentWifiStatus;
    Serial.printf("Wi-Fi status changed: %s\n", currentWifiStatus == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    if (displayOn) renderActivePage();
  }

  const uint32_t now = millis();
  if (now - lastBatteryRead >= 30000UL) {
    lastBatteryRead = now;
    readBattery();
    if (displayOn) renderActivePage();
  }

  const uint32_t retryInterval = weather.valid ? WEATHER_REFRESH_MS : RETRY_REFRESH_MS;
  if (now - lastWeatherAttempt >= retryInterval) {
    fetchWeather();
    // Data may change when fetchWeather() returns; redraw even if the request failed.
    if (displayOn) renderActivePage();
  }

  // Wi-Fi retries happen alongside scheduled weather refreshes
  // (every 30 seconds on failure), not on every loop() iteration.

  delay(5);
}
