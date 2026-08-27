#ifndef BOARD_HAS_PSRAM
#error "PSRAM not enabled — Tools -> PSRAM -> OPI PSRAM"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <cctype>
#include <Wire.h>
#include <TouchDrvGT911.hpp>
#include "epd_driver.h"
#include "firasans.h"
#include "weathericons.h"
#include "secrets.h"
#include "types.h"

#ifndef BOARD_SDA
#define BOARD_SDA 8
#endif
#ifndef BOARD_SCL
#define BOARD_SCL 9
#endif
#ifndef TOUCH_INT
#define TOUCH_INT 3
#endif

#define SLEEP_SECONDS (1 * 60 * 60)


#ifndef SD_CS
#define SD_CS 13
#endif
#ifndef SD_MOSI
#define SD_MOSI 15
#endif
#ifndef SD_SCLK
#define SD_SCLK 14
#endif
#ifndef SD_MISO
#define SD_MISO 2
#endif

#define COL_DIV (EPD_WIDTH / 2)
#define ROW_DIV (EPD_HEIGHT / 2)

uint8_t *framebuffer = NULL;
SPIClass sdSPI(HSPI);
bool sdReady = false;
ScreenMode currentScreen = SCREEN_DASHBOARD;
AppData appData;
TouchDrvGT911 touch;
bool touchReady = false;
bool touchWaitRelease = false;
int lastRenderedDay = -1;
char lastBmpError[96] = "";


void textBounds(const char *s, int32_t *w, int32_t *h) {
  int32_t x = 0, y = 0, x1, y1;
  get_text_bounds((GFXfont *)&FiraSans, s, &x, &y, &x1, &y1, w, h, NULL);
}

void drawAt(const char *s, int x, int y) {
  writeln((GFXfont *)&FiraSans, s, &x, &y, framebuffer);
}

void setPixel4(int x, int y, uint8_t gray) {
  if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
  uint32_t i = (uint32_t)y * EPD_WIDTH + x;
  uint8_t *b = &framebuffer[i / 2];
  gray &= 0x0F;
  if ((i & 1) == 0) *b = (*b & 0x0F) | (gray << 4);
  else              *b = (*b & 0xF0) | gray;
}

void fillRectWhite(int x, int y, int w, int h) {
  epd_fill_rect(x, y, w, h, 0xFF, framebuffer);
}

void drawCentered(const char *s, int x0, int x1, int y) {
  int32_t tw, th;
  textBounds(s, &tw, &th);
  int x = x0 + ((x1 - x0) - tw) / 2;
  writeln((GFXfont *)&FiraSans, s, &x, &y, framebuffer);
}

void drawOrnamentRule(const char *sym, int x0, int x1, int y) {
  int32_t sw, sh;
  textBounds(sym, &sw, &sh);
  int cx = x0 + (x1 - x0) / 2;
  int sym_x = cx - sw / 2;
  epd_draw_hline(x0 + 8, y - 6, cx - sw / 2 - x0 - 12, 0x00, framebuffer);
  epd_draw_hline(cx + sw / 2 + 4, y - 6, x1 - (cx + sw / 2 + 4) - 8, 0x00, framebuffer);
  epd_fill_rect(sym_x - 2, y - sh - 2, sw + 4, sh + 4, 0xFF, framebuffer);
  writeln((GFXfont *)&FiraSans, sym, &sym_x, &y, framebuffer);
}

void toUpper(char *s) {
  for (; *s; s++) *s = toupper((unsigned char)*s);
}

ScreenMode parseDisplayMode(const String &raw) {
  String s = raw;
  s.trim();
  s.toLowerCase();
  s.replace("-", "_");
  s.replace(" ", "_");
  if (s == "text_only" || s == "text" || s == "letter") return SCREEN_LETTER;
  if (s == "image_only" || s == "image" || s == "split_screen" || s == "split" || s == "memory") return SCREEN_MEMORY;
  if (s == "calendar") return SCREEN_CALENDAR;
  if (s == "weather_full" || s == "weather") return SCREEN_WEATHER_FULL;
  return SCREEN_DASHBOARD;
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}


const char *posixTimezone(const String &iana) {
  if (iana == "America/Sao_Paulo") return "BRT3";
  return "UTC0";
}

bool syncTime(const AppSettings &settings, struct tm *out) {
  configTzTime(posixTimezone(settings.timezone), "pool.ntp.org", "time.nist.gov");
  Serial.print("NTP");
  for (int i = 0; i < 20; i++) {
    delay(500); Serial.print(".");
    if (getLocalTime(out)) { Serial.println(" OK"); return true; }
  }
  Serial.println(" FAILED");
  return false;
}

String isoDate(const struct tm *t) {
  if (!t) return "";
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}


int daysUntilVisit(const struct tm *now, const AppSettings &settings) {
  struct tm visit = {};
  visit.tm_year = settings.next_visit_year - 1900;
  visit.tm_mon  = settings.next_visit_month - 1;
  visit.tm_mday = settings.next_visit_day;
  visit.tm_hour = 12;
  time_t t_visit = mktime(&visit);
  struct tm noon = *now;
  noon.tm_hour = 12; noon.tm_min = 0; noon.tm_sec = 0;
  time_t t_now = mktime(&noon);
  return (int)(difftime(t_visit, t_now) / 86400.0);
}


TagRecord fetchRecord(const String &tagId) {
  TagRecord rec; rec.valid = false;
  HTTPClient http;
  String url = String(APPS_SCRIPT_URL) + "?tag=" + tagId;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { Serial.printf("HTTP %d\n", code); http.end(); return rec; }
  String body = http.getString();
  http.end();
  Serial.println(body);
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) || doc.containsKey("error")) return rec;
  rec.tag_id       = doc["tag_id"].as<String>();
  rec.display_type = doc["display_type"].as<String>();
  rec.message      = doc["message"].as<String>();
  rec.image_file   = doc["image_file"].as<String>();
  rec.valid        = true;
  return rec;
}

