// Weather Alert Display — rebuild
// Board:    ESP32-WROOM-32 dev board (D-numbered silkscreen, RX2/TX2 = GPIO16/17)
// Display:  1.8" TFT, 128x160, ST7735-family, SPI
// Weather:  National Weather Service (api.weather.gov), free, no API key,
//           for Jonesboro, AR. Polls current conditions, forecast, and active
//           severe weather alerts (watches/warnings).
//
// This replaces a board whose original firmware was lost. It recreates the
// same Home Assistant entities the old one published (see the SENSORS list
// below) so the existing "Weather" dashboard keeps working unchanged:
//   binary_sensor.weather_alert_display_severe_weather
//   sensor.weather_alert_display_{temperature,feels_like,condition,
//     high_today,low_tonight,weather_alert,alert_count,dew_point,humidity,
//     pressure,wind_speed,wind_direction}
//
// Libraries (installed automatically by PlatformIO, see platformio.ini):
//   - GFX Library for Arduino by moononournation (TFT driver)
//   - ArduinoJson, PubSubClient (MQTT)
//
// Wiring (TFT, SPI):
//   VCC->VIN(5V)  GND->GND  SCL->D18  SDA->D23  RES->D4  DC->RX2
//   CS->D5  BLK->3V3
// (RX2 is this board's label for GPIO16, the second hardware UART, unused
// by this sketch, so safe to reuse as a plain GPIO. RES was originally on
// TX2/GPIO17 but that pin is bad on this specific ESP32 — confirmed
// 2026-10-05 by swapping it to GPIO4, which fixed a solid-white, never-
// initializes TFT across both a soldered perfboard and a breadboard build.)

#include <ctype.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <PubSubClient.h>
#include <Fonts/FreeSansBold24pt7b.h>  // smooth big digits for the temperature

// ---------- Settings you edit ----------
#include "secrets.h"  // Wi-Fi and MQTT logins (kept out of GitHub)

const char* CITY_NAME = "JONESBORO, AR";
const float LATITUDE  = 35.8423;
const float LONGITUDE = -90.7043;

// Looked up once from https://api.weather.gov/points/{lat},{lon}. If you
// change LATITUDE/LONGITUDE, look these up again the same way.
const char* NWS_USER_AGENT = "weather-alert-display (github.com/booherjr707-code)";
const char* NWS_GRID_ID = "MEG";
const int   NWS_GRID_X  = 17;
const int   NWS_GRID_Y  = 97;
const char* NWS_STATION = "KJBR";  // Jonesboro Municipal Airport

const uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;  // conditions + forecast
const uint32_t ALERT_REFRESH_MS   = 2UL  * 60UL * 1000UL;  // severe weather alerts, checked more often
const uint32_t WIFI_TIMEOUT_MS = 20000;

// The header rotates between the city name and the next three days' forecast.
// While a severe alert is active, this is replaced by a flashing alert banner.
const uint32_t HEADER_SLOT_SECS = 3;  // seconds each item stays up

// US Central time, switches to daylight saving automatically.
const char* TIMEZONE = "CST6CDT,M3.2.0,M11.1.0";

// TFT pins (SPI). See wiring comment above.
#define TFT_SCLK  18
#define TFT_MOSI  23
#define TFT_CS    5
#define TFT_DC    16   // RX2
#define TFT_RST   4   // GPIO17 (TX2) on this ESP32 is bad — moved RES to GPIO4

// Display settings. The startup screen shows RED, GREEN, BLUE, YELLOW bars:
//   - red and blue swapped?       flip PANEL_BGR
//   - colors look like a negative? flip PANEL_INVERT
//   - picture upside down?        change ROTATION from 1 to 3
//   - a line of junk on one edge? set the offsets to 2 and 1
const bool PANEL_BGR    = false;
const bool PANEL_INVERT = false;
const uint8_t ROTATION  = 1;
const uint8_t COL_OFFSET = 2;
const uint8_t ROW_OFFSET = 1;
// ---------------------------------------

const int SCREEN_W = 160;
const int SCREEN_H = 128;

