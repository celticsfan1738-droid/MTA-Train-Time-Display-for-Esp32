/*
 * ============================================================
 *  MTA NYC Subway Arrival Display
 *  ESP32 WROOM + 3.5" 480x320 TFT Display (ILI9488)
 * ============================================================
 *
 * SETUP INSTRUCTIONS:
 *   1. Fill in your WiFi credentials below
 *   2. Set your STATION_NAME (shown at the bottom of the screen)
 *   3. Configure up to 4 train slots (TRAIN_COUNT = however many you want)
 *      For each train slot set:
 *        - LABEL     : Text shown in the header (e.g. "J to Manhattan")
 *        - STOP_ID   : The MTA GTFS stop ID for that train/direction
 *        - COLOR     : Header background color in RGB565 hex
 *        - FEED      : Which MTA feed to use (see FEED options below)
 *
 * ── HOW TO FIND YOUR STOP ID ────────────────────────────────
 *   Stop IDs are 4 characters: a letter + 2 digits + N or S
 *     N = toward Manhattan (northbound)
 *     S = away from Manhattan (southbound)
 *   Examples:
 *     J27N = J/Z train at Myrtle/Broadway toward Manhattan
 *     J27S = J/Z train at Myrtle/Broadway toward Jamaica/Queens
 *     M18N = M train at Myrtle/Broadway toward Manhattan
 *     M18S = M train at Myrtle/Broadway toward Middle Village/Queens
 *   Full stop list: https://www.transit.land/feeds/f-dr5r-nyct~subway
 *   Or search: "MTA GTFS stops.txt" + your station name
 *
 * ── FEED OPTIONS ────────────────────────────────────────────
 *   Use one of these feed name strings for each train slot:
 *     "jz"    = J and Z trains
 *     "bdfm"  = B, D, F, and M trains
 *     "ace"   = A, C, and E trains
 *     "nqrw"  = N, Q, R, and W trains
 *     "g"     = G train
 *     "l"     = L train
 *     "1234567" = 1,2,3,4,5,6,7 trains
 *     "si"    = Staten Island Railway
 *
 * ── COLOR REFERENCE (RGB565 hex) ────────────────────────────
 *   MTA official train colors:
 *     J/Z Brown   : 0x8B26
 *     M Orange    : 0xEB40
 *     A/C/E Blue  : 0x009F  (deep blue)
 *     B/D/F/M Orange: 0xEB40
 *     N/Q/R/W Yellow: 0xFF80
 *     G  Green    : 0x05E0
 *     L  Gray     : 0x8430
 *     1/2/3 Red   : 0xF800
 *     4/5/6 Green : 0x0400
 *     7  Purple   : 0x780F
 *   Custom color: convert #RRGGBB at https://rgbto565.com
 *
 * ── WIRING ──────────────────────────────────────────────────
 *   TFT VCC  -> 3.3V        TFT GND  -> GND
 *   TFT CS   -> GPIO 15     TFT RST  -> GPIO 4
 *   TFT DC   -> GPIO 2      TFT MOSI -> GPIO 23
 *   TFT MISO -> GPIO 19     TFT CLK  -> GPIO 18
 *   TFT LED  -> 3.3V
 *
 * ── LIBRARIES REQUIRED ──────────────────────────────────────
 *   Install via Arduino Library Manager:
 *     - TFT_eSPI by Bodmer
 *   Configure TFT_eSPI/User_Setup.h with your driver and pins.
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <time.h>

// ╔══════════════════════════════════════════════════════════╗
// ║                  USER CONFIGURATION                      ║
// ╚══════════════════════════════════════════════════════════╝

// ── WiFi ─────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ── Station name shown at bottom of screen ───────────────────
const char* STATION_NAME  = "Myrtle Av / Broadway";

// ── Number of train slots to display (1 to 4) ────────────────
#define TRAIN_COUNT 4

// ── Train Slot Configuration ─────────────────────────────────
// Format: { "Label on screen", "STOP_ID", 0xCOLOR, "feed" }
// See comments at top of file for stop IDs, colors, and feeds.

struct TrainConfig {
  const char* label;    // Header text shown on display
  const char* stopId;   // MTA GTFS stop ID
  uint32_t    color;    // Header background color (RGB565)
  const char* feed;     // MTA feed name (see options above)
};

const TrainConfig TRAINS[TRAIN_COUNT] = {
  { "J to Manhattan", "J27N", 0x8B26, "jz"   },
  { "J to Queens",    "J27S", 0x8B26, "jz"   },
  { "M to Manhattan", "M18N", 0xEB40, "bdfm" },
  { "M to Queens",    "M18S", 0xEB40, "bdfm" },
};

// ── Refresh interval (milliseconds) ──────────────────────────
const unsigned long REFRESH_INTERVAL = 30000; // 30 seconds

// ╚══════════════════════════════════════════════════════════╝
//   END OF USER CONFIGURATION — no need to edit below this
// ╚══════════════════════════════════════════════════════════╝

// ── MTA Feed URLs ─────────────────────────────────────────────
String getFeedUrl(const char* feedName) {
  String base = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2fgtfs-";
  String name = String(feedName);
  // The 1234567 feed has a different URL pattern
  if (name == "1234567") {
    return "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2fgtfs";
  }
  return base + name;
}

// ── Display ───────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

#define COL_BG        0x0000  // Black background
#define COL_DIVIDER   0x4208  // Dark gray divider
#define COL_TIME_TEXT 0xFFFF  // White time text
#define COL_LABEL_TEXT 0xFFFF // White header text

// Layout — 480x320 landscape
#define SCREEN_W  480
#define SCREEN_H  320
#define MAX_ARRIVALS 4

// Dynamic layout based on TRAIN_COUNT
// 1-2 trains: single row layout
// 3-4 trains: two row layout (2 per row)
#define TRAINS_PER_ROW  2
#define NUM_ROWS        ((TRAIN_COUNT + 1) / 2)
#define COL_W           (SCREEN_W / TRAINS_PER_ROW)  // 240px each
#define HDR_H           36
#define TIME_H          30
#define GAP_H           10
#define TIME_ROWS       3

// Y position for a given row section
int sectionHdrY(int row) {
  int sectionH = HDR_H + TIME_H * TIME_ROWS + GAP_H;
  return 4 + row * (sectionH + 6);
}
int sectionTimeY(int row) {
  return sectionHdrY(row) + HDR_H + 2;
}

// ── Arrival storage ───────────────────────────────────────────
struct ArrivalList {
  uint32_t times[MAX_ARRIVALS];
  int      count;
};

ArrivalList arrivals[TRAIN_COUNT];

unsigned long lastRefresh = 0;

// ── Protobuf parser ───────────────────────────────────────────
int readVarint(const uint8_t* buf, int len, int pos, uint64_t& val) {
  val = 0;
  int shift = 0;
  int start = pos;
  while (pos < len) {
    uint8_t b = buf[pos++];
    val |= ((uint64_t)(b & 0x7F)) << shift;
    shift += 7;
    if (!(b & 0x80)) break;
    if (shift > 63) break;
  }
  return pos - start;
}

int skipField(const uint8_t* buf, int len, int pos, int wireType) {
  if (pos >= len) return 0;
  uint64_t v; int n;
  switch (wireType) {
    case 0: n = readVarint(buf, len, pos, v); return n;
    case 1: return 8;
    case 2: n = readVarint(buf, len, pos, v); return n + (int)v;
    case 5: return 4;
    default: return len;
  }
}

uint32_t parseStopTimeEvent(const uint8_t* buf, int len) {
  int pos = 0; uint32_t t = 0;
  while (pos < len) {
    uint64_t tag64; int n = readVarint(buf, len, pos, tag64); pos += n;
    int fieldNum = (int)(tag64 >> 3), wireType = (int)(tag64 & 0x07);
    if (wireType == 0) {
      uint64_t v; n = readVarint(buf, len, pos, v); pos += n;
      if (fieldNum == 2) t = (uint32_t)v;
    } else { pos += skipField(buf, len, pos, wireType); }
  }
  return t;
}

void parseStopTimeUpdate(const uint8_t* buf, int len,
                         char* stop_id_out, int stop_id_max,
                         uint32_t& arrival_time_out) {
  int pos = 0; stop_id_out[0] = 0; arrival_time_out = 0;
  while (pos < len) {
    uint64_t tag64; int n = readVarint(buf, len, pos, tag64);
    if (n == 0) break; pos += n;
    int fieldNum = (int)(tag64 >> 3), wireType = (int)(tag64 & 0x07);
    if (wireType == 2) {
      uint64_t subLen; n = readVarint(buf, len, pos, subLen); pos += n;
      int slen = (int)subLen;
      if (slen < 0 || pos + slen > len) break;
      if (fieldNum == 4) {
        int cl = min(slen, stop_id_max - 1);
        memcpy(stop_id_out, buf + pos, cl); stop_id_out[cl] = 0;
      } else if (fieldNum == 2) {
        arrival_time_out = parseStopTimeEvent(buf + pos, slen);
      }
      pos += slen;
    } else if (wireType == 0) {
      uint64_t v; n = readVarint(buf, len, pos, v); pos += n;
    } else { pos += skipField(buf, len, pos, wireType); }
  }
}

// Parse a trip update — check all stop_time_updates against all configured stop IDs
void parseTripUpdate(const uint8_t* buf, int len) {
  int pos = 0;
  while (pos < len) {
    uint64_t tag64; int n = readVarint(buf, len, pos, tag64);
    if (n == 0) break; pos += n;
    int fieldNum = (int)(tag64 >> 3), wireType = (int)(tag64 & 0x07);
    if (wireType == 2) {
      uint64_t subLen; n = readVarint(buf, len, pos, subLen); pos += n;
      int slen = (int)subLen;
      if (slen < 0 || pos + slen > len) break;
      if (fieldNum == 2) { // stop_time_update
        char stop_id[12] = {0}; uint32_t arr_time = 0;
        parseStopTimeUpdate(buf + pos, slen, stop_id, sizeof(stop_id), arr_time);
        if (arr_time > 0) {
          for (int i = 0; i < TRAIN_COUNT; i++) {
            if (strcmp(stop_id, TRAINS[i].stopId) == 0 && arrivals[i].count < MAX_ARRIVALS) {
              arrivals[i].times[arrivals[i].count++] = arr_time;
            }
          }
        }
      }
      pos += slen;
    } else if (wireType == 0) {
      uint64_t v; n = readVarint(buf, len, pos, v); pos += n;
    } else { pos += skipField(buf, len, pos, wireType); }
  }
}

void parseFeedEntity(const uint8_t* buf, int len) {
  int pos = 0;
  while (pos < len) {
    uint64_t tag64; int n = readVarint(buf, len, pos, tag64);
    if (n == 0) break; pos += n;
    int fieldNum = (int)(tag64 >> 3), wireType = (int)(tag64 & 0x07);
    if (wireType == 2) {
      uint64_t subLen; n = readVarint(buf, len, pos, subLen); pos += n;
      int slen = (int)subLen;
      if (slen < 0 || pos + slen > len) break;
      if (fieldNum == 3) { // trip_update
        parseTripUpdate(buf + pos, slen);
      }
      pos += slen;
    } else if (wireType == 0) {
      uint64_t v; n = readVarint(buf, len, pos, v); pos += n;
    } else { pos += skipField(buf, len, pos, wireType); }
  }
}

void parseFeed(const uint8_t* buf, int len) {
  int pos = 0;
  while (pos < len) {
    uint64_t tag64; int n = readVarint(buf, len, pos, tag64);
    if (n == 0) break; pos += n;
    int fieldNum = (int)(tag64 >> 3), wireType = (int)(tag64 & 0x07);
    if (wireType == 2) {
      uint64_t subLen; n = readVarint(buf, len, pos, subLen); pos += n;
      int slen = (int)subLen;
      if (slen < 0 || pos + slen > len) break;
      if (fieldNum == 2) parseFeedEntity(buf + pos, slen); // entity
      pos += slen;
    } else if (wireType == 0) {
      uint64_t v; n = readVarint(buf, len, pos, v); pos += n;
    } else { pos += skipField(buf, len, pos, wireType); }
  }
}

void sortArrivals(ArrivalList& list) {
  for (int i = 0; i < list.count - 1; i++)
    for (int j = i + 1; j < list.count; j++)
      if (list.times[j] < list.times[i]) {
        uint32_t tmp = list.times[i]; list.times[i] = list.times[j]; list.times[j] = tmp;
      }
}

// ── HTTP Fetch ────────────────────────────────────────────────
bool fetchFeed(const String& url, uint8_t*& outBuf, int& outLen) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error %d for %s\n", code, url.c_str());
    http.end(); return false;
  }
  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (len > 0) {
    outBuf = (uint8_t*)malloc(len);
    if (!outBuf) { http.end(); return false; }
    outLen = stream->readBytes(outBuf, len);
  } else {
    const int CHUNK = 4096; int capacity = CHUNK;
    outBuf = (uint8_t*)malloc(capacity);
    if (!outBuf) { http.end(); return false; }
    outLen = 0;
    while (stream->available() || http.connected()) {
      int avail = stream->available();
      if (avail > 0) {
        if (outLen + avail > capacity) {
          capacity += CHUNK;
          outBuf = (uint8_t*)realloc(outBuf, capacity);
          if (!outBuf) { http.end(); return false; }
        }
        outLen += stream->readBytes(outBuf + outLen, avail);
      }
      delay(1);
    }
  }
  http.end();
  return outLen > 0;
}

// ── Time ──────────────────────────────────────────────────────
bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo; int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) { delay(500); attempts++; }
  return attempts < 20;
}

uint32_t getCurrentEpoch() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  return (uint32_t)mktime(&timeinfo);
}

// ── Display Helpers ───────────────────────────────────────────
String formatMinutes(uint32_t arrivalTime, uint32_t nowTime) {
  if (arrivalTime == 0) return "--";
  if (arrivalTime <= nowTime) return "Due";
  int mins = (int)((arrivalTime - nowTime) / 60);
  if (mins == 0) return "Due";
  if (mins == 1) return "1 min";
  return String(mins) + " min";
}

void drawHeaderCell(int x, int y, int w, int h, uint32_t bgColor, const char* label) {
  tft.fillRect(x, y, w, h, bgColor);
  tft.setTextColor(COL_LABEL_TEXT, bgColor);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2);
}

void drawTimeCell(int x, int y, int w, int h, const String& timeStr) {
  tft.fillRect(x, y, w, h, COL_BG);
  tft.setTextColor(COL_TIME_TEXT, COL_BG);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeStr, x + w / 2, y + h / 2);
}

// ── Main Draw ─────────────────────────────────────────────────
void drawDisplay(uint32_t nowEpoch) {
  tft.fillScreen(COL_BG);

  for (int t = 0; t < TRAIN_COUNT; t++) {
    int row = t / TRAINS_PER_ROW;   // which row section (0 or 1)
    int col = t % TRAINS_PER_ROW;   // which column (0 or 1)
    int x   = col * COL_W;
    int hdrY = sectionHdrY(row);
    int timeY = sectionTimeY(row);

    // Draw divider bar between sections
    if (row == 1 && col == 0) {
      tft.fillRect(0, hdrY - GAP_H, SCREEN_W, 5, 0x4208);
    }

    // Header
    drawHeaderCell(x, hdrY, COL_W, HDR_H, TRAINS[t].color, TRAINS[t].label);

    // Vertical divider between columns
    if (col == 0 && TRAIN_COUNT > 1) {
      tft.drawFastVLine(COL_W, hdrY, HDR_H + TIME_H * TIME_ROWS + 2, COL_DIVIDER);
    }

    // Time rows
    for (int row_t = 0; row_t < TIME_ROWS; row_t++) {
      int y = timeY + row_t * TIME_H;
      String timeStr = (row_t < arrivals[t].count)
                       ? formatMinutes(arrivals[t].times[row_t], nowEpoch)
                       : "--";
      drawTimeCell(x, y, COL_W, TIME_H, timeStr);
      tft.drawFastHLine(0, y + TIME_H - 1, SCREEN_W, COL_DIVIDER);
    }
  }

  // Status bar
  int statusY = sectionTimeY(NUM_ROWS - 1) + TIME_ROWS * TIME_H + 4;
  tft.setTextColor(0x7BEF, COL_BG);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  String status = String(STATION_NAME) + "  |  Updated every 30s";
  tft.drawString(status, SCREEN_W / 2, statusY + 8);
}

// ── Data Refresh ──────────────────────────────────────────────
void refreshData() {
  Serial.println("Refreshing MTA data...");

  // Reset all arrival lists
  for (int i = 0; i < TRAIN_COUNT; i++) arrivals[i].count = 0;

  // Collect unique feeds needed
  String feedsDone[TRAIN_COUNT];
  int feedCount = 0;

  for (int i = 0; i < TRAIN_COUNT; i++) {
    String feedName = String(TRAINS[i].feed);
    bool already = false;
    for (int j = 0; j < feedCount; j++) {
      if (feedsDone[j] == feedName) { already = true; break; }
    }
    if (already) continue;
    feedsDone[feedCount++] = feedName;

    String url = getFeedUrl(TRAINS[i].feed);
    uint8_t* buf = nullptr; int len = 0;
    if (fetchFeed(url, buf, len)) {
      Serial.printf("Feed '%s': %d bytes\n", TRAINS[i].feed, len);
      parseFeed(buf, len);
      free(buf);
    }
  }

  for (int i = 0; i < TRAIN_COUNT; i++) sortArrivals(arrivals[i]);

  for (int i = 0; i < TRAIN_COUNT; i++) {
    Serial.printf("  %s: %d arrivals\n", TRAINS[i].label, arrivals[i].count);
  }
}

// ── Setup & Loop ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting to WiFi...", SCREEN_W / 2, SCREEN_H / 2);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.drawString("WiFi FAILED!", SCREEN_W / 2, SCREEN_H / 2);
    while (true) delay(1000);
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  tft.fillScreen(TFT_BLACK);
  tft.drawString("Syncing time...", SCREEN_W / 2, SCREEN_H / 2);
  for (int i = 0; i < 5 && !syncTime(); i++) delay(2000);

  tft.fillScreen(TFT_BLACK);
  tft.drawString("Fetching train data...", SCREEN_W / 2, SCREEN_H / 2);
  refreshData();
  drawDisplay(getCurrentEpoch());
  lastRefresh = millis();
}

void loop() {
  if (millis() - lastRefresh >= REFRESH_INTERVAL) {
    refreshData();
    drawDisplay(getCurrentEpoch());
    lastRefresh = millis();
  }
  static unsigned long lastRedraw = 0;
  if (millis() - lastRedraw >= 15000) {
    drawDisplay(getCurrentEpoch());
    lastRedraw = millis();
  }
}