bool fetchAppData(const String &date, AppData *out) {
  if (!out) return false;

  HTTPClient http;
  String url = date.length() ? String(APPS_SCRIPT_URL) + "?date=" + date : String(APPS_SCRIPT_URL);
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Sheet HTTP %d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Sheet JSON %s\n", err.c_str());
    return false;
  }
  if (doc.containsKey("error")) {
    Serial.printf("Sheet error %s\n", doc["error"].as<const char *>());
    return false;
  }

  AppData parsed = *out;
  JsonObject settings = doc["settings"];
  if (!settings.isNull()) {
    if (settings["timezone"].is<const char *>()) parsed.settings.timezone = settings["timezone"].as<String>();
    if (settings["location_name"].is<const char *>()) parsed.settings.location_name = settings["location_name"].as<String>();
    if (!settings["latitude"].isNull()) parsed.settings.latitude = settings["latitude"].as<float>();
    if (!settings["longitude"].isNull()) parsed.settings.longitude = settings["longitude"].as<float>();
    if (settings["next_visit_label"].is<const char *>()) parsed.settings.next_visit_label = settings["next_visit_label"].as<String>();
    if (settings["next_visit_date"].is<const char *>()) {
      parseIsoDateIntoVisit(settings["next_visit_date"].as<String>(), &parsed.settings);
    }
    parsed.settings.valid = true;
  }

  JsonObject letter = doc["letter"];
  if (!letter.isNull()) {
    if (letter["date"].is<const char *>()) parsed.letter.date = letter["date"].as<String>();
    if (letter["title"].is<const char *>()) parsed.letter.title = letter["title"].as<String>();
    if (letter["body"].is<const char *>()) parsed.letter.body = letter["body"].as<String>();
    parsed.letter.valid = parsed.letter.body.length() > 0;
  }

  JsonObject memory = doc["memory"];
  if (!memory.isNull()) {
    if (memory["date"].is<const char *>())    parsed.memory.date    = memory["date"].as<String>();
    if (memory["image_file"].is<const char *>()) parsed.memory.image_file = memory["image_file"].as<String>();
    if (memory["caption"].is<const char *>()) parsed.memory.caption = memory["caption"].as<String>();
    parsed.memory.valid = parsed.memory.caption.length() > 0;
  }

  *out = parsed;
  return true;
}

const char *weatherCodeDescription(int code) {
  switch (code) {
    case 0: return "céu limpo";
    case 1: return "quase limpo";
    case 2: return "parcialmente nublado";
    case 3: return "nublado";
    case 48: return "neblina";
    case 55: return "garoa";
    case 65: return "chuva";
    case 82: return "pancadas";
    case 95: return "trovoada";
    case 99: return "tempestade";
    default: return "tempo";
  }
}

const char *weekdayPt(int wday) {
  switch (wday) {
    case 0: return "domingo";
    case 1: return "segunda";
    case 2: return "terça";
    case 3: return "quarta";
    case 4: return "quinta";
    case 5: return "sexta";
    case 6: return "sábado";
    default: return "dia";
  }
}

const char *monthPt(int mon) {
  switch (mon) {
    case 0: return "janeiro";
    case 1: return "fevereiro";
    case 2: return "março";
    case 3: return "abril";
    case 4: return "maio";
    case 5: return "junho";
    case 6: return "julho";
    case 7: return "agosto";
    case 8: return "setembro";
    case 9: return "outubro";
    case 10: return "novembro";
    case 11: return "dezembro";
    default: return "mês";
  }
}

String weekdayShortFromIsoDate(const String &date) {
  if (date.length() < 10) return "";
  struct tm tm = {};
  tm.tm_year = date.substring(0, 4).toInt() - 1900;
  tm.tm_mon  = date.substring(5, 7).toInt() - 1;
  tm.tm_mday = date.substring(8, 10).toInt();
  mktime(&tm);
  const char *names[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
  return names[tm.tm_wday];
}

WeatherData fetchWeather(const AppSettings &settings) {
  WeatherData weather;
  weather.valid = false;
  weather.day_count = 0;
  weather.current.temperature_c = 0;
  weather.current.max_c = 0;
  weather.current.min_c = 0;
  weather.current.humidity_percent = 0;
  weather.current.wind_kmh = 0;
  weather.current.rain_percent = 0;
  weather.current.weather_code = -1;
  weather.current.description = "tempo indisponível";
  weather.current.valid = false;

  String tz = settings.timezone;
  tz.replace("/", "%2F");
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(settings.latitude, 4)
    + "&longitude=" + String(settings.longitude, 4)
    + "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    + "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
    + "&forecast_days=7&timezone=" + tz;

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP %d\n", code);
    http.end();
    return weather;
  }

  String body = http.getString();
  http.end();
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Weather JSON %s\n", err.c_str());
    return weather;
  }

  float temp = doc["current"]["temperature_2m"] | 0.0;
  weather.current.temperature_c = (int)round(temp);
  weather.current.humidity_percent = doc["current"]["relative_humidity_2m"] | 0;
  weather.current.wind_kmh = (int)round((float)(doc["current"]["wind_speed_10m"] | 0.0));
  weather.current.weather_code = doc["current"]["weather_code"] | -1;
  weather.current.description = weatherCodeDescription(weather.current.weather_code);
  weather.current.valid = true;

  JsonArray maxes = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray mins  = doc["daily"]["temperature_2m_min"].as<JsonArray>();
  JsonArray rains = doc["daily"]["precipitation_probability_max"].as<JsonArray>();
  JsonArray codes = doc["daily"]["weather_code"].as<JsonArray>();
  JsonArray times = doc["daily"]["time"].as<JsonArray>();
  int count = min(7, (int)maxes.size());
  for (int i = 0; i < count; i++) {
    weather.days[i].max_c       = (int)round((float)maxes[i]);
    weather.days[i].min_c       = (int)round((float)mins[i]);
    weather.days[i].rain_percent = rains[i] | 0;
    weather.days[i].weather_code = codes[i] | -1;
    weather.days[i].description  = weatherCodeDescription(weather.days[i].weather_code);
    weather.days[i].weekday      = weekdayShortFromIsoDate(times[i] | "");
  }
  weather.day_count = count;
  weather.current.max_c        = count > 0 ? weather.days[0].max_c        : weather.current.temperature_c;
  weather.current.min_c        = count > 0 ? weather.days[0].min_c        : weather.current.temperature_c;
  weather.current.rain_percent = count > 0 ? weather.days[0].rain_percent : 0;
  weather.valid = weather.current.valid;
  Serial.printf("Weather %s %dC day_count=%d\n", settings.location_name.c_str(), weather.current.temperature_c, weather.day_count);
  return weather;
}