// Vertical barometer gauge, right edge. A white line across the middle marks
// PRESSURE_GAUGE_CENTER ("even"); a blue fill + arrow move up above it for
// high pressure, down below it for low pressure, pinning at the top/bottom
// once the reading is PRESSURE_GAUGE_RANGE inHg away from center.
const float PRESSURE_GAUGE_CENTER = 29.92f;  // standard sea-level pressure
const float PRESSURE_GAUGE_RANGE  = 0.5f;    // inHg from center to a pinned top/bottom
const int   GAUGE_W      = 9;
const int   GAUGE_X      = SCREEN_W - GAUGE_W - 1;  // 1px margin at the screen edge
const int   GAUGE_TOP    = 14;                       // just below the header rule
const int   GAUGE_BOTTOM = SCREEN_H - 2;

// Everything else (header text, condition/H-L) stays clear of the gauge
// column by respecting this as their right-hand boundary.
const int CONTENT_RIGHT = GAUGE_X - 2;

// The header row (y 0-12) sits above the gauge (which starts at GAUGE_TOP),
// so the clock/Wi-Fi bars/fail-mark can use the full screen width instead of
// stopping at CONTENT_RIGHT — packed into the far right corner to leave the
// rotating header text (city/forecast) as much room as possible.
const int HEADER_RIGHT  = SCREEN_W - 2;
const int WIFI_X        = HEADER_RIGHT - 14;  // drawWiFiBars occupies [x, x+14]
const int FAIL_MARK_X   = WIFI_X - 8;
const int CLOCK_RIGHT   = FAIL_MARK_X - 2;

// Everything is drawn into a memory canvas first and sent to the screen in
// one go, so it doesn't flicker.
Arduino_DataBus* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, VSPI);
Arduino_GFX* panel = new Arduino_ST7735(bus, TFT_RST, ROTATION, PANEL_INVERT, 128, 160,
                                        COL_OFFSET, ROW_OFFSET, COL_OFFSET, ROW_OFFSET, PANEL_BGR);
Arduino_Canvas* gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);

// Colors (RGB565)
const uint16_t C_BLACK   = 0x0000;
const uint16_t C_WHITE   = 0xFFFF;
const uint16_t C_GRAY    = 0x8410;
const uint16_t C_DKGRAY  = 0x2104;
const uint16_t C_NAVY    = 0x0010;
const uint16_t C_RED     = 0xF800;
const uint16_t C_ORANGE  = 0xFC00;
const uint16_t C_YELLOW  = 0xFFE0;
const uint16_t C_GREEN   = 0x07E0;
const uint16_t C_BLUE    = 0x001F;
const uint16_t C_CYAN    = 0x07FF;
const uint16_t C_TEMPBLUE = C_NAVY;  // matches the header bar's blue exactly, per request

struct Weather {
  bool  valid = false;
  float temp = 0, feelsLike = 0, wind = 0, dewPoint = 0, pressureInHg = 0;
  float precip3hIn = -1;  // inches in the last 3h; -1 = station isn't reporting it
  int   high = 0, low = 0;
  int   humidity = 0;
  char  condition[24] = "";
  char  windDir[4] = "";
};

struct ForecastDay {
  char name[8] = "";
  char cond[20] = "";
  int  high = 0, low = 0;
};

struct Alert {
  bool active = false;
  int  severityRank = 0;   // 0 none, 1 unknown/minor .. 5 extreme
  int  count = 0;          // how many active alerts right now
  char event[36] = "";     // most urgent one
};

// Rain called out for the current/next NWS forecast period (broad — NWS's
// free API has no minute-level rain timing; see RainInfo.active threshold).
struct RainInfo {
  bool  active = false;        // this period is worth a dedicated screen for
  char  periodName[16] = "";   // e.g. "Today", "Tonight", "This Afternoon"
  char  forecastText[24] = ""; // e.g. "Rain Likely"
  int   chancePercent = -1;    // -1 = not reported
};

Weather weather;
ForecastDay forecast[3];
bool forecastValid = false;
RainInfo rain;
Alert alert;
bool lastFetchFailed = false;
bool lastAlertFetchFailed = false;
uint32_t lastFetch = 0;
uint32_t lastAlertFetch = 0;

int severityRank(const char* sev) {
  if (strcmp(sev, "Extreme")  == 0) return 5;
  if (strcmp(sev, "Severe")   == 0) return 4;
  if (strcmp(sev, "Moderate") == 0) return 3;
  if (strcmp(sev, "Minor")    == 0) return 2;
  return 1;
}