void parseIsoDateIntoVisit(const String &date, AppSettings *settings) {
  if (!settings || date.length() != 10) return;
  if (date.charAt(4) != '-' || date.charAt(7) != '-') return;

  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) continue;
    if (!isDigit(date.charAt(i))) return;
  }

  int year = date.substring(0, 4).toInt();
  int month = date.substring(5, 7).toInt();
  int day = date.substring(8, 10).toInt();
  if (month < 1 || month > 12) return;

  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  if (leap) daysInMonth[1] = 29;
  if (day < 1 || day > daysInMonth[month - 1]) return;

  settings->next_visit_year = year;
  settings->next_visit_month = month;
  settings->next_visit_day = day;
}

AppSettings defaultSettings() {
  AppSettings settings;
  settings.timezone = "America/Sao_Paulo";
  settings.location_name = "Botucatu, SP";
  settings.latitude = -22.8858;
  settings.longitude = -48.4450;
  settings.next_visit_year = NEXT_VISIT_YEAR;
  settings.next_visit_month = NEXT_VISIT_MONTH;
  settings.next_visit_day = NEXT_VISIT_DAY;
  settings.next_visit_label = "próxima visita";
  settings.valid = true;
  return settings;
}

LetterData defaultLetter() {
  LetterData letter;
  letter.date = "";
  letter.title = "cartinha diaria";
  letter.body = "onde eu vou poder me connectar com vc e te falar algo do meu coracao";
  letter.valid = true;
  return letter;
}

MemoryData defaultMemory() {
  MemoryData memory;
  memory.date = "";
  memory.image_file = "mem_001.bmp";
  memory.caption = "";
  memory.valid = true;
  return memory;
}

void loadDefaultAppData() {
  appData.settings = defaultSettings();
  appData.letter = defaultLetter();
  appData.memory = defaultMemory();
  appData.weather.current.temperature_c = 0;
  appData.weather.current.max_c = 0;
  appData.weather.current.min_c = 0;
  appData.weather.current.humidity_percent = 0;
  appData.weather.current.wind_kmh = 0;
  appData.weather.current.rain_percent = 0;
  appData.weather.current.weather_code = -1;
  appData.weather.current.description = "tempo indisponível";
  appData.weather.current.valid = false;
  appData.weather.day_count = 0;
  appData.weather.valid = false;
  for (int i = 0; i < 7; i++) {
    appData.weather.days[i].weekday = "";
    appData.weather.days[i].max_c = 0;
    appData.weather.days[i].min_c = 0;
    appData.weather.days[i].rain_percent = 0;
    appData.weather.days[i].weather_code = -1;
    appData.weather.days[i].description = "tempo indisponível";
  }
}

bool initSD() {
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSPI, 4000000);
  Serial.printf("SD %s\n", sdReady ? "OK" : "FAILED");
  return sdReady;
}

String memoryEntryPath(const String &name) {
  if (name.startsWith("/")) return name;
  return "/memories/" + name;
}

bool isSupportedBmpFile(const String &path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  bool ok = false;
  uint8_t h[34];
  if (f.size() >= 54 && f.read(h, sizeof(h)) == sizeof(h) && h[0] == 'B' && h[1] == 'M') {
    uint32_t headerSize = (uint32_t)h[14] | ((uint32_t)h[15] << 8) | ((uint32_t)h[16] << 16) | ((uint32_t)h[17] << 24);
    if (headerSize >= 40) {
      int32_t bmpW = (int32_t)((uint32_t)h[18] | ((uint32_t)h[19] << 8) | ((uint32_t)h[20] << 16) | ((uint32_t)h[21] << 24));
      int32_t bmpH = (int32_t)((uint32_t)h[22] | ((uint32_t)h[23] << 8) | ((uint32_t)h[24] << 16) | ((uint32_t)h[25] << 24));
      uint16_t planes = (uint16_t)h[26] | ((uint16_t)h[27] << 8);
      uint16_t depth = (uint16_t)h[28] | ((uint16_t)h[29] << 8);
      uint32_t compression = (uint32_t)h[30] | ((uint32_t)h[31] << 8) | ((uint32_t)h[32] << 16) | ((uint32_t)h[33] << 24);
      ok = bmpW > 0 && bmpH != 0 && planes == 1 && compression == 0
        && (depth == 1 || depth == 8 || depth == 24);
    }
  }
  f.close();
  return ok;
}

String firstSupportedMemory() {
  if (!sdReady) return "";
  File dir = SD.open("/memories");
  if (!dir || !dir.isDirectory()) { dir.close(); return ""; }

  String selected = "";
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());
    String nameLower = name;
    nameLower.toLowerCase();
    if (!f.isDirectory() && nameLower.endsWith(".bmp") && isSupportedBmpFile(memoryEntryPath(name))) {
      selected = name;
      f.close();
      break;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return selected;
}

String selectDailyMemory(const struct tm *t) {
  if (!sdReady) return "";
  File dir = SD.open("/memories");
  if (!dir || !dir.isDirectory()) { dir.close(); return ""; }

  int count = 0;
  {
    File f = dir.openNextFile();
    while (f) {
      String name = String(f.name());
      String nameLower = name;
      nameLower.toLowerCase();
      if (!f.isDirectory() && nameLower.endsWith(".bmp") && isSupportedBmpFile(memoryEntryPath(name))) count++;
      f.close();
      f = dir.openNextFile();
    }
  }
  dir.close();
  if (count == 0) return "";

  int day = (t->tm_year + 1900 - 2000) * 366 + t->tm_yday;
  int target = ((day % count) + count) % count;

  dir = SD.open("/memories");
  if (!dir) return "";
  String selected = "";
  int idx = 0;
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());
    String nameLower = name;
    nameLower.toLowerCase();
    if (!f.isDirectory() && nameLower.endsWith(".bmp") && isSupportedBmpFile(memoryEntryPath(name))) {
      if (idx == target) { selected = name; f.close(); break; }
      idx++;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  Serial.printf("Daily memory [%d/%d]: %s\n", target, count, selected.c_str());
  return selected;
}

bool initTouch() {
  Wire.begin(BOARD_SDA, BOARD_SCL);
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, HIGH);
  uint8_t touchAddress = 0;
  Wire.beginTransmission(0x14);
  if (Wire.endTransmission() == 0) touchAddress = 0x14;
  Wire.beginTransmission(0x5D);
  if (Wire.endTransmission() == 0) touchAddress = 0x5D;
  if (touchAddress == 0) {
    Serial.println("Touch not found");
    return false;
  }
  touch.setPins(-1, TOUCH_INT);
  if (!touch.begin(Wire, touchAddress, BOARD_SDA, BOARD_SCL)) {
    Serial.println("Touch begin failed");
    return false;
  }
  touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
  Serial.println("Touch OK");
  return true;
}

uint16_t read16(File &f) {
  uint16_t v = f.read();
  v |= (uint16_t)f.read() << 8;
  return v;
}

uint32_t read32(File &f) {
  uint32_t v = read16(f);
  v |= (uint32_t)read16(f) << 16;
  return v;
}

bool drawBmp(const String &name, int x0, int y0, int maxW, int maxH) {
  lastBmpError[0] = '\0';
  if (!sdReady) {
    snprintf(lastBmpError, sizeof(lastBmpError), "SD not ready");
    return false;
  }
  if (name.length() == 0) {
    snprintf(lastBmpError, sizeof(lastBmpError), "empty image path");
    return false;
  }
  String path = name.startsWith("/") ? name : "/" + name;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    snprintf(lastBmpError, sizeof(lastBmpError), "open failed: %.70s", path.c_str());
    Serial.printf("BMP open failed: %s\n", path.c_str());
    return false;
  }

  if (read16(f) != 0x4D42) {
    snprintf(lastBmpError, sizeof(lastBmpError), "bad signature: %.68s", path.c_str());
    f.close();
    return false;
  }
  (void)read32(f);
  (void)read32(f);
  uint32_t imageOffset = read32(f);
  uint32_t headerSize = read32(f);
  if (headerSize < 40) {
    snprintf(lastBmpError, sizeof(lastBmpError), "bad header: %.72s", path.c_str());
    f.close();
    return false;
  }

  int32_t bmpW = (int32_t)read32(f);
  int32_t bmpH = (int32_t)read32(f);
  if (bmpW <= 0 || bmpH == 0) {
    snprintf(lastBmpError, sizeof(lastBmpError), "bad size: %.74s", path.c_str());
    f.close();
    return false;
  }
  bool flip = bmpH > 0;
  if (bmpH < 0) bmpH = -bmpH;
  if (read16(f) != 1) {
    snprintf(lastBmpError, sizeof(lastBmpError), "bad planes: %.72s", path.c_str());
    f.close();
    return false;
  }
  uint16_t depth = read16(f);
  uint32_t compression = read32(f);
  if (compression != 0 || (depth != 1 && depth != 8 && depth != 24)) {
    snprintf(lastBmpError, sizeof(lastBmpError), "unsupported BMP d=%u c=%u", depth, compression);
    f.close();
    Serial.printf("BMP unsupported depth=%u compression=%u\n", depth, compression);
    return false;
  }

  int drawW = min((int)bmpW, maxW);
  int drawH = min((int)bmpH, maxH);
  uint32_t rowSize = ((uint32_t)depth * bmpW + 31) / 32 * 4;

  for (int y = 0; y < drawH; y++) {
    int srcY = flip ? (bmpH - 1 - y) : y;
    f.seek(imageOffset + (uint32_t)srcY * rowSize);
    uint8_t bitByte = 0;
    for (int x = 0; x < drawW; x++) {
      uint8_t gray = 0x0F;
      if (depth == 1) {
        if ((x & 7) == 0) bitByte = f.read();
        bool white = bitByte & (0x80 >> (x & 7));
        gray = white ? 0x0F : 0x00;
      } else if (depth == 8) {
        uint8_t v = f.read();
        gray = (v > 127) ? 0x0F : 0x00;
      } else {
        uint8_t b = f.read(), g = f.read(), r = f.read();
        uint16_t luma = (uint16_t)r * 30 + (uint16_t)g * 59 + (uint16_t)b * 11;
        gray = (luma > 12700) ? 0x0F : 0x00;
      }
      setPixel4(x0 + x, y0 + y, gray);
    }
  }
  f.close();
  return true;
}


void drawDoubleBorder() {
  epd_draw_rect(0, 0, EPD_WIDTH,     EPD_HEIGHT,     0x00, framebuffer);
  epd_draw_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, 0x00, framebuffer);
  epd_draw_rect(8, 8, EPD_WIDTH - 16, EPD_HEIGHT - 16, 0x00, framebuffer);
}