// Turns a compass bearing in degrees into a 16-point abbreviation.
const char* compassPoint(float deg) {
  static const char* points[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                  "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  int i = (int)((deg / 22.5f) + 0.5f) % 16;
  if (i < 0) i += 16;
  return points[i];
}

// Case-insensitive substring check (strcasestr isn't reliably available in
// the ESP32 Arduino toolchain).
bool containsCI(const char* haystack, const char* needle) {
  char h[48], n[16];
  strlcpy(h, haystack, sizeof(h));
  strlcpy(n, needle, sizeof(n));
  for (char* c = h; *c; c++) *c = tolower(*c);
  for (char* c = n; *c; c++) *c = tolower(*c);
  return strstr(h, n) != nullptr;
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawWiFiBars(int x, int bottom) {
  int bars = wifiBars();
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2;
    if (i < bars) gfx->fillRect(x + i * 4, bottom - h, 3, h, C_WHITE);
    else          gfx->drawFastHLine(x + i * 4, bottom - 1, 3, C_GRAY);
  }
}

int textWidth(const char* s, int size) { return strlen(s) * 6 * size; }

void drawText(int x, int y, const char* s, uint16_t color, int size = 1) {
  gfx->setFont((const GFXfont*)nullptr);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void drawCentered(int y, const char* s, uint16_t color, int size = 1) {
  drawText((SCREEN_W - textWidth(s, size)) / 2, y, s, color, size);
}

void drawRight(int right, int y, const char* s, uint16_t color, int size = 1) {
  drawText(right - textWidth(s, size), y, s, color, size);
}

void showMessage(const char* line1, const char* line2 = "", uint16_t color1 = C_WHITE) {
  gfx->fillScreen(C_BLACK);
  drawCentered(48, line1, color1, 1);
  drawCentered(64, line2, C_GRAY, 1);
  gfx->flush();
}

void showSplash() {
  gfx->fillScreen(C_BLACK);
  drawCentered(8, "WEATHER", C_YELLOW, 2);
  drawCentered(28, "ALERT DISPLAY", C_WHITE, 1);
  const uint16_t bars[] = {C_RED, C_GREEN, C_BLUE, C_YELLOW};
  const char* names[] = {"RED", "GREEN", "BLUE", "YELLOW"};
  for (int i = 0; i < 4; i++) {
    gfx->fillRect(0, 44 + i * 18, SCREEN_W, 18, bars[i]);
    drawCentered(49 + i * 18, names[i], i == 3 ? C_BLACK : C_WHITE, 1);
  }
  drawCentered(118, "Starting...", C_GRAY, 1);
  gfx->flush();
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  showMessage("Connecting WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Fetches live current conditions from the nearest NWS observation station.
bool fetchConditions() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/stations/") + NWS_STATION + "/observations/latest";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Conditions HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Conditions JSON error: %s\n", err.c_str());
    return false;
  }

  JsonObject p = doc["properties"];
  float tempC = p["temperature"]["value"] | NAN;
  if (isnan(tempC)) return false;

  float feelsC = p["heatIndex"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = p["windChill"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = tempC;

  float dewC = p["dewpoint"]["value"] | tempC;
  float pressurePa = p["barometricPressure"]["value"] | NAN;
  float windKmh  = p["windSpeed"]["value"] | 0.0f;
  float windDeg  = p["windDirection"]["value"] | NAN;
  float humidity = p["relativeHumidity"]["value"] | 0.0f;
  float precip3hMm = p["precipitationLast3Hours"]["value"] | NAN;
  const char* desc = p["textDescription"] | "Unknown";

  weather.temp         = tempC * 9.0f / 5.0f + 32.0f;
  weather.feelsLike     = feelsC * 9.0f / 5.0f + 32.0f;
  weather.dewPoint      = dewC * 9.0f / 5.0f + 32.0f;
  weather.pressureInHg  = isnan(pressurePa) ? 0.0f : pressurePa / 3386.39f;
  weather.precip3hIn    = isnan(precip3hMm) ? -1.0f : precip3hMm / 25.4f;
  weather.humidity      = (int)roundf(humidity);
  weather.wind          = windKmh * 0.621371f;
  strlcpy(weather.windDir, isnan(windDeg) ? "--" : compassPoint(windDeg), sizeof(weather.windDir));
  strlcpy(weather.condition, desc, sizeof(weather.condition));
  weather.valid = true;

  Serial.printf("%.0fF, %s\n", weather.temp, weather.condition);
  return true;
}

// Fetches today's high/low plus the next 3 days from the NWS gridpoint
// forecast. Each day has a daytime and nighttime period; we pair them up by
// date to get one high/low/condition per day.
bool fetchForecast() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/gridpoints/") + NWS_GRID_ID + "/" +
               NWS_GRID_X + "," + NWS_GRID_Y + "/forecast";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Forecast HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Forecast JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray periods = doc["properties"]["periods"];

  // Rain called out for the current/next period — broad NWS data, no
  // minute-level timing (see the RainInfo struct for why).
  rain.active = false;
  if (periods.size() > 0) {
    JsonObject p0 = periods[0];
    strlcpy(rain.periodName, p0["name"] | "", sizeof(rain.periodName));
    const char* shortFc = p0["shortForecast"] | "";
    strlcpy(rain.forecastText, shortFc, sizeof(rain.forecastText));
    rain.chancePercent = p0["probabilityOfPrecipitation"]["value"] | -1;
    bool mentionsRain = containsCI(shortFc, "rain") || containsCI(shortFc, "shower") ||
                         containsCI(shortFc, "storm") || containsCI(shortFc, "drizzle");
    rain.active = (rain.chancePercent >= 30) || mentionsRain;
  }

  bool gotHigh = false, gotLow = false;
  int dayCount = -1;         // -1 before first period; 0 = today; 1..3 = the header days
  char lastDate[11] = "";

  for (JsonObject p : periods) {
    const char* start = p["startTime"] | "";
    char date[11] = "";
    strncpy(date, start, 10);
    date[10] = 0;
    bool isDay = p["isDaytime"] | false;
    int  temp  = p["temperature"] | 0;
    const char* cond = p["shortForecast"] | "";

    // Today's high is the first daytime reading we see; tonight's low is the
    // first nighttime reading, regardless of which "day" they fall under.
    if (!gotHigh && isDay)  { weather.high = temp; gotHigh = true; }
    if (!gotLow  && !isDay) { weather.low  = temp; gotLow  = true; }

    if (strcmp(date, lastDate) != 0) {
      dayCount++;
      strlcpy(lastDate, date, sizeof(lastDate));
      if (dayCount > 3) break;
      if (dayCount >= 1) {
        ForecastDay& d = forecast[dayCount - 1];
        d.high = 0;
        d.low  = 0;
        d.cond[0] = 0;
        struct tm t = {};
        int y, m, day;
        if (sscanf(date, "%d-%d-%d", &y, &m, &day) == 3) {
          t.tm_year = y - 1900;
          t.tm_mon  = m - 1;
          t.tm_mday = day;
          t.tm_hour = 12;
          mktime(&t);  // fills in the weekday
          strftime(d.name, sizeof(d.name), "%a", &t);
        } else {
          strlcpy(d.name, "---", sizeof(d.name));
        }
      }
    }

    if (dayCount >= 1 && dayCount <= 3) {
      ForecastDay& d = forecast[dayCount - 1];
      if (isDay) {
        d.high = temp;
        strlcpy(d.cond, cond, sizeof(d.cond));
      } else {
        d.low = temp;
        if (!d.cond[0]) strlcpy(d.cond, cond, sizeof(d.cond));
      }
    }
  }

  forecastValid = (dayCount >= 3);
  return true;
}

// Fetches active NWS alerts (watches/warnings/advisories) for this point.
bool fetchAlerts() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/alerts/active?point=") +
               String(LATITUDE, 4) + "," + String(LONGITUDE, 4);
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Alert HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Alert JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray features = doc["features"];
  int bestRank = 0;
  const char* bestEvent = "Alert";
  int count = 0;
  for (JsonObject f : features) {
    JsonObject p = f["properties"];
    count++;
    int rank = severityRank(p["severity"] | "Unknown");
    if (rank > bestRank) {
      bestRank = rank;
      bestEvent = p["event"] | "Alert";
    }
  }

  bool wasActive = alert.active;
  alert.active = bestRank > 0;
  alert.severityRank = bestRank;
  alert.count = count;
  if (alert.active) {
    strlcpy(alert.event, bestEvent, sizeof(alert.event));
    if (!wasActive) Serial.printf("ALERT: %s\n", alert.event);
  } else {
    alert.event[0] = 0;
    if (wasActive) Serial.println("Alert cleared");
  }
  return true;
}

void drawDegree(int x, int y, int r = 2, uint16_t color = C_WHITE) {
  gfx->drawCircle(x, y, r, color);
}

void drawMarquee(int y, const char* text, int boxW) {
  int textW = textWidth(text, 1);
  if (textW <= boxW) {
    drawText(2, y, text, C_BLACK, 1);
    return;
  }
  const int gap = 20;
  int totalW = textW + gap;
  int offset = (millis() / 40) % totalW;
  int x = -offset;
  drawText(x, y, text, C_BLACK, 1);
  if (x + totalW < boxW) drawText(x + totalW, y, text, C_BLACK, 1);
}

// Replaces the header with a blinking, scrolling banner naming the most
// urgent active alert, e.g. "!! SEVERE THUNDERSTORM WARNING".
void drawAlertBanner() {
  bool flash = (millis() / 500) % 2 == 0;
  gfx->fillRect(0, 0, SCREEN_W, 12, flash ? C_RED : C_YELLOW);
  char banner[40];
  snprintf(banner, sizeof(banner), "%s %s", flash ? "!!" : "  ", alert.event);
  drawMarquee(2, banner, SCREEN_W);
}

void formatClock(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 0)) {
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, len, "%d:%02d", h, t.tm_min);
  } else {
    snprintf(buf, len, "--:--");
  }
}

// Header text: cycles through the city name and the next three days, e.g.
// "Tue 93/74 Sunny". The condition is dropped if it won't fit before the
// clock.
void drawHeaderText() {
  const char* text = CITY_NAME;
  char buf[28];
  uint32_t slot = (millis() / 1000 / HEADER_SLOT_SECS) % 4;
  if (forecastValid && slot > 0) {
    const ForecastDay& d = forecast[slot - 1];
    snprintf(buf, sizeof(buf), "%s %d/%d %s", d.name, d.high, d.low, d.cond);
    int maxW = CLOCK_RIGHT - 32 - 2;
    if (textWidth(buf, 1) > maxW) snprintf(buf, sizeof(buf), "%s %d/%d", d.name, d.high, d.low);
    text = buf;
  }
  drawText(2, 2, text, C_YELLOW, 1);
}

void drawHeader() {
  if (alert.active) {
    drawAlertBanner();
  } else {
    gfx->fillRect(0, 0, SCREEN_W, 12, C_NAVY);
    drawHeaderText();
    char clock[8];
    formatClock(clock, sizeof(clock));
    drawRight(CLOCK_RIGHT, 2, clock, C_WHITE, 1);
    drawWiFiBars(WIFI_X, 10);
    if (lastFetchFailed) drawText(FAIL_MARK_X, 2, "!", C_RED, 1);
  }
  gfx->drawFastHLine(0, 12, SCREEN_W, C_GRAY);
}