void renderDashboard(const struct tm *t, const WeatherNow &weather) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  drawDoubleBorder();

  epd_draw_vline(COL_DIV,     16, EPD_HEIGHT - 32, 0x00, framebuffer);
  epd_draw_vline(COL_DIV + 1, 16, EPD_HEIGHT - 32, 0x00, framebuffer);
  epd_draw_hline(16, ROW_DIV, EPD_WIDTH - 32, 0x00, framebuffer);
  epd_draw_hline(16, ROW_DIV + 1, EPD_WIDTH - 32, 0x00, framebuffer);

  {
    const char *sym = "+";
    int32_t sw, sh; textBounds(sym, &sw, &sh);
    epd_fill_rect(COL_DIV - sw/2 - 4, ROW_DIV - sh/2 - 4, sw + 8, sh + 8, 0xFF, framebuffer);
    int sx = COL_DIV - sw/2, sy = ROW_DIV + sh/2 + 4;
    writeln((GFXfont *)&FiraSans, sym, &sx, &sy, framebuffer);
  }

  char buf[64];

  {
    const int BW = 300, BH = 228;
    const int HEADER_H = 52;
    const int bcx = (16 + COL_DIV - 16) / 2;
    const int bcy = (16 + ROW_DIV) / 2;
    const int bx = bcx - BW / 2;
    const int by = bcy - BH / 2;

    epd_draw_rect(bx,     by,     BW,     BH,     0x00, framebuffer);
    epd_draw_rect(bx + 1, by + 1, BW - 2, BH - 2, 0x00, framebuffer);

    epd_fill_rect(bx + 2, by + 2, BW - 4, HEADER_H - 2, 0x00, framebuffer);

    {
      char hbuf[32];
      snprintf(hbuf, sizeof(hbuf), "%s %04d", monthPt(t->tm_mon), t->tm_year + 1900);
      toUpper(hbuf);
      int32_t tw, th;
      textBounds(hbuf, &tw, &th);
      FontProperties wp;
      wp.fg_color = 15; wp.bg_color = 0; wp.fallback_glyph = ' '; wp.flags = 0;
      int hx = bx + (BW - tw) / 2;
      int hy = by + HEADER_H - 10;
      write_mode((GFXfont *)&FiraSans, hbuf, &hx, &hy, framebuffer, WHITE_ON_BLACK, &wp);
    }

    snprintf(buf, sizeof(buf), "%d", t->tm_mday);
    drawCentered(buf, bx, bx + BW, by + HEADER_H + 58);

    snprintf(buf, sizeof(buf), "%s", weekdayPt(t->tm_wday));
    drawCentered(buf, bx, bx + BW, by + HEADER_H + 110);

    strftime(buf, sizeof(buf), "%H:%M", t);
    drawCentered(buf, bx, bx + BW, by + HEADER_H + 162);
  }

  const int R0 = COL_DIV + 16;
  const int R1 = EPD_WIDTH - 16;
  int days = daysUntilVisit(t, appData.settings);
  {
    const int EW = 310, EH = 210;
    const int ecx = (R0 + R1) / 2;
    const int ecy = (16 + ROW_DIV) / 2;
    const int ex = ecx - EW / 2;
    const int ey = ecy - EH / 2;

    epd_draw_rect(ex,     ey,     EW,     EH,     0x00, framebuffer);
    epd_draw_rect(ex + 1, ey + 1, EW - 2, EH - 2, 0x00, framebuffer);

    const int flapY = ey + EH * 2 / 5;
    epd_draw_line(ex,          ey, ecx, flapY, 0x00, framebuffer);
    epd_draw_line(ex + EW - 1, ey, ecx, flapY, 0x00, framebuffer);

    epd_draw_hline(ex + 2, flapY, EW - 4, 0x00, framebuffer);

    snprintf(buf, sizeof(buf), "%d dias...", abs(days));
    const int textY = flapY + (ey + EH - flapY) / 2 + 16;
    drawCentered(buf, ex + 16, ex + EW - 16, textY);
  }

  {
    const int BW = 290, BH = 205;
    const int wcx = (16 + COL_DIV - 16) / 2;
    const int wcy = (ROW_DIV + 16 + EPD_HEIGHT - 16) / 2;
    const int wx = wcx - BW / 2;
    const int wy = wcy - BH / 2;

    epd_draw_rect(wx,     wy,     BW,     BH,     0x00, framebuffer);
    epd_draw_rect(wx + 1, wy + 1, BW - 2, BH - 2, 0x00, framebuffer);

    char weatherLine[48];
    if (weather.valid) snprintf(weatherLine, sizeof(weatherLine), "botucatu %d\xC2\xB0""C", weather.temperature_c);
    else snprintf(weatherLine, sizeof(weatherLine), "botucatu --\xC2\xB0""C");
    drawCentered(weatherLine, wx, wx + BW, wy + 68);

    char rangeLine[48];
    if (weather.valid) snprintf(rangeLine, sizeof(rangeLine), "%d\xC2\xB0""C | %d\xC2\xB0""C", weather.min_c, weather.max_c);
    else snprintf(rangeLine, sizeof(rangeLine), "m\xC3\xA1x --\xC2\xB0""C  m\xC3\xADn --\xC2\xB0""C");
    drawCentered(rangeLine, wx, wx + BW, wy + 120);

    if (weather.valid)
      drawWeatherIcon(weatherIconStr(weather.weather_code), wx, wx + BW, wy + 172);
  }

  {
    const int PW = 330, PH = 210;
    const int cx = (R0 + R1) / 2;
    const int cy = (ROW_DIV + 16 + EPD_HEIGHT - 16) / 2;
    const int px = cx - PW / 2;
    const int py = cy - PH / 2;

    epd_draw_rect(px,     py,     PW,     PH,     0x00, framebuffer);
    epd_draw_rect(px + 1, py + 1, PW - 2, PH - 2, 0x00, framebuffer);

    const int M = 14, BOT = 56;
    epd_draw_rect(px + M, py + M, PW - 2*M, PH - M - BOT, 0x00, framebuffer);

    epd_draw_hline(px + 2, py + PH - BOT, PW - 4, 0x00, framebuffer);

    drawCentered("mem\xC3\xB3ria di\xC3\xA1ria", px, px + PW, py + PH - BOT/2 + 12);
  }
}