// Draws the barometer gauge. See the PRESSURE_GAUGE_*/GAUGE_* constants
// above for the center value, range, and position.
void drawPressureGauge() {
  if (weather.pressureInHg <= 0) return;  // no reading yet

  gfx->drawRect(GAUGE_X, GAUGE_TOP, GAUGE_W, GAUGE_BOTTOM - GAUGE_TOP, C_GRAY);

  int centerY = (GAUGE_TOP + GAUGE_BOTTOM) / 2;
  float delta = weather.pressureInHg - PRESSURE_GAUGE_CENTER;
  float frac  = constrain(delta / PRESSURE_GAUGE_RANGE, -1.0f, 1.0f);
  int halfTrack = (GAUGE_BOTTOM - GAUGE_TOP) / 2 - 3;  // keep the arrow inside the border
  int pointerY  = centerY - (int)roundf(frac * halfTrack);

  // Blue fill from the center line toward the pointer shows the magnitude.
  int fillTop = min(centerY, pointerY);
  int fillH   = abs(centerY - pointerY) + 1;
  gfx->fillRect(GAUGE_X + 1, fillTop, GAUGE_W - 2, fillH, C_BLUE);

  // White center line = baseline/"even" pressure, drawn after the fill so it
  // stays visible even when the reading sits right at center.
  gfx->drawFastHLine(GAUGE_X, centerY, GAUGE_W, C_WHITE);

  // Small arrow pointing at the current reading, just left of the track.
  gfx->fillTriangle(GAUGE_X - 1, pointerY,
                     GAUGE_X - 6, pointerY - 3,
                     GAUGE_X - 6, pointerY + 3, C_CYAN);
}

// A dedicated takeover screen for when rain.active — broad NWS period data
// only (name, forecast text, chance %), no minute-level timing. See RainInfo.
void drawRainScreen() {
  char buf[28];
  gfx->fillScreen(C_NAVY);
  drawCentered(4, "RAIN IN THE FORECAST", C_WHITE, 1);
  gfx->drawFastHLine(0, 16, SCREEN_W, C_GRAY);

  drawCentered(30, rain.periodName, C_YELLOW, 1);
  drawCentered(46, rain.forecastText, C_CYAN, 1);

  if (rain.chancePercent >= 0) snprintf(buf, sizeof(buf), "%d%% chance", rain.chancePercent);
  else snprintf(buf, sizeof(buf), "chance: no data");
  drawCentered(64, buf, C_WHITE, 1);

  if (weather.precip3hIn >= 0) snprintf(buf, sizeof(buf), "Last 3h: %.2f in", weather.precip3hIn);
  else snprintf(buf, sizeof(buf), "Last 3h: no data");
  drawCentered(80, buf, C_GRAY, 1);

  drawCentered(110, "(back to weather shortly)", C_GRAY, 1);
  gfx->flush();
}

void drawWeather() {
  char buf[32];
  gfx->fillScreen(C_BLACK);
  drawHeader();

  // Big temperature, left — real font instead of the scaled-up default
  // bitmap font, which looked blocky at this size.
  const int TEMP_TOP = 14;  // where we want the glyph's visual top to land
  int w = 0, degX = 0, degY = 0;
  {
    snprintf(buf, sizeof(buf), "%d", (int)roundf(weather.temp));
    gfx->setFont(&FreeSansBold24pt7b);
    gfx->setTextColor(C_TEMPBLUE);
    int16_t x1, y1;
    uint16_t tw, th;
    gfx->getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    int baselineY = TEMP_TOP - y1;
    gfx->setCursor(4 - x1, baselineY);
    gfx->print(buf);
    w = tw;
    degX = 4 + w + 8;
    degY = TEMP_TOP + 5;
  }
  gfx->setFont((const GFXfont*)nullptr);
  drawDegree(degX, degY, 3, C_TEMPBLUE);
  drawText(degX + 10, TEMP_TOP, "F", C_TEMPBLUE, 2);

  // Condition, right of the temperature
  const char* cond = weather.condition;
  int cw = textWidth(cond, 1);
  drawText(max(4 + w + 30, CONTENT_RIGHT - cw), 16, cond, C_CYAN, 1);

  snprintf(buf, sizeof(buf), "H%d L%d", weather.high, weather.low);
  drawText(max(4 + w + 30, CONTENT_RIGHT - textWidth(buf, 1)), 30, buf, C_GRAY, 1);

  gfx->drawFastHLine(0, 50, SCREEN_W, C_DKGRAY);

  // Details grid
  snprintf(buf, sizeof(buf), "Feels %d", (int)roundf(weather.feelsLike));
  drawText(4, 56, buf, C_TEMPBLUE, 1);
  snprintf(buf, sizeof(buf), "Hum %d%%", weather.humidity);
  drawText(84, 56, buf, C_WHITE, 1);

  snprintf(buf, sizeof(buf), "Dew %d", (int)roundf(weather.dewPoint));
  drawText(4, 70, buf, C_WHITE, 1);
  if (weather.pressureInHg > 0) {
    snprintf(buf, sizeof(buf), "%.2f in", weather.pressureInHg);
    drawText(84, 70, buf, C_WHITE, 1);
  }

  snprintf(buf, sizeof(buf), "Wind %s %d", weather.windDir, (int)roundf(weather.wind));
  drawText(4, 84, buf, C_WHITE, 1);

  gfx->drawFastHLine(0, 98, SCREEN_W, C_DKGRAY);

  if (alert.count > 0) {
    snprintf(buf, sizeof(buf), "%d active alert%s", alert.count, alert.count == 1 ? "" : "s");
    drawCentered(104, buf, C_RED, 1);
  } else {
    // Combine with the current condition, e.g. "No active alerts - Sunny".
    // Trim the condition character by character if it's too wide to fit.
    char combined[48];
    char cond[24];
    strlcpy(cond, weather.condition, sizeof(cond));
    snprintf(combined, sizeof(combined), "No active alerts - %s", cond);
    while (textWidth(combined, 1) > SCREEN_W - 4 && strlen(cond) > 0) {
      cond[strlen(cond) - 1] = 0;
      snprintf(combined, sizeof(combined), "No active alerts - %s", cond);
    }
    drawCentered(104, combined, C_GREEN, 1);
  }

  // One more line, same size, right below: a quick rain heads-up (e.g. "Rain
  // This Afternoon") whenever rain.active — blank otherwise. This is the
  // persistent one-liner; the full rain takeover screen (drawRainScreen)
  // still shows separately with more detail.
  if (rain.active) {
    char rainBuf[32];
    snprintf(rainBuf, sizeof(rainBuf), "Rain %s", rain.periodName);
    drawCentered(116, rainBuf, C_CYAN, 1);
  }

  drawPressureGauge();
  gfx->flush();
}