void renderTextOnly(const String &message) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  epd_draw_rect(0,  0,  EPD_WIDTH,      EPD_HEIGHT,      0x00, framebuffer);
  epd_draw_rect(1,  1,  EPD_WIDTH - 2,  EPD_HEIGHT - 2,  0x00, framebuffer);
  epd_draw_rect(14, 14, EPD_WIDTH - 28, EPD_HEIGHT - 28, 0x00, framebuffer);

  drawAt("✦", 28, 54);
  { int x = EPD_WIDTH - 54, y = 54; writeln((GFXfont *)&FiraSans, "✦", &x, &y, framebuffer); }
  drawAt("✦", 28, EPD_HEIGHT - 28);
  { int x = EPD_WIDTH - 54, y = EPD_HEIGHT - 28; writeln((GFXfont *)&FiraSans, "✦", &x, &y, framebuffer); }

  drawAt("my love,", 88, 130);

  drawOrnamentRule("✦", 88, EPD_WIDTH - 88, 162);

  {
    const int PAD = 88;
    const int LINE_W = EPD_WIDTH - PAD * 2;
    int wy = 230;
    String msg = message;
    while (msg.length() > 0) {
      int cut = (int)msg.length();
      if (cut > 36) {
        int sp = msg.lastIndexOf(' ', 36);
        cut = (sp > 0) ? sp : 36;
      }
      char line[64];
      msg.substring(0, cut).toCharArray(line, sizeof(line));
      drawCentered(line, PAD, EPD_WIDTH - PAD, wy);
      wy += 58;
      msg = (cut < (int)msg.length()) ? msg.substring(cut + 1) : "";
    }

    drawOrnamentRule("✦", 88, EPD_WIDTH - 88, wy + 20);

    { int x = EPD_WIDTH - 200, y = wy + 80;
      writeln((GFXfont *)&FiraSans, "-- always <3", &x, &y, framebuffer); }
  }
}

void renderImageOnly(const String &imageFile) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  epd_draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, 0x00, framebuffer);
  epd_draw_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, 0x00, framebuffer);

  const int matX = 36;
  const int matY = 28;
  const int photoW = EPD_WIDTH - matX * 2;
  const int photoH = EPD_HEIGHT - matY * 2;
  epd_draw_rect(matX, matY, photoW, photoH, 0x00, framebuffer);

  bool ok = drawBmp(imageFile, matX + 1, matY + 1, photoW - 2, photoH - 30);
  if (!ok) drawCentered("SD BMP missing", matX, matX + photoW, EPD_HEIGHT / 2);

  fillRectWhite(matX + 1, matY + photoH - 29, photoW - 2, 28);
  epd_draw_hline(matX + 1, matY + photoH - 29, photoW - 2, 0x00, framebuffer);
  char cap[64];
  imageFile.toCharArray(cap, sizeof(cap));
  toUpper(cap);
  drawAt(cap, matX + 18, matY + photoH - 8);
  drawAt("<3", matX + photoW - 46, matY + photoH - 8);
}

void renderSplitScreen(const String &message, const String &imageFile) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  bool ok = drawBmp(imageFile, 0, 0, EPD_WIDTH, EPD_HEIGHT);
  if (!ok) {
    for (int y = 0; y < EPD_HEIGHT; y += 10) {
      epd_draw_hline(0, y, EPD_WIDTH, 0x00, framebuffer);
    }
  }

  const int cardH = 170;
  const int cardY = EPD_HEIGHT - cardH;
  fillRectWhite(0, cardY, EPD_WIDTH, cardH);
  epd_draw_hline(0, cardY, EPD_WIDTH, 0x00, framebuffer);
  epd_draw_hline(0, cardY + 1, EPD_WIDTH, 0x00, framebuffer);
  drawOrnamentRule("*", 56, EPD_WIDTH - 56, cardY + 34);

  String msg = message.length() ? message : "Memory saved here.";
  int wy = cardY + 84;
  while (msg.length() > 0 && wy < EPD_HEIGHT - 42) {
    int cut = (int)msg.length();
    if (cut > 48) {
      int sp = msg.lastIndexOf(' ', 48);
      cut = (sp > 0) ? sp : 48;
    }
    char line[80];
    msg.substring(0, cut).toCharArray(line, sizeof(line));
    drawCentered(line, 56, EPD_WIDTH - 56, wy);
    wy += 42;
    msg = (cut < (int)msg.length()) ? msg.substring(cut + 1) : "";
  }
  drawOrnamentRule("<3", 56, EPD_WIDTH - 56, EPD_HEIGHT - 20);
}


bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int monthZeroBased) {
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (monthZeroBased == 1 && isLeapYear(year)) return 29;
  return days[monthZeroBased];
}

int firstWeekdayOfMonth(int year, int monthZeroBased) {
  struct tm first = {};
  first.tm_year = year - 1900;
  first.tm_mon  = monthZeroBased;
  first.tm_mday = 1;
  mktime(&first);
  return first.tm_wday;
}


void renderCalendar(const struct tm *t) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  drawDoubleBorder();
  int year  = t->tm_year + 1900;
  int month = t->tm_mon;
  char title[64];
  snprintf(title, sizeof(title), "%s %04d", monthPt(month), year);
  drawCentered(title, 20, EPD_WIDTH - 20, 54);

  const char *dow[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
  int gridX = 32, gridY = 86, gridW = EPD_WIDTH - 64, gridH = EPD_HEIGHT - 118;
  int cellW = gridW / 7;
  int cellH = gridH / 7;
  for (int c = 0; c <= 7; c++) epd_draw_vline(gridX + c * cellW, gridY, cellH * 7, 0x00, framebuffer);
  for (int r = 0; r <= 7; r++) epd_draw_hline(gridX, gridY + r * cellH, cellW * 7, 0x00, framebuffer);
  for (int c = 0; c < 7; c++) drawCentered(dow[c], gridX + c * cellW, gridX + (c + 1) * cellW, gridY + 42);

  int first = firstWeekdayOfMonth(year, month);
  int total = daysInMonth(year, month);
  for (int day = 1; day <= total; day++) {
    int index = first + day - 1;
    int col = index % 7;
    int row = index / 7 + 1;
    int x0  = gridX + col * cellW;
    int y0  = gridY + row * cellH;
    char num[4];
    snprintf(num, sizeof(num), "%d", day);
    if (day == t->tm_mday) {
      epd_draw_rect(x0 + 8, y0 + 8, cellW - 16, cellH - 16, 0x00, framebuffer);
      epd_draw_rect(x0 + 9, y0 + 9, cellW - 18, cellH - 18, 0x00, framebuffer);
    }
    drawCentered(num, x0, x0 + cellW, y0 + 44);
  }
}


void drawWrappedCentered(String text, int x0, int x1, int y, int lineHeight, int maxChars, int maxLines) {
  int lines = 0;
  while (text.length() > 0 && lines < maxLines) {
    int cut = (int)text.length();
    if (cut > maxChars) {
      int sp = text.lastIndexOf(' ', maxChars);
      cut = (sp > 0) ? sp : maxChars;
    }
    char line[128];
    text.substring(0, cut).toCharArray(line, sizeof(line));
    drawCentered(line, x0, x1, y + lines * lineHeight);
    text  = (cut < (int)text.length()) ? text.substring(cut + 1) : "";
    lines++;
  }
  if (text.length() > 0) drawCentered("...", x0, x1, y + lines * lineHeight);
}


void renderLetter(const LetterData &letter) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  epd_draw_rect(0,  0,  EPD_WIDTH,      EPD_HEIGHT,      0x00, framebuffer);
  epd_draw_rect(1,  1,  EPD_WIDTH - 2,  EPD_HEIGHT - 2,  0x00, framebuffer);
  epd_draw_rect(14, 14, EPD_WIDTH - 28, EPD_HEIGHT - 28, 0x00, framebuffer);
  String title = letter.title.length() ? letter.title : "para hoje";
  char titleBuf[80];
  title.toCharArray(titleBuf, sizeof(titleBuf));
  drawCentered(titleBuf, 80, EPD_WIDTH - 80, 86);
  epd_draw_hline(88, 120, EPD_WIDTH - 176, 0x00, framebuffer);
  String body = letter.body.length() ? letter.body : defaultLetter().body;
  if ((int)body.length() <= 180) drawWrappedCentered(body, 80, EPD_WIDTH - 80, 210, 58, 34, 5);
  else                           drawWrappedCentered(body, 72, EPD_WIDTH - 72, 170, 48, 42, 7);
}


const char *weatherIconStr(int code) {
  if (code == 0 || code == 1)              return WI_CLEAR;
  if (code == 2)                           return WI_PARTLY;
  if (code == 3)                           return WI_OVERCAST;
  if (code == 45 || code == 48)            return WI_FOG;
  if (code >= 51 && code <= 55)            return WI_DRIZZLE;
  if (code >= 61 && code <= 65)            return WI_RAIN;
  if (code >= 80 && code <= 82)            return WI_HEAVY_RAIN;
  if (code >= 95)                          return WI_THUNDER;
  return WI_OVERCAST;
}

void drawWeatherIcon(const char *icon, int x0, int x1, int y) {
  int32_t tw, th, ix = 0, iy = 0, ix1, iy1;
  get_text_bounds((GFXfont *)&WeatherIcons, icon, &ix, &iy, &ix1, &iy1, &tw, &th, NULL);
  int x = x0 + ((x1 - x0) - tw) / 2;
  writeln((GFXfont *)&WeatherIcons, icon, &x, &y, framebuffer);
}

void renderWeatherFull(const WeatherData &weather, const AppSettings &settings) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  drawDoubleBorder();
  char loc[80];
  settings.location_name.toCharArray(loc, sizeof(loc));
  drawCentered(loc, 20, EPD_WIDTH - 20, 48);
  epd_draw_hline(20, 74, EPD_WIDTH - 40, 0x00, framebuffer);

  const WeatherNow &now = weather.current;
  char temp[32];
  snprintf(temp, sizeof(temp), "%d°C", now.temperature_c);
  drawCentered(temp, 40, 420, 170);
  drawWeatherIcon(weatherIconStr(now.weather_code), 40, 420, 232);

  char fact[48];
  snprintf(fact, sizeof(fact), "chuva %d%%", now.rain_percent);
  drawAt(fact, 500, 130);
  snprintf(fact, sizeof(fact), "vento %d km/h", now.wind_kmh);
  drawAt(fact, 500, 190);
  snprintf(fact, sizeof(fact), "umid. %d%%", now.humidity_percent);
  drawAt(fact, 500, 250);

  epd_draw_hline(20, 300, EPD_WIDTH - 40, 0x00, framebuffer);
  int cardW = (EPD_WIDTH - 40) / 7;
  for (int i = 0; i < weather.day_count; i++) {
    int x = 20 + i * cardW;
    epd_draw_vline(x, 300, 210, 0x00, framebuffer);
    char day[16];
    weather.days[i].weekday.toCharArray(day, sizeof(day));
    drawCentered(day, x, x + cardW, 342);
    char range[24];
    snprintf(range, sizeof(range), "%d/%d", weather.days[i].max_c, weather.days[i].min_c);
    drawCentered(range, x, x + cardW, 402);
    drawWeatherIcon(weatherIconStr(weather.days[i].weather_code), x, x + cardW, 456);
  }
  epd_draw_vline(20 + 7 * cardW, 300, 210, 0x00, framebuffer);
}