// ---------- Home Assistant (MQTT) ----------
// Publishes one JSON "state" message with every reading, plus discovery
// messages so the sensors appear in Home Assistant automatically. Each
// discovery message sets "object_id" explicitly so the entity_id always
// comes out as sensor.weather_alert_display_<key>, matching the dashboard
// built for the original (lost) firmware.

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
const char* DEVICE_ID = "weather_alert_display";
char stateTopic[48];
char availTopic[48];
uint32_t lastMqttTry = 0;
uint32_t lastStatePublish = 0;
bool mqttStarted = false;

bool mqttEnabled() { return MQTT_HOST[0] != 0; }

void mqttSetup() {
  snprintf(stateTopic, sizeof(stateTopic), "%s/state", DEVICE_ID);
  snprintf(availTopic, sizeof(availTopic), "%s/status", DEVICE_ID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(3);
  mqttStarted = true;
}

void publishJson(const char* topic, JsonDocument& doc, bool retain = true) {
  static char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));
  mqtt.publish(topic, (const uint8_t*)out, n, retain);
}

void addDevice(JsonDocument& d) {
  JsonObject dev = d["device"].to<JsonObject>();
  dev["identifiers"][0] = DEVICE_ID;
  dev["name"] = "Weather Alert Display";
  dev["manufacturer"] = "DIY";
  dev["model"] = "ESP32 + NWS + 1.8in TFT";
}

struct SensorDef {
  const char* key;
  const char* name;
  const char* unit;
  const char* deviceClass;
  const char* icon;
};

const SensorDef SENSORS[] = {
  {"temperature",   "Weather Alert Display Temperature",   "°F", "temperature", nullptr},
  {"feels_like",     "Weather Alert Display Feels Like",     "°F", "temperature", nullptr},
  {"condition",      "Weather Alert Display Condition",      nullptr, nullptr,        "mdi:weather-partly-cloudy"},
  {"high_today",    "Weather Alert Display High Today",     "°F", "temperature", nullptr},
  {"low_tonight",    "Weather Alert Display Low Tonight",    "°F", "temperature", nullptr},
  {"weather_alert",  "Weather Alert Display Weather Alert",  nullptr, nullptr,        "mdi:alert"},
  {"alert_count",    "Weather Alert Display Alert Count",    nullptr, nullptr,        "mdi:alert-circle-outline"},
  {"dew_point",      "Weather Alert Display Dew Point",      "°F", "temperature", nullptr},
  {"humidity",       "Weather Alert Display Humidity",       "%",     "humidity",     nullptr},
  {"pressure",       "Weather Alert Display Pressure",       "inHg",  "pressure",     nullptr},
  {"wind_speed",     "Weather Alert Display Wind Speed",     "mph",   nullptr,        "mdi:weather-windy"},
  {"wind_direction", "Weather Alert Display Wind Direction",  nullptr, nullptr,        "mdi:compass-outline"},
};

void publishDiscovery() {
  char topic[96];
  char objectId[48];
  for (const SensorDef& sd : SENSORS) {
    JsonDocument d;
    snprintf(objectId, sizeof(objectId), "%s_%s", DEVICE_ID, sd.key);
    d["name"] = sd.name;
    d["object_id"] = objectId;
    d["unique_id"] = objectId;
    d["state_topic"] = stateTopic;
    d["value_template"] = String("{{ value_json.") + sd.key + " }}";
    d["availability_topic"] = availTopic;
    if (sd.unit) d["unit_of_measurement"] = sd.unit;
    if (sd.deviceClass) {
      d["device_class"] = sd.deviceClass;
      d["state_class"] = "measurement";
    }
    if (sd.icon) d["icon"] = sd.icon;
    addDevice(d);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", DEVICE_ID, sd.key);
    publishJson(topic, d);
  }

  JsonDocument d;
  snprintf(objectId, sizeof(objectId), "%s_severe_weather", DEVICE_ID);
  d["name"] = "Weather Alert Display Severe Weather";
  d["object_id"] = objectId;
  d["unique_id"] = objectId;
  d["state_topic"] = stateTopic;
  d["value_template"] = "{{ 'ON' if value_json.severe_weather else 'OFF' }}";
  d["availability_topic"] = availTopic;
  d["device_class"] = "safety";
  addDevice(d);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s/severe_weather/config", DEVICE_ID);
  publishJson(topic, d);
}