String memoryImagePath(const String &fileName) {
  if (fileName.startsWith("/")) return fileName;
  return "/memories/" + fileName;
}

void renderMemory(const MemoryData &memory) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  const int M = 14;
  String path = memoryImagePath(memory.image_file.length() ? memory.image_file : defaultMemory().image_file);
  bool ok = drawBmp(path, M, M, EPD_WIDTH - 2 * M, EPD_HEIGHT - 2 * M);
  if (!ok) {
    String fallback = firstSupportedMemory();
    if (fallback.length() > 0 && memoryEntryPath(fallback) != path) {
      path = memoryEntryPath(fallback);
      ok = drawBmp(path, M, M, EPD_WIDTH - 2 * M, EPD_HEIGHT - 2 * M);
    }
  }
  if (!ok) {
    drawDoubleBorder();
    drawCentered("imagem não encontrada", 40, EPD_WIDTH - 40, 230);
    drawCentered(lastBmpError[0] ? lastBmpError : "sem BMP valido", 40, EPD_WIDTH - 40, 300);
  }
}


void renderError(const char *msg) {
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  drawDoubleBorder();
  drawCentered(msg, 0, EPD_WIDTH, 280);
}

void renderRecord(const TagRecord &rec, const struct tm *t, const WeatherNow &weather) {
  String displayType = rec.display_type;
  ScreenMode mode = parseDisplayMode(displayType);
  currentScreen = mode;
  switch (mode) {
    case SCREEN_LETTER:
      renderTextOnly(rec.message.length() ? rec.message : "Te amo tanto minha perereca.");
      break;
    case SCREEN_MEMORY: {
                          displayType.trim();
                          displayType.toLowerCase();
                          displayType.replace("-", "_");
                          displayType.replace(" ", "_");
                          if (displayType == "image_only" || displayType == "image") {
                            renderImageOnly(rec.image_file.length() ? rec.image_file : "memory.bmp");
                          } else {
                            renderSplitScreen(rec.message, rec.image_file.length() ? rec.image_file : "memory.bmp");
                          }
                          break;
                        }
    case SCREEN_CALENDAR:
    case SCREEN_WEATHER_FULL:
    case SCREEN_DASHBOARD:
    default:
                        renderDashboard(t, weather);
                        break;
  }
}


void renderCurrentScreen() {
  struct tm t;
  if (!getLocalTime(&t)) memset(&t, 0, sizeof(t));
  switch (currentScreen) {
    case SCREEN_CALENDAR:
      renderCalendar(&t);
      break;
    case SCREEN_LETTER:
      renderLetter(appData.letter);
      break;
    case SCREEN_WEATHER_FULL:
      renderWeatherFull(appData.weather, appData.settings);
      break;
    case SCREEN_MEMORY:
      renderMemory(appData.memory);
      break;
    case SCREEN_DASHBOARD:
    default:
      renderDashboard(&t, appData.weather.current);
      break;
  }
}


ScreenMode modeForDashboardTouch(int16_t x, int16_t y) {
  if (x < COL_DIV && y < ROW_DIV)  return SCREEN_CALENDAR;
  if (x >= COL_DIV && y < ROW_DIV) return SCREEN_LETTER;
  if (x < COL_DIV && y >= ROW_DIV) return SCREEN_WEATHER_FULL;
  return SCREEN_MEMORY;
}

void handleTouch() {
  if (!touchReady) return;
  const TouchPoints &pts = touch.getTouchPoints();
  if (touchWaitRelease) {
    if (!pts.hasPoints()) touchWaitRelease = false;
    return;
  }
  if (!pts.hasPoints()) return;
  touchWaitRelease = true;
  int16_t x = (int16_t)pts.getPoint(0).x;
  int16_t y = (int16_t)pts.getPoint(0).y;
  if (currentScreen == SCREEN_DASHBOARD) {
    currentScreen = modeForDashboardTouch(x, y);
  } else {
    currentScreen = SCREEN_DASHBOARD;
  }
  renderCurrentScreen();
  flushDisplay();
}


void flushDisplay() {
  epd_poweron();
  epd_clear();
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  epd_poweroff_all();
}


void setup() {
  Serial.begin(115200);
  delay(500);
  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) { Serial.println("PSRAM fail"); while (1); }
  epd_init();
  initSD();
  touchReady = initTouch();
  loadDefaultAppData();

  if (connectWiFi()) {
    struct tm t;
    if (syncTime(appData.settings, &t)) {
      fetchAppData(isoDate(&t), &appData);
      syncTime(appData.settings, &t);
      appData.weather = fetchWeather(appData.settings);
      String dailyImg = selectDailyMemory(&t);
      if (dailyImg.length() > 0) appData.memory.image_file = dailyImg;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  currentScreen = SCREEN_DASHBOARD;
  renderCurrentScreen();
  flushDisplay();

  struct tm tNow;
  if (getLocalTime(&tNow)) lastRenderedDay = tNow.tm_mday;
}

void loop() {
  handleTouch();

  struct tm t;
  if (getLocalTime(&t) && t.tm_mday != lastRenderedDay) {
    lastRenderedDay = t.tm_mday;
    String dailyImg = selectDailyMemory(&t);
    if (dailyImg.length() > 0) appData.memory.image_file = dailyImg;
    if (currentScreen == SCREEN_MEMORY) {
      renderCurrentScreen();
      flushDisplay();
    }
  }

  delay(30);
}