void publishState() {
  if (!mqtt.connected()) return;
  JsonDocument d;
  if (weather.valid) {
    d["temperature"]   = (int)roundf(weather.temp);
    d["feels_like"]     = (int)roundf(weather.feelsLike);
    d["condition"]      = weather.condition;
    d["high_today"]     = weather.high;
    d["low_tonight"]     = weather.low;
    d["dew_point"]      = (int)roundf(weather.dewPoint);
    d["humidity"]       = weather.humidity;
    d["wind_speed"]     = (int)roundf(weather.wind);
    d["wind_direction"] = weather.windDir;
    if (weather.pressureInHg > 0) d["pressure"] = roundf(weather.pressureInHg * 100) / 100;
    else d["pressure"] = nullptr;
  } else {
    d["temperature"] = nullptr;
    d["feels_like"] = nullptr;
    d["condition"] = nullptr;
    d["high_today"] = nullptr;
    d["low_tonight"] = nullptr;
    d["dew_point"] = nullptr;
    d["humidity"] = nullptr;
    d["pressure"] = nullptr;
    d["wind_speed"] = nullptr;
    d["wind_direction"] = nullptr;
  }
  d["weather_alert"]   = alert.active ? alert.event : "None";
  d["alert_count"]     = alert.count;
  d["severe_weather"]  = alert.active;
  publishJson(stateTopic, d);
  lastStatePublish = millis();
}

void mqttLoop() {
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED) return;
  if (!mqttStarted) mqttSetup();
  if (!mqtt.connected()) {
    if (lastMqttTry != 0 && millis() - lastMqttTry < 30000) return;
    lastMqttTry = millis();
    const char* user = MQTT_USER[0] ? MQTT_USER : nullptr;
    const char* pass = MQTT_USER[0] ? MQTT_PASSWORD : nullptr;
    if (!mqtt.connect(DEVICE_ID, user, pass, availTopic, 0, true, "offline")) {
      Serial.printf("MQTT connect failed (state %d)\n", mqtt.state());
      return;
    }
    Serial.println("MQTT connected");
    mqtt.publish(availTopic, "online", true);
    publishDiscovery();
    publishState();
  }
  mqtt.loop();
  if (millis() - lastStatePublish > 60000UL) publishState();
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);

  if (!gfx->begin()) Serial.println("Display init failed");
  showSplash();
  delay(2500);

  if (!connectWiFi()) {
    showMessage("WiFi failed", "Retrying in background", C_ORANGE);
    delay(2000);
  } else {
    showMessage("WiFi connected", WiFi.localIP().toString().c_str());
    delay(1000);
  }
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");

  showMessage("Getting weather...");
  bool condOk = fetchConditions();
  bool fcOk   = fetchForecast();
  lastFetchFailed = !(condOk && fcOk);
  lastFetch = millis();

  lastAlertFetchFailed = !fetchAlerts();
  lastAlertFetch = millis();

  mqttLoop();
}

void loop() {
  uint32_t interval = lastFetchFailed ? 60UL * 1000UL : WEATHER_REFRESH_MS;
  if (millis() - lastFetch >= interval) {
    bool condOk = fetchConditions();
    bool fcOk   = fetchForecast();
    lastFetchFailed = !(condOk && fcOk);
    lastFetch = millis();
  }

  uint32_t alertInterval = lastAlertFetchFailed ? 30UL * 1000UL : ALERT_REFRESH_MS;
  if (millis() - lastAlertFetch >= alertInterval) {
    lastAlertFetchFailed = !fetchAlerts();
    lastAlertFetch = millis();
  }

  static uint32_t lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 300000UL) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }
  mqttLoop();

  // When rain is in the forecast, alternate every 5s between the normal
  // screen and a dedicated rain takeover screen — but a severe weather
  // alert always wins, since that's the higher-priority thing to show.
  bool showRainScreen = rain.active && !alert.active && ((millis() / 5000) % 2 == 1);

  if (showRainScreen) {
    drawRainScreen();
  } else if (weather.valid) {
    drawWeather();
  } else {
    showMessage("No weather yet", "Retrying in 1 min");
  }
  gfx->flush();
  delay(alert.active ? 200 : 1000);
}
