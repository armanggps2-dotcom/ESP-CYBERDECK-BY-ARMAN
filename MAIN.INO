#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ESP DECK display performance update: tracked SPI bus switching + faster TFT transfer.

// Forward declaration: required by environments that auto-generate prototypes.
enum Scene : int;



// ---------- Pins ----------
constexpr int PIN_LEFT = 21, PIN_RIGHT = 20, PIN_A = 10, PIN_B = 9;
constexpr int SD_CS = 6, SD_MOSI = 5, SD_MISO = 7, SD_SCK = 2;
constexpr int TFT_CS = 4, TFT_DC = 3, TFT_RST = 8, TFT_SCK = 1, TFT_MOSI = 0;

// TFT LED -> 3V3
// TFT VCC -> 3V3

// ---------- Shared helpers (declared first so every section can use them) ----------
typedef struct Theme { uint16_t top, bot, accent, text; } Theme;

enum Scene : int { HOME, MENU, CATCH, MAZE, FOOTBALL, PJ, RUNNER, WIKI_SEARCH, WIKI_READ,
             DICT_SEARCH, DICT_READ, WIFI_LIST, WIFI_PASS, FILES, READER, PHOTOS,
             CLOCK, SETTINGS, NOTES, STOPWATCH, CALC, DICE, SNAKE, MSG, TTT, SIMON,SHOOTER,OFFLINE_SLEEP,SYSTEM_INFO, MAP_SEARCH, MAP_RESULT, WEATHER, NEON_RALLY, ASTEROIDS };

typedef struct FBody { float x, y, vx, vy; } FBody;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

bool wifiOn = false, timeReady = false;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 fb(160, 128);
const size_t FRAME_BYTES = 160 * 128 * 2;
// Photo/book-cover buffers are allocated only when those apps are used.
uint16_t* imgCur = NULL;
uint16_t* imgNext = NULL;
bool ensureImageBuffers() {
  if (imgCur && imgNext) return true;
  if (!imgCur) imgCur = (uint16_t*)malloc(FRAME_BYTES);
  if (!imgNext) imgNext = (uint16_t*)malloc(FRAME_BYTES);
  if (!imgCur || !imgNext) {
    if (imgCur) { free(imgCur); imgCur = NULL; }
    if (imgNext) { free(imgNext); imgNext = NULL; }
    return false;
  }
  return true;
}
void releaseImageBuffers() {
  if (imgCur) { free(imgCur); imgCur = NULL; }
  if (imgNext) { free(imgNext); imgNext = NULL; }
}

const unsigned long FRAME_MS = 33;
unsigned long lastFrame = 0, lastActivity = 0, frameNo = 0;
bool sdReady = false;

// ---------- Settings ----------
const long TZ_OFF[] = {19800, 0, 3600, 7200, 28800, -18000, -28800};
const char* TZ_NAME[] = {"IST +5:30", "UTC", "UTC+1", "UTC+2", "UTC+8", "UTC-5", "UTC-8"};
const int TZ_N = 7;
const unsigned long IDLE_OPT[] = {30000UL, 60000UL, 300000UL, 0UL};
const char* IDLE_NAME[] = {"30 sec", "1 min", "5 min", "never"};
const int IDLE_N = 4;
int tzIdx = 0, idleIdx = 1, themeIdx = 0;
bool wifiEnabled = true;
int setRow = 0;
String settingsMsg;


Theme TH[3] = {
  {rgb(30, 10, 70),  rgb(70, 20, 120), rgb(255, 150, 200), rgb(255, 245, 255)},
  {rgb(255, 170, 200), rgb(255, 230, 240), rgb(200, 60, 120), rgb(60, 20, 50)},
  {rgb(10, 60, 50),  rgb(40, 130, 100), rgb(160, 255, 200), rgb(230, 255, 240)}
};
#define T() (TH[themeIdx])

// ---------- Scenes ----------

Scene scene = MENU;

// ---------- Input ----------
enum : uint8_t { BTN_LEFT = 1, BTN_RIGHT = 2, BTN_A = 4, BTN_B = 8 };
uint8_t pad = 0, padPrev = 0;

void readPad() {
  padPrev = pad; pad = 0;
  if (!digitalRead(PIN_LEFT))  pad |= BTN_LEFT;
  if (!digitalRead(PIN_RIGHT)) pad |= BTN_RIGHT;
  if (!digitalRead(PIN_A))     pad |= BTN_A;
  if (!digitalRead(PIN_B))     pad |= BTN_B;
}
bool held(uint8_t b)    { return pad & b; }
bool pressed(uint8_t b) { return (pad & b) && !(padPrev & b); }

// ---------- SPI bus switching ----------
// Track which SPI pin map is active. Reinitialise only when switching devices.
bool displayBusActive = false;
unsigned long lastPresentMs = 0;
unsigned long maxPresentMs = 0;
void useSdBus() {
  if (displayBusActive) {
    SPI.end();
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    displayBusActive = false;
  }
}
void useDisplayBus() {
  if (!displayBusActive) {
    SPI.end();
    SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
    displayBusActive = true;
  }
}

void present() {
  // One full-frame transfer at a fixed colour level. No per-row brightness
  // conversion, so the UI stays consistent and avoids the slow dimming path.
  useDisplayBus();
  const unsigned long drawStart = millis();
  tft.drawRGBBitmap(0, 0, fb.getBuffer(), 160, 128);
  lastPresentMs = millis() - drawStart;
  if (lastPresentMs > maxPresentMs) maxPresentMs = lastPresentMs;
}

void themeBackground() {
  for (int y = 0; y < 128; y++) {
    uint8_t r1 = (T().top >> 11) << 3, g1 = ((T().top >> 5) & 63) << 2, b1 = (T().top & 31) << 3;
    uint8_t r2 = (T().bot >> 11) << 3, g2 = ((T().bot >> 5) & 63) << 2, b2 = (T().bot & 31) << 3;
    uint8_t r = r1 + (r2 - r1) * y / 127, g = g1 + (g2 - g1) * y / 127, b = b1 + (b2 - b1) * y / 127;
    fb.drawFastHLine(0, y, 160, rgb(r, g, b));
  }
}

void panel(int x, int y, int w, int h, uint16_t fill) {
  fb.fillRoundRect(x, y, w, h, 6, fill);
  fb.drawRoundRect(x, y, w, h, 6, T().accent);
}

void banner(const char* t1, const char* t2) {
  fb.fillRoundRect(14, 42, 132, 44, 6, rgb(20, 10, 40));
  fb.drawRoundRect(14, 42, 132, 44, 6, T().accent);
  fb.setTextColor(T().accent); fb.setCursor(40, 50); fb.print(t1);
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(24, 68); fb.print(t2);
}

void statusBar(bool time) {
  fb.fillRect(0, 0, 160, 10, rgb(10, 5, 25));
  fb.setTextColor(ST77XX_WHITE);
  struct tm tmv;
  if (time && timeReady && getLocalTime(&tmv, 5)) {
    fb.setCursor(3, 1);
    int h = tmv.tm_hour % 12; if (h == 0) h = 12;
    fb.print(h); fb.print(':'); if (tmv.tm_min < 10) fb.print('0'); fb.print(tmv.tm_min);
  }
  int bars = WiFi.status() == WL_CONNECTED ? 3 : 0;
  for (int i = 0; i < 3; i++) {
    int h = 2 + i * 2;
    fb.fillRect(140 + i * 4, 9 - h, 2, h, i < bars ? T().accent : rgb(80, 70, 100));
  }
}

bool endsWithCI(const String& s, const char* ext) {
  String a = s; a.toLowerCase(); return a.endsWith(ext);
}

// ================= BMP LOADER =================
bool loadBmp(const String& path, uint16_t* dst) {
  if (!dst) return false;
  memset(dst, 0, FRAME_BYTES);
  useSdBus();
  bool ok = false;
  File f = SD.open(path);
  if (f) {
    uint8_t hdr[54];
    if (f.read(hdr, 54) == 54 && hdr[0] == 'B' && hdr[1] == 'M') {
      uint32_t dataOff = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                         ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
      int32_t w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
                            ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
      int32_t h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
                            ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
      uint16_t bpp = hdr[28] | (hdr[29] << 8);
      uint32_t comp = (uint32_t)hdr[30] | ((uint32_t)hdr[31] << 8);
      if (bpp == 24 && comp == 0 && w > 0 && w <= 160 && h != 0) {
        bool bottomUp = h > 0;
        int32_t absH = h > 0 ? h : -h;
        int rowSize = ((w * 3 + 3) / 4) * 4;
        int dx = (160 - w) / 2;
        int dy = (absH < 128) ? (128 - absH) / 2 : 0;
        static uint8_t row[640];
        f.seek(dataOff);
        for (int32_t i = 0; i < absH; i++) {
          if (f.read(row, rowSize) != rowSize) break;
          int32_t sy = bottomUp ? (absH - 1 - i) : i;
          int32_t py = sy + dy;
          if (py < 0 || py >= 128) continue;
          for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
            dst[py * 160 + x + dx] = rgb(r, g, b);
          }
        }
        ok = true;
      }
    }
    f.close();
  }
  useDisplayBus();
  return ok;
}

void slideBuffers(const uint16_t* from, const uint16_t* to, int dir) {
  if (!from || !to) return;
  uint16_t* out = fb.getBuffer();
  for (int step = 1; step <= 8; step++) {
    int off = step * 160 / 8;
    for (int y = 0; y < 128; y++)
      for (int x = 0; x < 160; x++) {
        int sx = dir > 0 ? x + off : x - off;
        uint16_t v;
        if (dir > 0) v = (sx < 160) ? from[y * 160 + sx] : to[y * 160 + sx - 160];
        else         v = (sx >= 0)  ? from[y * 160 + sx] : to[y * 160 + sx + 160];
        out[y * 160 + x] = v;
      }
    present();
    delay(20);
  }
}

// ================= ON-SCREEN KEYBOARD =================
const char KB[] = "abcdefghijklmnopqrstuvwxyz0123456789 .-_@!&<>";
const int KBN = sizeof(KB) - 1;
int kbIdx = 0;
String kbText;
unsigned long kbRep = 0;

bool kbUpdate() {
  unsigned long now = millis();
  if (pressed(BTN_LEFT)  || (held(BTN_LEFT)  && now - kbRep > 150)) { kbIdx = (kbIdx + KBN - 1) % KBN; kbRep = now; }
  if (pressed(BTN_RIGHT) || (held(BTN_RIGHT) && now - kbRep > 150)) { kbIdx = (kbIdx + 1) % KBN; kbRep = now; }
  if (pressed(BTN_A)) {
    char c = KB[kbIdx];
    if (c == '<') { if (kbText.length()) kbText.remove(kbText.length() - 1); }
    else if (c == '>') return true;
    else if (kbText.length() < 60) kbText += c;
  }
  return false;
}

void kbDraw(const char* prompt) {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print(prompt);
  fb.fillRoundRect(4, 18, 152, 18, 4, ST77XX_WHITE);
  String shown = kbText;
  if (shown.length() > 22) shown = shown.substring(shown.length() - 22);
  fb.setTextColor(ST77XX_BLACK); fb.setCursor(8, 23);
  fb.print(shown); fb.print("_");
  for (int off = -4; off <= 4; off++) {
    int idx = (kbIdx + off + KBN * 2) % KBN;
    int x = 80 + off * 17 - 8;
    bool cur = (off == 0);
    fb.fillRoundRect(x, 54, 16, 22, 4, cur ? T().accent : rgb(60, 40, 90));
    fb.setTextColor(cur ? rgb(20, 10, 30) : ST77XX_WHITE);
    fb.setCursor(x + 5, 61); fb.print(KB[idx]);
  }
  fb.setTextColor(T().text); fb.setCursor(4, 90); fb.print("< del    > OK");
  fb.setTextColor(T().text); fb.setCursor(4, 110); fb.print("L/R pick  A type  B back");
}

// ================= URL ENCODING =================
String urlEnc(const String& q) {
  String enc = "";
  for (int i = 0; i < (int)q.length(); i++) {
    char c = q[i];
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.') enc += c;
    else if (c == ' ') enc += '+';
    else { char b[4]; sprintf(b, "%%%02X", (uint8_t)c); enc += b; }
  }
  return enc;
}

// ================= WI-FI =================
String wifiSsid, wifiPass, wifiMsg;
String netNames[12];
int netRssi[12], netCount = 0, netSel = 0;

void loadWifiCreds() {
  if (!sdReady) return;
  useSdBus();
  File f = SD.open("/wifi.txt");
  if (f) {
    wifiSsid = f.readStringUntil('\n');
    wifiPass = f.readStringUntil('\n');
    f.close();
  }
  useDisplayBus();
  wifiSsid.trim(); wifiPass.trim();
}

void saveWifiCreds() {
  if (!sdReady) return;
  useSdBus();
  SD.remove("/wifi.txt");
  File f = SD.open("/wifi.txt", FILE_WRITE);
  if (f) { f.println(wifiSsid); f.println(wifiPass); f.close(); }
  useDisplayBus();
}

void scanNetworks() {
  if (!wifiEnabled) { wifiMsg = "Turn Wi-Fi on in Settings"; return; }
  banner("SCANNING", "please wait");
  present();
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) n = 0;
  netCount = min(n, 12);
  for (int i = 0; i < netCount; i++) {
    netNames[i] = WiFi.SSID(i);
    netRssi[i] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  netSel = 0;
}

void applyTimeZone() {
  if (!wifiOn) return;
  configTime(TZ_OFF[tzIdx], 0, "pool.ntp.org", "time.nist.gov");
  struct tm tmv;
  timeReady = getLocalTime(&tmv, 8000);
}

bool wifiConnect() {
  if (!wifiEnabled) { wifiMsg = "Wi-Fi is off"; return false; }
  if (WiFi.status() == WL_CONNECTED) { wifiOn = true; return true; }
  if (wifiSsid.length() == 0) return false;
  banner("CONNECTING", wifiSsid.substring(0, 18).c_str());
  present();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  wifiOn = (WiFi.status() == WL_CONNECTED);
  if (wifiOn && !timeReady) applyTimeZone();
  return wifiOn;
}

void wifiOffNow() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  timeReady = false;
}

void wifiApplyEnabled() {
  if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
    if (wifiSsid.length()) wifiConnect();
  } else {
    wifiOffNow();
  }
}

void forgetWifi() {
  WiFi.disconnect(true);
  wifiOn = false;
  wifiSsid = ""; wifiPass = "";
  if (sdReady) { useSdBus(); SD.remove("/wifi.txt"); useDisplayBus(); }
  settingsMsg = "Wi-Fi forgotten";
}

void wifiListUpdate() {
  int n = max(1, netCount);
  if (pressed(BTN_LEFT))  netSel = (netSel + n - 1) % n;
  if (pressed(BTN_RIGHT)) netSel = (netSel + 1) % n;
  if (pressed(BTN_A)) {
    if (netCount == 0) { scanNetworks(); return; }
    wifiSsid = netNames[netSel];
    kbText = ""; kbIdx = 0;
    scene = WIFI_PASS;
  }
}

void wifiListDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("WI-FI");
  fb.setCursor(96, 2); fb.print(wifiOn ? "ONLINE" : "OFFLINE");
  fb.setTextColor(T().text); fb.setCursor(4, 16);
  fb.print(wifiSsid.length() ? wifiSsid.substring(0, 22) : String("no network saved"));
  if (netCount == 0) {
    fb.setCursor(4, 40); fb.print("No networks. A = scan");
  } else {
    int first = max(0, min(netSel - 3, netCount - 6));
    for (int i = 0; i < 6 && first + i < netCount; i++) {
      int idx = first + i, y = 28 + i * 13;
      bool sel = (idx == netSel);
      panel(4, y, 152, 11, sel ? T().accent : rgb(60, 30, 70));
      fb.setTextColor(sel ? rgb(20, 10, 30) : ST77XX_WHITE);
      fb.setCursor(8, y + 2); fb.print(netNames[idx].substring(0, 18));
      fb.setCursor(132, y + 2); fb.print(netRssi[idx]);
    }
  }
  fb.setTextColor(T().accent); fb.setCursor(4, 104); fb.print(wifiMsg.substring(0, 24));
  fb.setTextColor(T().text); fb.setCursor(4, 116); fb.print("L/R pick  A join  B menu");
}

void wifiPassUpdate() {
  if (!kbUpdate()) return;
  wifiPass = kbText;
  saveWifiCreds();
  WiFi.disconnect(true);
  wifiOn = false;
  wifiMsg = wifiConnect() ? "Connected!" : "Failed - check password";
  scene = WIFI_LIST;
}

void wifiPassDraw() {
  String p = "JOIN " + wifiSsid;
  kbDraw(p.substring(0, 24).c_str());
}

// ================= TEXT WRAP =================
String wrapText(const String& s, int w) {
  String out, word;
  int col = 0;
  for (int i = 0; i <= (int)s.length(); i++) {
    char c = (i < (int)s.length()) ? s[i] : ' ';
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ') {
      if (word.length()) {
        if (col > 0 && col + 1 + (int)word.length() > w) { out += '\n'; col = 0; }
        else if (col > 0) { out += ' '; col++; }
        out += word;
        col += word.length();
        word = "";
      }
    } else {
      word += c;
    }
  }
  return out;
}

String asciiFold(const String& s) {
  String out;
  for (int i = 0; i < (int)s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) { out += (char)c; continue; }
    if (c == 0xC3 && i + 1 < (int)s.length()) {
      uint8_t n = s[++i];
      switch (n) {
        case 0x81: case 0xA1: out += (n == 0x81 ? 'A' : 'a'); break;
        case 0x89: case 0xA9: out += (n == 0x89 ? 'E' : 'e'); break;
        case 0x8D: case 0xAD: out += (n == 0x8D ? 'I' : 'i'); break;
        case 0x93: case 0xB3: out += (n == 0x93 ? 'O' : 'o'); break;
        case 0x9A: case 0xBA: out += (n == 0x9A ? 'U' : 'u'); break;
        case 0x9C: case 0xBC: out += (n == 0x9C ? 'U' : 'u'); break;
        case 0x91: case 0xB1: out += (n == 0x91 ? 'N' : 'n'); break;
        default: break;
      }
    }
  }
  return out;
}

// ================= WIKIPEDIA =================
String wikiTitle, wikiExtract, wikiText, wikiErr;
int wikiLS[80], wikiLineN = 0, wikiPageN = 0, wikiPage = 0;

void wikiLayout() {
  wikiText = wrapText(wikiExtract, 26);
  wikiLineN = 0;
  wikiLS[wikiLineN++] = 0;
  for (int i = 0; i < (int)wikiText.length() && wikiLineN < 80; i++)
    if (wikiText[i] == '\n') wikiLS[wikiLineN++] = i + 1;
  wikiPageN = (wikiLineN + 8) / 9;
  wikiPage = 0;
}

bool wikiFetch(const String& q) {
  if (!wifiConnect()) { wikiErr = "No Wi-Fi. Turn on in Settings"; return false; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.begin(client, String("https://en.wikipedia.org/api/rest_v1/page/summary/") + urlEnc(q));
  http.setUserAgent("ESP32C3-Arcade/1.0 (personal hobby project)");
  int code = http.GET();
  if (code != 200) { http.end(); wikiErr = "No article found"; return false; }

  JsonDocument filter;
  filter["title"] = true;
  filter["extract"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { wikiErr = "Could not read article"; return false; }

  wikiTitle = doc["title"].as<String>();
  wikiExtract = doc["extract"].as<String>();
  if (wikiExtract.length() > 1000) wikiExtract = wikiExtract.substring(0, 1000);
  if (wikiExtract.length() == 0) { wikiErr = "Empty article"; return false; }
  return true;
}

void wikiSearchUpdate() {
  if (!kbUpdate()) return;
  String q = kbText;
  q.trim();
  if (q.length() == 0) { wikiErr = "Type something first"; return; }
  banner("SEARCHING", q.substring(0, 18).c_str());
  present();
  if (wikiFetch(q)) {
    wikiErr = "";
    wikiLayout();
    scene = WIKI_READ;
  }
}

void wikiSearchDraw() {
  kbDraw("WIKIPEDIA SEARCH");
  if (wikiErr.length()) {
    fb.setTextColor(T().accent); fb.setCursor(4, 100);
    fb.print(wikiErr.substring(0, 24));
  }
}

void wikiReadUpdate() {
  if (pressed(BTN_RIGHT) && wikiPage + 1 < wikiPageN) wikiPage++;
  if (pressed(BTN_LEFT) && wikiPage > 0) wikiPage--;
}

void wikiReadDraw() {
  fb.fillScreen(rgb(255, 250, 240));
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(3, 2);
  fb.print(wikiTitle.substring(0, 24));
  fb.setTextColor(ST77XX_BLACK);
  for (int k = 0; k < 9; k++) {
    int ln = wikiPage * 9 + k;
    if (ln >= wikiLineN) break;
    int s = wikiLS[ln];
    int e = (ln + 1 < wikiLineN) ? wikiLS[ln + 1] - 1 : (int)wikiText.length();
    fb.setCursor(4, 18 + k * 11);
    fb.print(wikiText.substring(s, e));
  }
  fb.fillRect(0, 116, 160, 12, rgb(230, 230, 240));
  fb.setTextColor(ST77XX_BLACK); fb.setCursor(3, 118);
  fb.print("Page "); fb.print(wikiPage + 1); fb.print("/"); fb.print(wikiPageN);
  fb.print("  L/R  B search");
}

// ================= DICTIONARY (English to Spanish) =================
String dictWord, dictSpanish, dictErr;
int dictPageN = 0, dictPage = 0;
String dictText;
int dictLS[20], dictLineN = 0;

bool dictFetch(const String& word) {
  if (!wifiConnect()) { dictErr = "No Wi-Fi. Turn on in Settings"; return false; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  String url = String("https://api.mymemory.translated.net/get?q=") + urlEnc(word) + "&langpair=en|es";
  http.begin(client, url);
  http.setUserAgent("ESP32C3-Arcade/1.0 (personal hobby project)");
  int code = http.GET();
  if (code != 200) { http.end(); dictErr = "Translation service error"; return false; }

  JsonDocument filter;
  filter["responseData"]["translatedText"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { dictErr = "Could not read reply"; return false; }

  String t = doc["responseData"]["translatedText"].as<String>();
  if (t.length() == 0) { dictErr = "No translation found"; return false; }
  dictSpanish = asciiFold(t);
  return true;
}

void dictLayout() {
  dictText = wrapText(dictSpanish, 26);
  dictLineN = 0;
  dictLS[dictLineN++] = 0;
  for (int i = 0; i < (int)dictText.length() && dictLineN < 20; i++)
    if (dictText[i] == '\n') dictLS[dictLineN++] = i + 1;
  dictPageN = (dictLineN + 4) / 5;
  dictPage = 0;
}

void dictSearchUpdate() {
  if (!kbUpdate()) return;
  String w = kbText;
  w.trim();
  if (w.length() == 0) { dictErr = "Type a word first"; return; }
  banner("TRANSLATING", w.substring(0, 18).c_str());
  present();
  dictWord = w;
  if (dictFetch(w)) {
    dictErr = "";
    dictLayout();
    scene = DICT_READ;
  }
}

void dictSearchDraw() {
  kbDraw("DICTIONARY  EN>ES");
  if (dictErr.length()) {
    fb.setTextColor(T().accent); fb.setCursor(4, 100);
    fb.print(dictErr.substring(0, 24));
  }
}

void dictReadUpdate() {
  if (pressed(BTN_RIGHT) && dictPage + 1 < dictPageN) dictPage++;
  if (pressed(BTN_LEFT) && dictPage > 0) dictPage--;
  if (pressed(BTN_A)) { kbText = ""; kbIdx = 0; scene = DICT_SEARCH; }
}

void dictReadDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("SPANISH");
  panel(4, 16, 152, 22, rgb(20, 10, 40));
  fb.setTextColor(T().accent); fb.setCursor(8, 20);
  fb.print(dictWord.substring(0, 22));
  fb.setTextColor(ST77XX_WHITE);
  for (int k = 0; k < 5; k++) {
    int ln = dictPage * 5 + k;
    if (ln >= dictLineN) break;
    int s = dictLS[ln];
    int e = (ln + 1 < dictLineN) ? dictLS[ln + 1] - 1 : (int)dictText.length();
    fb.setCursor(6, 46 + k * 12);
    fb.print(dictText.substring(s, e));
  }
  fb.setTextColor(T().text); fb.setCursor(4, 116);
  fb.print("Page "); fb.print(dictPage + 1); fb.print("/"); fb.print(dictPageN);
  fb.print("  A new  B menu");
}

// ================= MESSAGES (chat over Wi-Fi using ntfy.sh) =================
String ntfyTopic = "EspDeck2312"; // editable from Settings; saved on SD card
bool ntfyEditMode = false;
const char* CHAT_NAME  = "ESP";
const int CHAT_MAX = 8;
String chatLines[CHAT_MAX];
int chatCount = 0;
String chatLastId = "";
bool msgCompose = false;
unsigned long msgNextPoll = 0;
String msgStatus = "";

bool validNtfyTopic(const String& topic) {
  if (topic.length() < 3 || topic.length() > 48) return false;
  for (unsigned int i = 0; i < topic.length(); ++i) {
    char c = topic[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

void loadNtfyTopic() {
  if (!sdReady) return;
  useSdBus();
  File f = SD.open("/ntfy_topic.txt", FILE_READ);
  if (f) {
    String saved = f.readStringUntil('\n');
    f.close(); saved.trim();
    if (validNtfyTopic(saved)) ntfyTopic = saved;
  }
  useDisplayBus();
}

bool saveNtfyTopic(const String& topic) {
  if (!validNtfyTopic(topic)) return false;
  ntfyTopic = topic;
  if (!sdReady) return true; // session-only if no SD card is present
  useSdBus();
  SD.remove("/ntfy_topic.txt");
  File f = SD.open("/ntfy_topic.txt", FILE_WRITE);
  if (!f) { useDisplayBus(); return false; }
  f.println(ntfyTopic); f.close();
  useDisplayBus();
  return true;
}

void chatAdd(const String& s) {
  if (chatCount < CHAT_MAX) { chatLines[chatCount++] = s; return; }
  for (int i = 1; i < CHAT_MAX; i++) chatLines[i - 1] = chatLines[i];
  chatLines[CHAT_MAX - 1] = s;
}

bool chatSend(const String& text) {
  if (!wifiConnect()) { msgStatus = "No Wi-Fi"; return false; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(client, String("https://ntfy.sh/") + ntfyTopic);
  http.addHeader("Title", CHAT_NAME);
  int code = http.POST(text);
  http.end();
  if (code == 200) return true;
  msgStatus = "Send failed";
  return false;
}

void chatPoll() {
  if (!wifiOn || WiFi.status() != WL_CONNECTED) { msgStatus = "No Wi-Fi"; return; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  String url = String("https://ntfy.sh/") + ntfyTopic + "/json?poll=1";
  url += chatLastId.length() ? "&since=" + chatLastId : String("&since=10m");
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) { http.end(); msgStatus = "Sync failed"; return; }
  String body = http.getString();
  http.end();

  int start = 0;
  while (start < (int)body.length()) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    String line = body.substring(start, nl);
    start = nl + 1;
    if (line.length() < 5) continue;
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    if (d["event"].as<String>() != "message") continue;
    chatLastId = d["id"].as<String>();
    if (d["title"].as<String>() == CHAT_NAME) continue;
    chatAdd(asciiFold(d["message"].as<String>()));
  }
  msgStatus = "";
}

void msgUpdate() {
  unsigned long now = millis();
  if (!msgCompose) {
    if (pressed(BTN_A)) { msgCompose = true; kbText = ""; kbIdx = 0; return; }
    if (now >= msgNextPoll && wifiEnabled) {
      msgStatus = "syncing";
      chatPoll();
      msgNextPoll = now + 8000;
    } else if (!wifiEnabled) {
      msgStatus = "Wi-Fi off";
    }
    return;
  }
  if (kbUpdate()) {
    String t = kbText;
    t.trim();
    if (t.length() && chatSend(t)) chatAdd(String(CHAT_NAME) + ": " + t);
    kbText = "";
    msgCompose = false;
  }
}

void msgDraw() {
  if (msgCompose) { kbDraw("MESSAGE  (> send)"); return; }
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("MESSAGES");
  fb.setCursor(96, 2); fb.print(msgStatus.substring(0, 8));
  if (chatCount == 0) {
    fb.setTextColor(T().text); fb.setCursor(4, 40); fb.print("No messages yet");
  }
  int first = max(0, chatCount - 6);
  for (int i = first; i < chatCount; i++) {
    fb.setTextColor(ST77XX_WHITE);
    fb.setCursor(4, 16 + (i - first) * 13);
    fb.print(chatLines[i].substring(0, 25));
  }
  fb.setTextColor(T().text); fb.setCursor(4, 116); fb.print("A write   B menu");
}

// ================= TIC-TAC-TOE =================
char tttB[9];
int tttCur = 4;
bool tttOver = false;
int tttWins = 0, tttLoss = 0;
String tttMsg;

void tttInit() {
  for (int i = 0; i < 9; i++) tttB[i] = ' ';
  tttCur = 4; tttOver = false; tttMsg = "";
}

char tttWinner() {
  static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
  for (auto& l : L)
    if (tttB[l[0]] != ' ' && tttB[l[0]] == tttB[l[1]] && tttB[l[1]] == tttB[l[2]]) return tttB[l[0]];
  for (int i = 0; i < 9; i++) if (tttB[i] == ' ') return 0;
  return 'D';
}

int tttFind(char p) {
  static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
  for (auto& l : L) {
    int cnt = 0, empty = -1;
    for (int k = 0; k < 3; k++) {
      if (tttB[l[k]] == p) cnt++;
      else if (tttB[l[k]] == ' ') empty = l[k];
    }
    if (cnt == 2 && empty >= 0) return empty;
  }
  return -1;
}

void tttComputer() {
  int m = tttFind('O');
  if (m < 0) m = tttFind('X');
  if (m < 0 && tttB[4] == ' ') m = 4;
  if (m < 0) {
    int e[9], n = 0;
    for (int i = 0; i < 9; i++) if (tttB[i] == ' ') e[n++] = i;
    if (n) m = e[random(n)];
  }
  if (m >= 0) tttB[m] = 'O';
}

void tttUpdate() {
  if (tttOver) { if (pressed(BTN_A)) tttInit(); return; }
  if (pressed(BTN_LEFT))  tttCur = (tttCur + 8) % 9;
  if (pressed(BTN_RIGHT)) tttCur = (tttCur + 1) % 9;
  if (pressed(BTN_A) && tttB[tttCur] == ' ') {
    tttB[tttCur] = 'X';
    char w = tttWinner();
    if (!w) { tttComputer(); w = tttWinner(); }
    if (w) {
      tttOver = true;
      if (w == 'X')      { tttWins++; tttMsg = "You win!"; }
      else if (w == 'O') { tttLoss++; tttMsg = "Computer wins"; }
      else               { tttMsg = "Draw"; }
    }
  }
}

void tttDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("TIC-TAC-TOE");
  fb.setCursor(104, 2); fb.print("W"); fb.print(tttWins); fb.print(" L"); fb.print(tttLoss);
  for (int i = 0; i < 9; i++) {
    int x = 30 + (i % 3) * 32, y = 18 + (i / 3) * 24;
    bool cur = (i == tttCur);
    panel(x, y, 28, 20, cur ? T().accent : rgb(60, 30, 70));
    fb.setTextColor(cur ? rgb(20, 10, 30) : ST77XX_WHITE);
    fb.setCursor(x + 10, y + 6);
    fb.print(tttB[i] == ' ' ? '.' : tttB[i]);
  }
  fb.setTextColor(T().accent); fb.setCursor(4, 96); fb.print(tttMsg);
  fb.setTextColor(T().text); fb.setCursor(4, 116);
  fb.print(tttOver ? "A again   B menu" : "L/R move  A play");
}

// ================= SIMON =================
const uint16_t SIM_COL[3] = {rgb(255, 70, 90), rgb(70, 200, 255), rgb(255, 220, 70)};
int simSeq[30], simLen = 0, simStep = 0, simPhase = 0, simLit = -1, simBest = 0;
int simFlashColor = -1;
unsigned long simT = 0, simFlashUntil = 0;

void simNext() {
  if (simLen < 30) simSeq[simLen++] = random(3);
  simStep = 0; simPhase = 0; simT = millis(); simLit = -1;
}

void simInit() { simLen = 0; simFlashColor = -1; simNext(); }

void simUpdate() {
  unsigned long now = millis();
  if (simPhase == 0) {
    int idx = (now - simT) / 500;
    if (idx >= simLen * 2 + 1) { simPhase = 1; simLit = -1; return; }
    simLit = (idx % 2 == 0 && idx / 2 < simLen) ? simSeq[idx / 2] : -1;
    return;
  }
  if (simPhase == 2) { if (pressed(BTN_A)) simInit(); return; }

  int b = -1;
  if (pressed(BTN_LEFT))  b = 0;
  if (pressed(BTN_RIGHT)) b = 1;
  if (pressed(BTN_A))     b = 2;
  if (b < 0) return;
  simFlashColor = b;
  simFlashUntil = now + 200;
  if (b == simSeq[simStep]) {
    simStep++;
    if (simStep == simLen) {
      if (simLen > simBest) simBest = simLen;
      simNext();
    }
  } else {
    simPhase = 2;
  }
}

void simDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("SIMON");
  fb.setCursor(96, 2); fb.print("BEST "); fb.print(simBest);
  int active = -1;
  if (simPhase == 0) active = simLit;
  else if (simFlashColor >= 0 && millis() < simFlashUntil) active = simFlashColor;
  for (int i = 0; i < 3; i++) {
    int x = 10 + i * 50;
    fb.fillRoundRect(x, 26, 44, 44, 6, i == active ? SIM_COL[i] : rgb(60, 50, 80));
  }
  fb.setTextColor(T().text); fb.setCursor(4, 80);
  if (simPhase == 0)      fb.print("Watch the colors...");
  else if (simPhase == 1) fb.print("Your turn!");
  else { fb.print("Missed! Score "); fb.print(simLen - 1); }
  fb.setCursor(4, 116);
  fb.print(simPhase == 2 ? "A retry  B menu" : "L red R blue A yellow");
}

// ================= PIXEL JUMP =================
const int PJ_COLS = 40, PJ_ROWS = 16;
bool pjGot[PJ_COLS][PJ_ROWS];
float pjX, pjY, pjVX, pjVY;
bool pjGround, pjOver, pjWon;
int pjScore, pjLives, pjCam;

char pjTile(int c, int r) {
  if (c < 0 || c >= PJ_COLS || r < 0 || r >= PJ_ROWS) return '.';
  if (r >= 14 && !(c % 13 == 6 || c % 13 == 7)) return '#';
  if (r == 10 && c % 9 >= 3 && c % 9 <= 5) return '#';
  if (c == 38 && r == 13) return 'G';
  return '.';
}

bool pjCoin(int c, int r) {
  if (c < 0 || c >= PJ_COLS || r < 0 || r >= PJ_ROWS) return false;
  if (pjGot[c][r]) return false;
  return (c % 5 == 2 && r == 8) || (c % 9 == 4 && r == 9);
}

bool pjHit(float x, float y) {
  int c1 = (int)floorf(x / 8.0f), c2 = (int)floorf((x + 5.0f) / 8.0f);
  int r1 = (int)floorf(y / 8.0f), r2 = (int)floorf((y + 7.0f) / 8.0f);
  for (int c = c1; c <= c2; c++)
    for (int r = r1; r <= r2; r++)
      if (pjTile(c, r) == '#') return true;
  return false;
}

void pjInit() {
  memset(pjGot, 0, sizeof(pjGot));
  pjX = 16; pjY = 96; pjVX = pjVY = 0;
  pjGround = false; pjOver = pjWon = false;
  pjScore = 0; pjLives = 3; pjCam = 0;
}

void pjUpdate() {
  if (pjOver) { if (pressed(BTN_A)) pjInit(); return; }
  pjVX = 0;
  if (held(BTN_LEFT))  pjVX = -1.8f;
  if (held(BTN_RIGHT)) pjVX =  1.8f;
  if (pressed(BTN_A) && pjGround) { pjVY = -5.6f; pjGround = false; }
  pjVY += 0.35f;
  if (pjVY > 6.0f) pjVY = 6.0f;

  float nx = pjX + pjVX;
  if (!pjHit(nx, pjY)) pjX = nx;
  pjX = constrain(pjX, 0.0f, PJ_COLS * 8.0f - 6.0f);

  float ny = pjY + pjVY;
  if (!pjHit(pjX, ny)) { pjY = ny; pjGround = false; }
  else { if (pjVY > 0) pjGround = true; pjVY = 0; }

  int cc = (int)((pjX + 3) / 8), cr = (int)((pjY + 4) / 8);
  if (pjCoin(cc, cr)) { pjGot[cc][cr] = true; pjScore++; }
  if (pjTile(cc, cr) == 'G') { pjWon = true; pjOver = true; }

  if (pjY > PJ_ROWS * 8) {
    if (--pjLives <= 0) pjOver = true;
    else { pjX = 16; pjY = 96; pjVX = pjVY = 0; }
  }
  pjCam = constrain((int)pjX - 60, 0, PJ_COLS * 8 - 160);
}

void pjDraw() {
  fb.fillRect(0, 0, 160, 128, rgb(110, 190, 255));
  for (int c = pjCam / 8; c <= (pjCam + 159) / 8 + 1 && c < PJ_COLS; c++) {
    for (int r = 0; r < PJ_ROWS; r++) {
      int sx = c * 8 - pjCam, sy = r * 8;
      char t = pjTile(c, r);
      if (t == '#') {
        fb.fillRect(sx, sy, 8, 8, rgb(150, 90, 50));
        if (pjTile(c, r - 1) != '#') fb.fillRect(sx, sy, 8, 2, rgb(80, 200, 80));
      } else if (t == 'G') {
        fb.fillRect(sx + 3, sy, 2, 8, ST77XX_WHITE);
        fb.fillTriangle(sx + 5, sy, sx + 8, sy + 2, sx + 5, sy + 4, rgb(80, 255, 120));
      }
      if (pjCoin(c, r)) fb.fillCircle(sx + 4, sy + 4, 2, ST77XX_YELLOW);
    }
  }
  int px = (int)pjX - pjCam, py = (int)pjY;
  fb.fillRect(px, py, 6, 8, rgb(255, 140, 40));
  fb.fillRect(px + 1, py + 2, 1, 1, ST77XX_BLACK);
  fb.fillRect(px + 4, py + 2, 1, 1, ST77XX_BLACK);
  fb.setTextColor(ST77XX_YELLOW); fb.setCursor(2, 2); fb.print("COINS "); fb.print(pjScore);
  fb.setTextColor(rgb(255, 120, 150)); fb.setCursor(100, 2); fb.print("LIVES "); fb.print(pjLives);
  if (pjOver) banner(pjWon ? "YOU WIN!" : "GAME OVER", "A retry  B menu");
}

// ================= 3D MAZE =================
const char mazeMap[8][9] = {
  "11111111", "10000001", "10111101", "10100001",
  "10101111", "10100001", "10001201", "11111111"
};
float mx, my, mang;
bool mazeOver;

char cellAt(int x, int y) { return (x < 0 || x >= 8 || y < 0 || y >= 8) ? '1' : mazeMap[y][x]; }
bool walkable(float x, float y) { char c = cellAt((int)x, (int)y); return c == '0' || c == '2'; }
void mazeInit() { mx = 1.5f; my = 1.5f; mang = 0; mazeOver = false; }

void mazeUpdate() {
  if (mazeOver) { if (pressed(BTN_A)) mazeInit(); return; }
  if (held(BTN_LEFT))  mang -= 0.06f;
  if (held(BTN_RIGHT)) mang += 0.06f;
  if (held(BTN_A)) {
    float nx = mx + cosf(mang) * 0.07f, ny = my + sinf(mang) * 0.07f;
    if (walkable(nx, my)) mx = nx;
    if (walkable(mx, ny)) my = ny;
  }
  if (cellAt((int)mx, (int)my) == '2') mazeOver = true;
}

void mazeDraw() {
  fb.fillRect(0, 0, 160, 64, rgb(40, 60, 140));
  fb.fillRect(0, 64, 160, 64, rgb(120, 70, 40));
  float dirX = cosf(mang), dirY = sinf(mang);
  float plX = -dirY * 0.66f, plY = dirX * 0.66f;
  for (int col = 0; col < 80; col++) {
    float camX = 2.0f * col / 80.0f - 1.0f;
    float rdx = dirX + plX * camX, rdy = dirY + plY * camX;
    int mapX = (int)mx, mapY = (int)my;
    float ddx = fabsf(rdx) < 1e-6f ? 1e30f : fabsf(1.0f / rdx);
    float ddy = fabsf(rdy) < 1e-6f ? 1e30f : fabsf(1.0f / rdy);
    int stepX = rdx < 0 ? -1 : 1, stepY = rdy < 0 ? -1 : 1;
    float sideX = rdx < 0 ? (mx - mapX) * ddx : (mapX + 1.0f - mx) * ddx;
    float sideY = rdy < 0 ? (my - mapY) * ddy : (mapY + 1.0f - my) * ddy;
    char hit = '1';
    int side = 0;
    for (int i = 0; i < 24; i++) {
      if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
      else               { sideY += ddy; mapY += stepY; side = 1; }
      hit = cellAt(mapX, mapY);
      if (hit != '0') break;
    }
    float dist = side == 0 ? sideX - ddx : sideY - ddy;
    if (dist < 0.1f) dist = 0.1f;
    int lineH = min(128, (int)(110.0f / dist));
    uint16_t color = (hit == '2') ? rgb(80, 255, 120) : (side == 0 ? rgb(230, 90, 60) : rgb(170, 50, 90));
    fb.fillRect(col * 2, 64 - lineH / 2, 2, lineH, color);
  }
  fb.setTextColor(ST77XX_YELLOW); fb.setCursor(2, 2); fb.print("Find the green wall!");
  if (mazeOver) banner("YOU ESCAPED!", "A retry  B menu");
}

// ================= FOOTBALL =================

FBody fbYou, fbCpu, fbBall;
int fbYourScore, fbCpuScore, fbKick;
bool fbOver;
unsigned long fbStart;
const int FX0 = 4, FY0 = 14, FX1 = 156, FY1 = 124, FGH = 30, FGY0 = 54;

void fbPlace() { fbYou = {40, 69, 0, 0}; fbCpu = {120, 69, 0, 0}; fbBall = {80, 69, 0, 0}; fbKick = 0; }
void fbInit() { fbYourScore = fbCpuScore = 0; fbOver = false; fbStart = millis(); fbPlace(); }

// Use primitive parameters here: the sketch preprocessor can generate
// prototypes before the FBody typedef appears later in this file.
void fbClamp(float& x, float& y, float r) {
  x = constrain(x, FX0 + r, FX1 - r);
  y = constrain(y, FY0 + r, FY1 - r);
}

void fbCollide(float px, float py, float pvx, float pvy) {
  float dx = fbBall.x - px, dy = fbBall.y - py, d = sqrtf(dx * dx + dy * dy);
  if (d < 6.0f && d > 0.01f) {
    float nx = dx / d, ny = dy / d;
    fbBall.x = px + nx * 6.0f; fbBall.y = py + ny * 6.0f;
    fbBall.vx = nx * 1.2f + pvx * 0.6f; fbBall.vy = ny * 1.2f + pvy * 0.6f;
  }
}

void fbUpdate() {
  if (fbOver) { if (pressed(BTN_A)) fbInit(); return; }
  if (millis() - fbStart >= 60000) { fbOver = true; return; }

  fbYou.vx = 0;
  if (held(BTN_LEFT))  fbYou.vx = -1.6f;
  if (held(BTN_RIGHT)) fbYou.vx =  1.6f;
  fbYou.x += fbYou.vx;
  fbClamp(fbYou.x, fbYou.y, 4);
  if (pressed(BTN_A)) {
    float dx = fbBall.x - fbYou.x, dy = fbBall.y - fbYou.y, d = sqrtf(dx * dx + dy * dy);
    if (d < 10 && d > 0.01f) { fbBall.vx += dx / d * 3.0f; fbBall.vy += dy / d * 3.0f; }
  }

  float dx = fbBall.x + 8 - fbCpu.x, dy = fbBall.y - fbCpu.y, d = sqrtf(dx * dx + dy * dy);
  if (d > 1.0f) { fbCpu.vx = dx / d * 1.1f; fbCpu.vy = dy / d * 1.1f; }
  else          { fbCpu.vx = fbCpu.vy = 0; }
  fbCpu.x += fbCpu.vx; fbCpu.y += fbCpu.vy;
  fbClamp(fbCpu.x, fbCpu.y, 4);

  float bx = fbBall.x - fbCpu.x, by = fbBall.y - fbCpu.y;
  if (sqrtf(bx * bx + by * by) < 7 && fbKick == 0) { fbBall.vx -= 2.0f; fbKick = 10; }
  if (fbKick > 0) fbKick--;

  fbBall.x += fbBall.vx; fbBall.y += fbBall.vy;
  fbBall.vx *= 0.97f; fbBall.vy *= 0.97f;
  fbCollide(fbYou.x, fbYou.y, fbYou.vx, fbYou.vy);
  fbCollide(fbCpu.x, fbCpu.y, fbCpu.vx, fbCpu.vy);

  bool inGoal = fbBall.y > FGY0 && fbBall.y < FGY0 + FGH;
  if (!inGoal) {
    if (fbBall.x < FX0 + 2) { fbBall.x = FX0 + 2; fbBall.vx = -fbBall.vx * 0.7f; }
    if (fbBall.x > FX1 - 2) { fbBall.x = FX1 - 2; fbBall.vx = -fbBall.vx * 0.7f; }
  }
  if (fbBall.y < FY0 + 2) { fbBall.y = FY0 + 2; fbBall.vy = -fbBall.vy * 0.7f; }
  if (fbBall.y > FY1 - 2) { fbBall.y = FY1 - 2; fbBall.vy = -fbBall.vy * 0.7f; }

  if (fbBall.x > FX1 - 1 && inGoal) { fbYourScore++; fbPlace(); }
  else if (fbBall.x < FX0 + 1 && inGoal) { fbCpuScore++; fbPlace(); }
}

void fbDraw() {
  fb.fillScreen(rgb(30, 150, 60));
  for (int x = 0; x < 160; x += 16) fb.fillRect(x, FY0, 8, FY1 - FY0, rgb(35, 160, 70));
  fb.drawRect(FX0, FY0, FX1 - FX0, FY1 - FY0, ST77XX_WHITE);
  fb.drawFastVLine(80, FY0, FY1 - FY0, ST77XX_WHITE);
  fb.fillRect(FX0 - 3, FGY0, 3, FGH, ST77XX_WHITE);
  fb.fillRect(FX1, FGY0, 3, FGH, ST77XX_WHITE);
  fb.fillCircle((int)fbYou.x, (int)fbYou.y, 4, rgb(60, 140, 255));
  fb.fillCircle((int)fbCpu.x, (int)fbCpu.y, 4, rgb(255, 70, 70));
  fb.fillCircle((int)fbBall.x, (int)fbBall.y, 2, ST77XX_WHITE);
  fb.fillRect(0, 0, 160, 12, rgb(10, 10, 40));
  fb.setTextColor(ST77XX_YELLOW); fb.setCursor(4, 2);
  fb.print("YOU "); fb.print(fbYourScore); fb.print(" - "); fb.print(fbCpuScore);
  unsigned long el = millis() - fbStart;
  fb.setTextColor(ST77XX_CYAN);
  fb.setCursor(132, 2); fb.print(el >= 60000 ? 0 : (long)((60000 - el) / 1000)); fb.print("s");
  if (fbOver) banner(fbYourScore > fbCpuScore ? "YOU WIN!" : fbYourScore < fbCpuScore ? "CPU WINS" : "DRAW", "A retry  B menu");
}

// ================= 3D RUNNER =================
const float RUN_HY = 40;
struct Ob { int lane; float d; bool on; };
Ob obs[6];
int runLane;
float runH, runVY, runSpeed, runDist;
bool runOver;

void runInit() {
  for (auto& o : obs) o.on = false;
  runLane = 0; runH = 0; runVY = 0;
  runSpeed = 0.012f; runDist = 0; runOver = false;
}

void runUpdate() {
  if (runOver) { if (pressed(BTN_A)) runInit(); return; }
  if (pressed(BTN_LEFT)  && runLane > -1) runLane--;
  if (pressed(BTN_RIGHT) && runLane <  1) runLane++;
  if (pressed(BTN_A) && runH <= 0) runVY = 3.2f;
  runH += runVY;
  runVY -= 0.25f;
  if (runH < 0) { runH = 0; runVY = 0; }
  if (random(100) < 4) {
    for (auto& o : obs) if (!o.on) { o.on = true; o.lane = random(-1, 2); o.d = 0; break; }
  }
  runSpeed = min(0.03f, runSpeed + 0.00002f);
  runDist += runSpeed * 10;
  for (auto& o : obs) {
    if (!o.on) continue;
    o.d += runSpeed;
    if (o.d > 1.05f) o.on = false;
    else if (o.d > 0.88f && o.d < 1.0f && o.lane == runLane && runH < 8) runOver = true;
  }
}

void runDraw() {
  for (int y = 0; y < (int)RUN_HY; y++) fb.drawFastHLine(0, y, 160, rgb(60 + y * 2, 120 + y * 2, 230));
  for (int y = (int)RUN_HY; y < 128; y++) {
    float d = (y - RUN_HY) / (127.0f - RUN_HY);
    float hw = 4 + d * 56;
    float z = 1.0f / (d + 0.08f);
    int band = ((int)(z * 3 + runDist * 40)) & 1;
    fb.drawFastHLine(0, y, 160, band ? rgb(40, 150, 70) : rgb(30, 125, 55));
    fb.drawFastHLine(80 - (int)hw, y, (int)(2 * hw), band ? rgb(90, 90, 110) : rgb(75, 75, 95));
  }
  for (auto& o : obs) {
    if (!o.on) continue;
    int y = (int)(RUN_HY + o.d * (127 - RUN_HY));
    float hw = 4 + o.d * 56;
    int x = 80 + (int)(o.lane * hw * 0.55f);
    int s = 2 + (int)(o.d * 16);
    fb.fillRect(x - s / 2, y - s, s, s, rgb(255, 80, 80));
  }
  float hw = 4 + 0.95f * 56;
  int px = 80 + (int)(runLane * hw * 0.55f);
  int py = 118 - (int)runH;
  fb.fillRoundRect(px - 5, py - 14, 10, 14, 3, rgb(80, 230, 255));
  fb.fillRect(px - 3, py - 10, 2, 2, ST77XX_BLACK);
  fb.fillRect(px + 1, py - 10, 2, 2, ST77XX_BLACK);
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(2, 2); fb.print("DIST "); fb.print((int)runDist);
  if (runOver) banner("CRASH!", "A retry  B menu");
}

// ================= SNAKE =================
const int SN_W = 16, SN_H = 14, SN_CELL = 8;
int snX[64], snY[64], snLen, snDir;
int snFoodX, snFoodY, snScore, snHigh = 0, snTick;
bool snOver;

void snPlaceFood() {
  for (int tries = 0; tries < 100; tries++) {
    snFoodX = random(SN_W); snFoodY = random(SN_H);
    bool ok = true;
    for (int i = 0; i < snLen; i++) if (snX[i] == snFoodX && snY[i] == snFoodY) ok = false;
    if (ok) return;
  }
}

void snInit() {
  snLen = 3; snDir = 0; snScore = 0; snTick = 0; snOver = false;
  for (int i = 0; i < snLen; i++) { snX[i] = 6 - i; snY[i] = 7; }
  snPlaceFood();
}

void snUpdate() {
  if (snOver) { if (pressed(BTN_A)) snInit(); return; }
  if (pressed(BTN_LEFT))  snDir = (snDir + 3) % 4;
  if (pressed(BTN_RIGHT)) snDir = (snDir + 1) % 4;
  if (++snTick < 5) return;
  snTick = 0;
  static const int DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};
  int nx = snX[0] + DX[snDir], ny = snY[0] + DY[snDir];
  if (nx < 0 || nx >= SN_W || ny < 0 || ny >= SN_H) snOver = true;
  for (int i = 0; i < snLen && !snOver; i++) if (snX[i] == nx && snY[i] == ny) snOver = true;
  if (snOver) {
    if (snScore > snHigh) snHigh = snScore;
    return;
  }
  bool eat = (nx == snFoodX && ny == snFoodY);
  if (eat && snLen < 63) { snLen++; snScore++; }
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = nx; snY[0] = ny;
  if (eat) snPlaceFood();
}

void snDraw() {
  themeBackground();
  int ox = 0, oy = 12;
  fb.fillRect(ox, oy, SN_W * SN_CELL, SN_H * SN_CELL, rgb(10, 20, 30));
  fb.fillRoundRect(ox + snFoodX * SN_CELL + 1, oy + snFoodY * SN_CELL + 1, 6, 6, 3, rgb(255, 90, 110));
  for (int i = snLen - 1; i >= 0; i--) {
    uint16_t c = (i == 0) ? T().accent : rgb(80, 220, 150);
    fb.fillRoundRect(ox + snX[i] * SN_CELL + 1, oy + snY[i] * SN_CELL + 1, 6, 6, 2, c);
  }
  fb.fillRect(0, 0, 160, 12, rgb(10, 5, 25));
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(3, 2);
  fb.print("SCORE "); fb.print(snScore);
  fb.setCursor(90, 2); fb.print("BEST "); fb.print(snHigh);
  if (snOver) banner("GAME OVER", "A retry  B menu");
}

// ================= DICE =================
int diceA = 1, diceB = 1, diceMode = 1, diceShake = 0;
bool diceAnim = false;

void diceRoll() { diceAnim = true; diceShake = 10; }

void diceUpdate() {
  if (pressed(BTN_RIGHT)) diceMode = 2;
  if (pressed(BTN_LEFT))  diceMode = 1;
  if (pressed(BTN_A)) diceRoll();
  if (diceAnim) {
    diceA = random(1, 7); diceB = random(1, 7);
    if (--diceShake <= 0) {
      diceAnim = false;
      diceA = random(1, 7); diceB = random(1, 7);
    }
  }
}

void drawPip(int cx, int cy, int v, uint16_t c) {
  int ofs = 5;
  struct P { int x, y; };
  static const P pips[7][7] = {
    {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}},
    {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}},
    {{-1,-1},{1,1},{0,0},{0,0},{0,0},{0,0},{0,0}},
    {{-1,-1},{0,0},{1,1},{0,0},{0,0},{0,0},{0,0}},
    {{-1,-1},{1,-1},{-1,1},{1,1},{0,0},{0,0},{0,0}},
    {{-1,-1},{1,-1},{0,0},{-1,1},{1,1},{0,0},{0,0}},
    {{-1,-1},{1,-1},{-1,0},{1,0},{-1,1},{1,1},{0,0}}
  };
  for (int i = 0; i < v; i++) {
    int px = cx + pips[v][i].x * ofs, py = cy + pips[v][i].y * ofs;
    fb.fillCircle(px, py, 2, c);
  }
}

void diceDraw() {
  themeBackground();
  fb.setTextColor(T().text); fb.setCursor(40, 6); fb.print("DICE");
  int count = diceMode;
  for (int d = 0; d < count; d++) {
    int cx = (count == 1) ? 80 : (d == 0 ? 58 : 102);
    int sh = diceAnim ? (int)random(-2, 3) : 0;
    panel(cx - 20 + sh, 30, 40, 40, ST77XX_WHITE);
    drawPip(cx + sh, 50, d == 0 ? diceA : diceB, rgb(40, 20, 60));
  }
  fb.setTextColor(T().text);
  fb.setCursor(8, 90); fb.print("L 1 die   R 2 dice");
  fb.setCursor(8, 104); fb.print("A roll           B menu");
}

// ================= CALCULATOR =================
const char CALC_KEYS[] = "0123456789+-*/=<>";
const int CALC_N = sizeof(CALC_KEYS) - 1;
int calcIdx = 0;
String calcExpr, calcOut;

String calcEval(const String& e) {
  int op = -1;
  for (int i = 1; i < (int)e.length(); i++) {
    char c = e[i];
    if (c == '+' || c == '-' || c == '*' || c == '/') { op = i; break; }
  }
  if (op < 0) return e;
  double a = e.substring(0, op).toDouble();
  double b = e.substring(op + 1).toDouble();
  char o = e[op];
  if (o == '+') return String(a + b, 4);
  if (o == '-') return String(a - b, 4);
  if (o == '*') return String(a * b, 4);
  if (b == 0) return "div by 0";
  return String(a / b, 4);
}

void calcUpdate() {
  if (pressed(BTN_LEFT))  calcIdx = (calcIdx + CALC_N - 1) % CALC_N;
  if (pressed(BTN_RIGHT)) calcIdx = (calcIdx + 1) % CALC_N;
  if (pressed(BTN_A)) {
    char c = CALC_KEYS[calcIdx];
    if (c == '<') { if (calcExpr.length()) calcExpr.remove(calcExpr.length() - 1); }
    else if (c == '=') calcOut = calcEval(calcExpr);
    else if (c == '>') { calcExpr = ""; calcOut = ""; }
    else if (calcExpr.length() < 18) calcExpr += c;
  }
}

void calcDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("CALCULATOR");
  panel(6, 18, 148, 30, rgb(20, 10, 40));
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(10, 22); fb.print(calcExpr); fb.print("_");
  fb.setTextColor(T().accent); fb.setCursor(10, 36);
  fb.print(calcOut.length() ? calcOut.substring(0, 20) : String(""));
  for (int k = -4; k <= 4; k++) {
    int i = (calcIdx + k + CALC_N * 2) % CALC_N;
    int x = 80 + k * 15 - 7;
    bool cur = (k == 0);
    fb.fillRoundRect(x, 64, 14, 18, 3, cur ? T().accent : rgb(60, 40, 90));
    fb.setTextColor(cur ? rgb(20, 10, 30) : ST77XX_WHITE);
    fb.setCursor(x + 4, 69); fb.print(CALC_KEYS[i]);
  }
  fb.setTextColor(T().text); fb.setCursor(4, 90); fb.print("= calc   > clear   < del");
  fb.setCursor(4, 110); fb.print("L/R pick  A press  B menu");
}

// ================= SD: FILE LISTS =================
const int MAX_FILES = 20;
String fileNames[MAX_FILES];
bool fileHasCover[MAX_FILES];
int fileCount = 0, fileSel = 0;

const int MAX_PHOTOS = 20;
String photoNames[MAX_PHOTOS];
int photoCount = 0, photoIdx = 0;

void scanStories() {
  fileCount = 0;
  if (!sdReady) return;
  useSdBus();
  File dir = SD.open("/stories");
  if (dir && dir.isDirectory()) {
    File f;
    while (fileCount < MAX_FILES && (f = dir.openNextFile())) {
      String name = f.name();
      bool isDir = f.isDirectory();
      f.close();
      if (!isDir && endsWithCI(name, ".txt")) {
        fileNames[fileCount] = name;
        String base = name.substring(0, name.length() - 4);
        fileHasCover[fileCount] = SD.exists("/stories/" + base + ".bmp");
        fileCount++;
      }
    }
  }
  dir.close();
  useDisplayBus();
}

void scanPhotos() {
  photoCount = 0;
  if (!sdReady) return;
  useSdBus();
  File dir = SD.open("/photos");
  if (dir && dir.isDirectory()) {
    File f;
    while (photoCount < MAX_PHOTOS && (f = dir.openNextFile())) {
      String name = f.name();
      bool isDir = f.isDirectory();
      f.close();
      if (!isDir && endsWithCI(name, ".bmp")) photoNames[photoCount++] = name;
    }
  }
  dir.close();
  useDisplayBus();
}

// ================= SD: READER =================
const int TV_CHARS = 26, TV_LINES = 9;
const int MAX_PAGES = 2000;
uint32_t pageStart[MAX_PAGES];
int pageCount = 0, tvPage = 0, cachedPage = -1, openIdx = 0;
String cachedText, openName, coverPath;
bool hasCover = false, showingCover = false;
File readFile;

const int MAX_BM = 20;
String bmName[MAX_BM];
int bmPage[MAX_BM];
int bmCount = 0;
unsigned long bmBannerUntil = 0;
String recentName = "";

int bmFind(const String& n) {
  for (int i = 0; i < bmCount; i++) if (bmName[i] == n) return i;
  return -1;
}

void bmWrite() {
  if (!sdReady) return;
  useSdBus();
  SD.remove("/bookmarks.txt");
  if (bmCount > 0) {
    File f = SD.open("/bookmarks.txt", FILE_WRITE);
    if (f) {
      for (int i = 0; i < bmCount; i++) { f.print(bmName[i]); f.print('|'); f.println(bmPage[i]); }
      f.close();
    }
  }
  useDisplayBus();
}

void bmLoad() {
  bmCount = 0;
  if (!sdReady) return;
  useSdBus();
  File f = SD.open("/bookmarks.txt");
  if (f) {
    while (f.available() && bmCount < MAX_BM) {
      String line = f.readStringUntil('\n');
      line.trim();
      int p = line.lastIndexOf('|');
      if (p > 0) {
        bmName[bmCount] = line.substring(0, p);
        bmPage[bmCount] = line.substring(p + 1).toInt();
        bmCount++;
      }
    }
    f.close();
  }
  useDisplayBus();
}

void bmSaveCurrent() {
  int i = bmFind(openName);
  if (i < 0) {
    if (bmCount >= MAX_BM) return;
    i = bmCount++;
    bmName[i] = openName;
  }
  bmPage[i] = tvPage;
  bmWrite();
  bmBannerUntil = millis() + 1500;
}

uint32_t readPageAt(uint32_t start, String& out) {
  out = "";
  readFile.seek(start);
  int lines = 0, col = 0;
  while (readFile.available() && lines < TV_LINES) {
    char c = readFile.read();
    if (c == '\r') continue;
    if (c == '\n' || col >= TV_CHARS) {
      lines++; col = 0;
      if (c == '\n') { out += '\n'; continue; }
      out += '\n';
    }
    out += c; col++;
  }
  return readFile.position();
}

void buildPageIndex() {
  pageCount = 0;
  uint32_t pos = 0, size = readFile.size();
  String scratch;
  while (pos < size && pageCount < MAX_PAGES) {
    pageStart[pageCount++] = pos;
    pos = readPageAt(pos, scratch);
    if (pos <= pageStart[pageCount - 1]) break;
  }
}

uint16_t accentFor(int i) {
  switch (i % 4) {
    case 0: return rgb(255, 120, 80);
    case 1: return rgb(80, 200, 255);
    case 2: return rgb(150, 255, 120);
    default: return rgb(230, 130, 255);
  }
}

bool openBook(int idx) {
  useSdBus();
  if (readFile) readFile.close();
  openIdx = idx;
  openName = fileNames[idx];
  readFile = SD.open("/stories/" + openName);
  bool ok = (bool)readFile;
  if (ok) buildPageIndex();
  useDisplayBus();
  tvPage = 0; cachedPage = -1; showingCover = false;
  int b = bmFind(openName);
  if (ok && b >= 0 && bmPage[b] < pageCount) tvPage = bmPage[b];
  hasCover = fileHasCover[idx];
  if (hasCover) {
    String base = openName.substring(0, openName.length() - 4);
    coverPath = "/stories/" + base + ".bmp";
  }
  if (ok) recentName = openName;
  return ok;
}

void closeBook() {
  useSdBus();
  if (readFile) readFile.close();
  useDisplayBus();
  showingCover = false;
}

void ensureCachedText() {
  if (cachedPage != tvPage && pageCount > 0) {
    useSdBus();
    readPageAt(pageStart[tvPage], cachedText);
    useDisplayBus();
    cachedPage = tvPage;
  }
}

void renderTextPage() {
  ensureCachedText();
  uint16_t accent = accentFor(openIdx);
  fb.fillScreen(rgb(18, 8, 34));
  fb.fillRect(0, 0, 160, 11, accent);
  fb.setTextColor(ST77XX_BLACK); fb.setCursor(3, 2);
  fb.print(openName.substring(0, 24));
  fb.setTextColor(rgb(255, 240, 200));
  int y = 15, start = 0, len = cachedText.length();
  for (int i = 0; i <= len; i++) {
    if (i == len || cachedText[i] == '\n') {
      fb.setCursor(4, y);
      fb.print(cachedText.substring(start, i));
      y += 11;
      start = i + 1;
    }
  }
  fb.fillRect(0, 116, 160, 12, accent);
  fb.setTextColor(ST77XX_BLACK); fb.setCursor(3, 118);
  fb.print(tvPage + 1); fb.print("/"); fb.print(pageCount);
  fb.print(hasCover ? "  L/R  A:pic  B" : "  L/R  hold B:mark");
}

void toggleCover() {
  if (!hasCover) return;
  if (!ensureImageBuffers()) return;
  if (!showingCover) {
    renderTextPage();
    memcpy(imgCur, fb.getBuffer(), FRAME_BYTES);
    if (!loadBmp(coverPath, imgNext)) return;
    slideBuffers(imgCur, imgNext, 1);
    memcpy(imgCur, imgNext, FRAME_BYTES);
    showingCover = true;
  } else {
    renderTextPage();
    memcpy(imgNext, fb.getBuffer(), FRAME_BYTES);
    slideBuffers(imgCur, imgNext, -1);
    memcpy(imgCur, imgNext, FRAME_BYTES);
    showingCover = false;
  }
}

void readerUpdate() {
  if (pressed(BTN_A) && hasCover) { toggleCover(); return; }
  if (showingCover) return;
  if (pressed(BTN_RIGHT) && tvPage + 1 < pageCount) tvPage++;
  if (pressed(BTN_LEFT)  && tvPage > 0) tvPage--;
}

void readerDraw() {
  if (showingCover) {
    memcpy(fb.getBuffer(), imgCur, FRAME_BYTES);
    fb.fillRect(0, 116, 160, 12, rgb(10, 10, 30));
    fb.setTextColor(ST77XX_YELLOW); fb.setCursor(3, 118);
    fb.print("A: text   B: list");
    return;
  }
  renderTextPage();
  if (millis() < bmBannerUntil) {
    fb.fillRoundRect(30, 56, 100, 16, 4, rgb(255, 200, 80));
    fb.setTextColor(ST77XX_BLACK); fb.setCursor(40, 61);
    fb.print("BOOKMARK SAVED");
  }
}

void fileListUpdate() {
  int n = max(1, fileCount);
  if (pressed(BTN_LEFT))  fileSel = (fileSel + n - 1) % n;
  if (pressed(BTN_RIGHT)) fileSel = (fileSel + 1) % n;
  if (pressed(BTN_A) && fileCount > 0) {
    if (openBook(fileSel)) scene = READER;
  }
}

void fileListDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("LIBRARY");
  if (!sdReady) {
    fb.setTextColor(T().text); fb.setCursor(4, 30); fb.print("SD card not found");
  } else if (fileCount == 0) {
    fb.setTextColor(T().text); fb.setCursor(4, 30); fb.print("No .txt in /stories");
  } else {
    int firstShown = max(0, min(fileSel - 3, fileCount - 6));
    for (int i = 0; i < 6 && firstShown + i < fileCount; i++) {
      int idx = firstShown + i, y = 18 + i * 15;
      bool sel = (idx == fileSel);
      panel(4, y, 152, 13, sel ? accentFor(idx) : rgb(60, 30, 70));
      fb.setTextColor(sel ? rgb(20, 10, 30) : ST77XX_WHITE);
      fb.setCursor(8, y + 3); fb.print(fileNames[idx].substring(0, 18));
      if (fileNames[idx] == recentName) {
        fb.setTextColor(sel ? rgb(20, 10, 30) : rgb(120, 255, 180));
        fb.setCursor(120, y + 3); fb.print("NEW");
      }
      if (fileHasCover[idx]) {
        fb.setTextColor(sel ? rgb(20, 10, 30) : T().accent);
        fb.setCursor(136, y + 3); fb.print("*");
      }
      if (bmFind(fileNames[idx]) >= 0) {
        fb.setTextColor(sel ? rgb(20, 10, 30) : rgb(255, 180, 90));
        fb.setCursor(146, y + 3); fb.print("@");
      }
    }
  }
  fb.setTextColor(T().text); fb.setCursor(4, 116); fb.print("L/R move  A open  B menu");
}

// ================= PHOTOS =================
String photoPath(int i) { return "/photos/" + photoNames[i]; }
bool photoMemError = false;

void photosInit() {
  scanPhotos();
  photoIdx = 0;
  photoMemError = !ensureImageBuffers();
  if (photoMemError) return;
  if (photoCount > 0) loadBmp(photoPath(0), imgCur);
  else memset(imgCur, 0, FRAME_BYTES);
}

void photosUpdate() {
  if (photoMemError || !imgCur || !imgNext || photoCount < 2) return;
  int dir = 0;
  if (pressed(BTN_RIGHT)) dir = 1;
  if (pressed(BTN_LEFT))  dir = -1;
  if (dir == 0) return;
  int ni = (photoIdx + dir + photoCount) % photoCount;
  if (!loadBmp(photoPath(ni), imgNext)) return;
  slideBuffers(imgCur, imgNext, dir);
  memcpy(imgCur, imgNext, FRAME_BYTES);
  photoIdx = ni;
}

void photosDraw() {
  if (photoMemError || !imgCur) {
    themeBackground();
    fb.setTextColor(T().text); fb.setCursor(4, 40); fb.print("Not enough RAM for photos");
    fb.setCursor(4, 116); fb.print("B menu");
    return;
  }
  if (photoCount == 0) {
    themeBackground();
    fb.setTextColor(T().text); fb.setCursor(4, 40); fb.print("No .bmp in /photos");
    fb.setCursor(4, 116); fb.print("B menu");
    return;
  }
  memcpy(fb.getBuffer(), imgCur, FRAME_BYTES);
  fb.fillRect(0, 116, 160, 12, rgb(10, 10, 30));
  fb.setTextColor(T().accent); fb.setCursor(3, 118);
  fb.print(photoIdx + 1); fb.print("/"); fb.print(photoCount); fb.print("  ");
  fb.print(photoNames[photoIdx].substring(0, 16));
}

// ================= NOTES =================
String noteMsg;

void appendNote(const String& text) {
  if (!sdReady) { noteMsg = "No SD card"; return; }
  useSdBus();
  File f = SD.open("/notes.txt", FILE_APPEND);
  if (f) { f.println(text); f.close(); noteMsg = "Saved to notes"; }
  else noteMsg = "Save failed";
  useDisplayBus();
}

void notesUpdate() {
  if (!kbUpdate()) return;
  String t = kbText;
  t.trim();
  if (t.length()) appendNote(t);
  else noteMsg = "Type something";
  kbText = "";
}

void notesDraw() {
  kbDraw("NOTES  (> saves)");
  fb.setTextColor(T().accent); fb.setCursor(4, 100);
  fb.print(noteMsg.substring(0, 24));
}

// ================= STOPWATCH =================
bool swRun = false;
unsigned long swStart = 0, swAccum = 0;

unsigned long swElapsed() { return swAccum + (swRun ? millis() - swStart : 0); }

void swUpdate() {
  if (pressed(BTN_A)) {
    if (swRun) { swAccum += millis() - swStart; swRun = false; }
    else { swStart = millis(); swRun = true; }
  }
  if (pressed(BTN_LEFT)) { swAccum = 0; swRun = false; }
}

void swDraw() {
  themeBackground();
  unsigned long e = swElapsed();
  unsigned long m = e / 60000, s = (e / 1000) % 60, cs = (e % 1000) / 10;
  panel(10, 30, 140, 50, rgb(20, 10, 40));
  fb.setTextSize(2); fb.setTextColor(T().accent);
  fb.setCursor(18, 44);
  if (m < 10) fb.print('0');
  fb.print(m); fb.print(':');
  if (s < 10) fb.print('0');
  fb.print(s);
  fb.setTextSize(1); fb.setTextColor(ST77XX_YELLOW);
  fb.setCursor(118, 62);
  fb.print('.'); if (cs < 10) fb.print('0'); fb.print(cs);
  fb.setTextColor(T().text); fb.setCursor(4, 100);
  fb.print(swRun ? "A pause" : "A start");
  fb.setCursor(4, 112); fb.print("L reset   B menu");
}

// ================= SETTINGS =================
void loadSettings() {
  if (!sdReady) return;
  useSdBus();
  File f = SD.open("/settings.txt");
  if (f) {
    tzIdx = f.parseInt(); idleIdx = f.parseInt(); themeIdx = f.parseInt();
    wifiEnabled = f.parseInt() != 0;
    f.close();
  }
  useDisplayBus();
  if (tzIdx < 0 || tzIdx >= TZ_N) tzIdx = 0;
  if (idleIdx < 0 || idleIdx >= IDLE_N) idleIdx = 1;
  if (themeIdx < 0 || themeIdx >= 3) themeIdx = 0;
}

void saveSettings() {
  if (!sdReady) return;
  useSdBus();
  SD.remove("/settings.txt");
  File f = SD.open("/settings.txt", FILE_WRITE);
  if (f) {
    f.print(tzIdx); f.print(' '); f.print(idleIdx); f.print(' ');
    f.print(themeIdx); f.print(' '); f.println(wifiEnabled ? 1 : 0);
    f.close();
  }
  useDisplayBus();
}

void settingsUpdate() {
  if (ntfyEditMode) {
    if (kbUpdate()) {
      String candidate = kbText; candidate.trim();
      if (validNtfyTopic(candidate)) {
        if (saveNtfyTopic(candidate)) settingsMsg = "ntfy topic saved";
        else settingsMsg = "SD save failed";
        chatLastId = ""; msgNextPoll = 0;
      } else settingsMsg = "Use 3-48 letters/numbers";
      ntfyEditMode = false;
    }
    return;
  }
  if (pressed(BTN_A)) {
    if (setRow == 6) {
      ntfyEditMode = true; kbText = ntfyTopic; kbIdx = 0;
      return;
    }
    setRow = (setRow + 1) % 7;
  }
  bool L = pressed(BTN_LEFT), R = pressed(BTN_RIGHT);
  if (setRow == 0 && (L || R)) {
    tzIdx = (tzIdx + (R ? 1 : TZ_N - 1)) % TZ_N;
    saveSettings(); applyTimeZone(); settingsMsg = "Time zone set";
  }
  if (setRow == 1 && (L || R)) {
    idleIdx = (idleIdx + (R ? 1 : IDLE_N - 1)) % IDLE_N;
    saveSettings(); settingsMsg = "Sleep timer set";
  }
  if (setRow == 2 && (L || R)) {
    themeIdx = (themeIdx + (R ? 1 : 2)) % 3;
    saveSettings(); settingsMsg = "Theme set";
  }
  if (setRow == 3 && (L || R)) {
    wifiEnabled = !wifiEnabled; saveSettings(); wifiApplyEnabled();
    settingsMsg = wifiEnabled ? "Wi-Fi on" : "Wi-Fi off";
  }
  if (setRow == 4 && R) { bmCount = 0; bmWrite(); settingsMsg = "Bookmarks cleared"; }
  if (setRow == 5 && R) forgetWifi();
}

void settingsDraw() {
  if (ntfyEditMode) { kbDraw("NTFY TOPIC (> save)"); return; }
  themeBackground();
  fb.fillRect(0, 0, 160, 12, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 2); fb.print("SETTINGS");
  const char* labels[7] = {"Time zone", "Sleep after", "Theme", "Wi-Fi", "Clear marks", "Forget Wi-Fi", "ntfy topic"};
  const char* themeNames[3] = {"Night", "Candy", "Mint"};
  for (int i = 0; i < 7; i++) {
    int y = 13 + i * 12;
    bool sel = (i == setRow);
    panel(4, y, 152, 11, sel ? T().accent : rgb(60, 30, 70));
    fb.setTextColor(sel ? rgb(20, 10, 30) : ST77XX_WHITE);
    fb.setCursor(8, y + 2); fb.print(labels[i]);
    fb.setCursor(88, y + 2);
    if (i == 0) fb.print(TZ_NAME[tzIdx]);
    if (i == 1) fb.print(IDLE_NAME[idleIdx]);
    if (i == 2) fb.print(themeNames[themeIdx]);
    if (i == 3) fb.print(wifiEnabled ? "ON" : "OFF");
    if (i == 4) fb.print("press R");
    if (i == 5) fb.print(wifiSsid.length() ? "saved" : "none");
    if (i == 6) fb.print(ntfyTopic.substring(0, 10));
  }
  fb.setTextColor(T().accent); fb.setCursor(4, 111); fb.print(settingsMsg.substring(0, 24));
  fb.setTextColor(T().text); fb.setCursor(4, 120); fb.print("A next/edit  L/R change");
}

// ================= HOME DASHBOARD =================
const char* DAYS[7] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
const char* MONTHS[12] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
                          "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};

int daysInMonth(int m, int y) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return d[m];
}

bool timeReadyCheck(struct tm& tmv) { return timeReady && getLocalTime(&tmv, 5); }

void homeDraw() {
  themeBackground();
  statusBar(true);
  struct tm tmv;
  bool ok = timeReadyCheck(tmv);
  panel(8, 16, 144, 50, rgb(20, 10, 40));
  fb.setTextColor(T().accent); fb.setCursor(14, 22);
  fb.print(ok ? DAYS[tmv.tm_wday] : "CONNECT WI-FI");
  fb.setTextSize(2); fb.setTextColor(T().text); fb.setCursor(14, 36);
  if (ok) {
    int h = tmv.tm_hour % 12; if (h == 0) h = 12;
    if (h < 10) fb.print('0');
    fb.print(h); fb.print(':');
    if (tmv.tm_min < 10) fb.print('0');
    fb.print(tmv.tm_min);
  } else fb.print("--:--");
  fb.setTextSize(1);
  if (ok) {
    fb.setCursor(14, 56);
    fb.print(tmv.tm_mday); fb.print(' '); fb.print(MONTHS[tmv.tm_mon]); fb.print(' ');
    fb.print(tmv.tm_year + 1900);
  }
  panel(8, 72, 144, 40, rgb(20, 10, 40));
  if (ok) {
    int first = ((tmv.tm_wday - (tmv.tm_mday - 1)) % 7 + 7) % 7;
    int dim = daysInMonth(tmv.tm_mon, tmv.tm_year + 1900);
    int col = first, row = 0;
    for (int d = 1; d <= dim; d++) {
      int x = 14 + col * 19, y = 76 + row * 8;
      if (d == tmv.tm_mday) fb.fillRoundRect(x - 2, y - 1, 16, 8, 2, T().accent);
      fb.setTextColor(d == tmv.tm_mday ? rgb(20, 10, 30) : T().text);
      fb.setCursor(x, y); fb.print(d);
      if (++col == 7) { col = 0; row++; }
    }
  }
  fb.setTextColor(T().text); fb.setCursor(8, 116);
  fb.print("A to open menu   B clock");
}

// ================= SLEEP SCREEN =================
void magicSky() {
  for (int y = 0; y < 128; y++) {
    uint8_t r = 18 + y / 10, g = 8 + y / 16, b = 50 + y / 2;
    fb.drawFastHLine(0, y, 160, rgb(r, g, b));
  }
  for (int i = 0; i < 40; i++) {
    int x = (i * 67 + 13) % 160, y = (i * 37 + 7) % 90;
    bool bright = ((frameNo / 6 + i) % 5) < 2;
    fb.drawPixel(x, y, bright ? rgb(255, 250, 220) : rgb(120, 110, 180));
  }
  for (int x = 0; x < 160; x += 2) {
    float w = sinf(x * 0.05f + frameNo * 0.04f) * 7 + sinf(x * 0.13f - frameNo * 0.06f) * 3;
    int y = 22 + (int)w;
    uint16_t c = ((x / 20) % 2) ? rgb(80, 255, 180) : rgb(200, 90, 255);
    fb.drawFastVLine(x, y, 12, c);
  }
  fb.fillCircle(132, 30, 20, rgb(50, 40, 110));
  fb.fillCircle(132, 30, 14, rgb(255, 240, 200));
  fb.fillCircle(138, 26, 12, rgb(24, 12, 70));
  for (int x = 0; x < 160; x++) {
    int h = 106 + (int)(5 * sinf(x * 0.07f)) + (int)(3 * sinf(x * 0.19f + 1));
    fb.drawFastVLine(x, h, 128 - h, rgb(20, 10, 45));
  }
  for (int i = 0; i < 12; i++) {
    int x = (int)((i * 41 + frameNo * (1 + i % 3) / 2) % 160);
    int y = 127 - (int)((frameNo * (1 + i % 2) + i * 17) % 120);
    uint16_t c = (i % 2) ? rgb(255, 200, 255) : rgb(255, 230, 120);
    fb.drawPixel(x, y, c);
    fb.drawPixel(x - 1, y, c); fb.drawPixel(x + 1, y, c);
    fb.drawPixel(x, y - 1, c); fb.drawPixel(x, y + 1, c);
  }
}

void clockDraw() {
  magicSky();
  struct tm tmv;
  bool ok = timeReadyCheck(tmv);
  float pulse = (sinf(frameNo * 0.08f) + 1) * 0.5f;
  uint8_t gl = 150 + (uint8_t)(90 * pulse);
  fb.fillRoundRect(8, 6, 144, 46, 8, rgb(250, 235, 255));
  fb.drawRoundRect(8, 6, 144, 46, 8, rgb(gl, 120, 255));
  fb.setTextColor(rgb(60, 40, 90)); fb.setCursor(14, 9);
  fb.print(ok ? DAYS[tmv.tm_wday] : "SET WI-FI");
  fb.setTextSize(3); fb.setCursor(14, 19);
  if (ok) {
    int h = tmv.tm_hour % 12; if (h == 0) h = 12;
    if (h < 10) fb.print('0');
    fb.print(h); fb.print(':');
    if (tmv.tm_min < 10) fb.print('0');
    fb.print(tmv.tm_min);
  } else fb.print("--:--");
  fb.setTextSize(1);
  if (ok) { fb.setCursor(124, 26); fb.print(tmv.tm_hour < 12 ? "AM" : "PM"); }
  fb.setCursor(14, 44);
  if (ok) {
    if (tmv.tm_mday < 10) fb.print('0');
    fb.print(tmv.tm_mday); fb.print(' '); fb.print(MONTHS[tmv.tm_mon]); fb.print(' ');
    fb.print(tmv.tm_year + 1900);
  }
  fb.fillRoundRect(8, 56, 112, 70, 8, rgb(250, 235, 255));
  fb.drawRoundRect(8, 56, 112, 70, 8, rgb(255, 120, 180));
  fb.fillRect(12, 60, 104, 10, rgb(255, 150, 200));
  fb.setTextColor(rgb(60, 40, 90)); fb.setCursor(16, 61);
  if (ok) { fb.print(MONTHS[tmv.tm_mon]); fb.print(' '); fb.print(tmv.tm_year + 1900); }
  const char* wd = "SMTWTFS";
  for (int c = 0; c < 7; c++) { fb.setCursor(14 + c * 15, 73); fb.print(wd[c]); }
  if (ok) {
    int first = ((tmv.tm_wday - (tmv.tm_mday - 1)) % 7 + 7) % 7;
    int dim = daysInMonth(tmv.tm_mon, tmv.tm_year + 1900);
    int col = first, row = 0;
    for (int d = 1; d <= dim; d++) {
      int x = 14 + col * 15, y = 82 + row * 7;
      if (d == tmv.tm_mday) fb.fillRect(x - 2, y - 1, 13, 8, rgb(255, 150, 200));
      fb.setCursor(x + (d < 10 ? 2 : 0), y); fb.print(d);
      if (++col == 7) { col = 0; row++; }
    }
  }
  uint16_t catCol = rgb(255, 170, 80);
  fb.fillCircle(138, 116, 9, catCol);
  fb.fillCircle(138, 104, 6, catCol);
  fb.fillTriangle(132, 100, 133, 92, 137, 99, catCol);
  fb.fillTriangle(139, 99, 143, 92, 144, 100, catCol);
  fb.drawFastHLine(135, 104, 3, ST77XX_BLACK);
  fb.fillRect(148, 116, 9, 10, rgb(200, 90, 60));
  fb.fillCircle(152, 110, 5, rgb(80, 200, 110));
  int wy = 60 + (int)(sinf(frameNo * 0.1f) * 3);
  fb.fillCircle(150, wy, 2, rgb(255, 240, 150));
  fb.drawPixel(146, wy - 3, ST77XX_WHITE);
  fb.drawPixel(154, wy + 4, ST77XX_WHITE);
}
// ================= CATCH =================
struct Blk { int x, y; bool on; uint16_t col; };
Blk cb[4];
int ctPaddle, ctScore, ctLives;
bool ctOver;

void ctInit() {
  ctPaddle = 68; ctScore = 0; ctLives = 3; ctOver = false;
  for (auto& b : cb) b.on = false;
}

void ctUpdate() {
  if (ctOver) { if (pressed(BTN_A)) ctInit(); return; }
  if (held(BTN_LEFT))  ctPaddle -= 3;
  if (held(BTN_RIGHT)) ctPaddle += 3;
  ctPaddle = constrain(ctPaddle, 0, 136);
  if (random(100) < 5) {
    for (auto& b : cb) {
      if (!b.on) {
        b.x = random(0, 152); b.y = 10; b.on = true;
        b.col = rgb((uint8_t)random(120, 256), (uint8_t)random(120, 256), (uint8_t)random(120, 256));
        break;
      }
    }
  }
  for (auto& b : cb) {
    if (!b.on) continue;
    b.y += 2 + ctScore / 5;
    if (b.y >= 116) {
      b.on = false;
      if (b.x + 6 >= ctPaddle && b.x <= ctPaddle + 24) ctScore++;
      else if (--ctLives <= 0) ctOver = true;
    }
  }
}

void ctDraw() {
  themeBackground();
  for (auto& b : cb) if (b.on) fb.fillRoundRect(b.x, b.y, 6, 6, 2, b.col);
  fb.fillRoundRect(ctPaddle, 120, 24, 4, 2, T().accent);
  fb.setTextColor(T().text); fb.setCursor(2, 2); fb.print("SCORE "); fb.print(ctScore);
  fb.setCursor(100, 2); fb.print("LIVES "); fb.print(ctLives);
  if (ctOver) banner("GAME OVER", "A retry  B menu");
}
// ================= SHOOTER (original first-person game) =================
const char shMap[12][13] = {
  "############",
  "#..........#",
  "#.####.###.#",
  "#.#....#...#",
  "#.#.##.#.#.#",
  "#...#..#.#.#",
  "###.#.##.#.#",
  "#...#....#.#",
  "#.#####.##.#",
  "#.....#....#",
  "#.###.#.##.#",
  "############"
};

float shX, shY, shAng;
int shHealth, shAmmo, shKills, shShootFlash, shHurtFlash;
bool shOver;
struct Foe { float x, y; int hp; bool alive; };
Foe foes[5];
unsigned long shLastFoeMove = 0;

char shCell(int x, int y) {
  if (x < 0 || x >= 12 || y < 0 || y >= 12) return '#';
  return shMap[y][x];
}

bool shWalk(float x, float y) { return shCell((int)x, (int)y) == '.'; }

void shInit() {
  shX = 1.5f; shY = 1.5f; shAng = 0;
  shHealth = 100; shAmmo = 20; shKills = 0;
  shShootFlash = 0; shHurtFlash = 0; shOver = false;
  foes[0] = {9.5f, 1.5f, 2, true};
  foes[1] = {5.5f, 7.5f, 2, true};
  foes[2] = {2.5f, 9.5f, 2, true};
  foes[3] = {9.5f, 9.5f, 2, true};
  foes[4] = {7.5f, 5.5f, 2, true};
  shLastFoeMove = millis();
}

void shShoot() {
  if (shAmmo <= 0) return;
  shAmmo--;
  shShootFlash = 4;
  int best = -1;
  float bestD = 99;
  for (int i = 0; i < 5; i++) {
    if (!foes[i].alive) continue;
    float dx = foes[i].x - shX, dy = foes[i].y - shY;
    float d = sqrtf(dx * dx + dy * dy);
    float ang = atan2f(dy, dx) - shAng;
    while (ang > PI) ang -= 2 * PI;
    while (ang < -PI) ang += 2 * PI;
    if (fabsf(ang) < 0.18f && d < bestD) { bestD = d; best = i; }
  }
  if (best >= 0) {
    foes[best].hp--;
    if (foes[best].hp <= 0) { foes[best].alive = false; shKills++; }
  }
}

void shUpdate() {
  // B fires on a short press; hold B for 700 ms to leave the shooter.
  static unsigned long bStart = 0;
  static bool bTracking = false;
  if (pressed(BTN_B)) {
    bStart = millis();
    bTracking = true;
  }
  if (bTracking && held(BTN_B) && millis() - bStart >= 700) {
    bTracking = false;
    scene = MENU;
    return;
  }
  if (bTracking && !held(BTN_B) && (padPrev & BTN_B)) {
    bTracking = false;
    if (!shOver) shShoot();
  }

  if (shOver) { if (pressed(BTN_A)) shInit(); return; }

  if (held(BTN_LEFT))  shAng -= 0.07f;
  if (held(BTN_RIGHT)) shAng += 0.07f;
  if (shAng < 0) shAng += 2 * PI;
  if (shAng > 2 * PI) shAng -= 2 * PI;


  if (held(BTN_A)) {
    float nx = shX + cosf(shAng) * 0.08f, ny = shY + sinf(shAng) * 0.08f;
    if (shWalk(nx, shY)) shX = nx;
    if (shWalk(shX, ny)) shY = ny;
  }

  unsigned long now = millis();
  if (now - shLastFoeMove > 400) {
    shLastFoeMove = now;
    for (int i = 0; i < 5; i++) {
      if (!foes[i].alive) continue;
      float dx = shX - foes[i].x, dy = shY - foes[i].y;
      float d = sqrtf(dx * dx + dy * dy);
      if (d < 0.6f) {
        shHealth -= 5; shHurtFlash = 4;
        if (shHealth <= 0) shOver = true;
      } else if (d < 6.0f) {
        float nx = foes[i].x + dx / d * 0.25f, ny = foes[i].y + dy / d * 0.25f;
        if (shWalk(nx, foes[i].y)) foes[i].x = nx;
        if (shWalk(foes[i].x, ny)) foes[i].y = ny;
      }
    }
  }

  if (shShootFlash > 0) shShootFlash--;
  if (shHurtFlash > 0) shHurtFlash--;
  if (shKills >= 5) shOver = true;
}

void shDraw() {
  fb.fillRect(0, 0, 160, 64, rgb(40, 60, 110));
  fb.fillRect(0, 64, 160, 64, rgb(80, 60, 50));

  float dirX = cosf(shAng), dirY = sinf(shAng);
  float plX = -dirY * 0.66f, plY = dirX * 0.66f;

  for (int col = 0; col < 80; col++) {
    float camX = 2.0f * col / 80.0f - 1.0f;
    float rdx = dirX + plX * camX, rdy = dirY + plY * camX;
    int mapX = (int)shX, mapY = (int)shY;
    float ddx = fabsf(rdx) < 1e-6f ? 1e30f : fabsf(1.0f / rdx);
    float ddy = fabsf(rdy) < 1e-6f ? 1e30f : fabsf(1.0f / rdy);
    int stepX = rdx < 0 ? -1 : 1, stepY = rdy < 0 ? -1 : 1;
    float sideX = rdx < 0 ? (shX - mapX) * ddx : (mapX + 1.0f - shX) * ddx;
    float sideY = rdy < 0 ? (shY - mapY) * ddy : (mapY + 1.0f - shY) * ddy;
    char hit = '#';
    int side = 0;
    for (int i = 0; i < 24; i++) {
      if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
      else               { sideY += ddy; mapY += stepY; side = 1; }
      hit = shCell(mapX, mapY);
      if (hit != '.') break;
    }
    float dist = side == 0 ? sideX - ddx : sideY - ddy;
    if (dist < 0.1f) dist = 0.1f;
    int lineH = min(128, (int)(110.0f / dist));
    uint16_t color = side == 0 ? rgb(150, 110, 170) : rgb(110, 80, 140);
    if (shHurtFlash > 0) color = rgb(180, 60, 60);
    fb.fillRect(col * 2, 64 - lineH / 2, 2, lineH, color);
  }

  // Enemies as simple original shapes
  for (int i = 0; i < 5; i++) {
    if (!foes[i].alive) continue;
    float dx = foes[i].x - shX, dy = foes[i].y - shY;
    float ang = atan2f(dy, dx) - shAng;
    while (ang > PI) ang -= 2 * PI;
    while (ang < -PI) ang += 2 * PI;
    float d = sqrtf(dx * dx + dy * dy);
    if (fabsf(ang) < 0.9f && d > 0.2f) {
      int sx = 80 + (int)(ang / 0.9f * 80);
      int size = min(60, (int)(40.0f / d));
      uint16_t body = (foes[i].hp > 1) ? rgb(255, 90, 60) : rgb(255, 170, 60);
      fb.fillCircle(sx, 64 + size / 3, size / 3, body);
      fb.fillRect(sx - size / 3, 64 + size / 3, size * 2 / 3, size / 2, body);
      fb.fillRect(sx - size / 5, 64 - size / 6, size / 6, size / 6, ST77XX_WHITE);
      fb.fillRect(sx + size / 8, 64 - size / 6, size / 6, size / 6, ST77XX_WHITE);
    }
  }

  int gx = 110, gy = 96 + (shShootFlash > 0 ? -3 : 0);
  fb.fillRect(gx, gy, 30, 10, rgb(60, 60, 70));
  fb.fillRect(gx + 22, gy - 4, 8, 14, rgb(45, 45, 55));
  if (shShootFlash > 0) fb.fillCircle(gx + 30, gy - 6, 6, rgb(255, 230, 120));

  fb.drawFastHLine(76, 64, 8, ST77XX_WHITE);
  fb.drawFastVLine(80, 60, 8, ST77XX_WHITE);

  fb.fillRect(0, 0, 160, 10, rgb(10, 5, 25));
  fb.setTextColor(ST77XX_YELLOW); fb.setCursor(2, 1);
  fb.print("HP "); fb.print(shHealth);
  fb.setCursor(60, 1); fb.print("AMMO "); fb.print(shAmmo);
  fb.setCursor(118, 1); fb.print("KILL "); fb.print(shKills);

  if (shOver) {
    if (shKills >= 5) banner("CLEARED!", "A retry  hold B menu");
    else banner("GAME OVER", "A retry  hold B menu");
  }
}
// ================= OFFLINE SLEEP SCREEN (owl, works with Wi-Fi off) =================
struct Star { int x, y, phase; };
Star owlStars[14];
bool owlInit = false;

void owlSetup() {
  for (int i = 0; i < 14; i++) {
    owlStars[i] = {(i * 47 + 9) % 160, (i * 23 + 5) % 70, i * 7};
  }
  owlInit = true;
}

void owlSky() {
  for (int y = 0; y < 128; y++) {
    uint8_t r = 8 + y / 14, g = 6 + y / 20, b = 30 + y / 3;
    fb.drawFastHLine(0, y, 160, rgb(r, g, b));
  }
}

void owlDrawOwl(int cx, int cy, bool blink) {
  uint16_t body = rgb(120, 85, 60), belly = rgb(230, 200, 150), dark = rgb(60, 40, 30);
  int bob = (int)(sinf(frameNo * 0.08f) * 1.5f);
  cy += bob;
  fb.fillRoundRect(cx - 14, cy - 12, 28, 30, 12, body);
  fb.fillCircle(cx - 13, cy - 13, 4, body);
  fb.fillCircle(cx + 13, cy - 13, 4, body);
  fb.fillCircle(cx, cy + 6, 9, belly);
  fb.fillTriangle(cx, cy - 18, cx - 6, cy - 9, cx + 6, cy - 9, body);
  if (blink) {
    fb.drawFastHLine(cx - 9, cy - 4, 7, dark);
    fb.drawFastHLine(cx + 2, cy - 4, 7, dark);
  } else {
    fb.fillCircle(cx - 6, cy - 4, 5, rgb(255, 230, 120));
    fb.fillCircle(cx + 6, cy - 4, 5, rgb(255, 230, 120));
    fb.fillCircle(cx - 6, cy - 4, 2, dark);
    fb.fillCircle(cx + 6, cy - 4, 2, dark);
  }
  fb.fillTriangle(cx - 2, cy, cx + 2, cy, cx, cy + 4, rgb(240, 170, 60));
  fb.fillRect(cx - 9, cy + 20, 4, 3, rgb(240, 170, 60));
  fb.fillRect(cx + 5, cy + 20, 4, 3, rgb(240, 170, 60));
  fb.fillRect(cx - 14, cy + 18, 28, 2, rgb(90, 60, 40));
}

void offlineSleepDraw() {
  if (!owlInit) owlSetup();
  owlSky();

  for (int i = 0; i < 14; i++) {
    bool on = ((frameNo / 8 + owlStars[i].phase) % 6) < 4;
    fb.drawPixel(owlStars[i].x, owlStars[i].y, on ? ST77XX_WHITE : rgb(90, 80, 140));
  }

  int mx = 130 + (int)(sinf(frameNo * 0.02f) * 4);
  fb.fillCircle(mx, 22, 13, rgb(255, 245, 200));
  fb.fillCircle(mx + 5, 18, 11, rgb(18, 10, 40));

  int cloudX = (int)((frameNo * 0.4f)) % 220 - 40;
  fb.fillRoundRect(cloudX, 46, 40, 10, 5, rgb(60, 50, 100));
  fb.fillRoundRect(cloudX + 14, 40, 26, 10, 5, rgb(60, 50, 100));

  fb.fillRect(0, 108, 160, 20, rgb(20, 40, 35));
  fb.fillRect(0, 104, 160, 4, rgb(30, 60, 50));

  bool blink = (frameNo % 90) < 4;
  owlDrawOwl(80, 80, blink);

  struct tm tmv;
  if (timeReady && getLocalTime(&tmv, 5)) {
    int h = tmv.tm_hour % 12; if (h == 0) h = 12;
    fb.setTextSize(2); fb.setTextColor(rgb(255, 245, 255));
    fb.setCursor(40, 4);
    if (h < 10) fb.print('0');
    fb.print(h); fb.print(':');
    if (tmv.tm_min < 10) fb.print('0');
    fb.print(tmv.tm_min);
    fb.setTextSize(1);
  } else {
    fb.setTextColor(rgb(180, 170, 220));
    fb.setCursor(34, 6);
    fb.print("Offline sleep");
  }
  fb.setTextColor(rgb(180, 170, 220));
  fb.setCursor(40, 118);
  fb.print("press any button");
}
// ================= MAPS + WEATHER (LIGHTWEIGHT) =================
// Uses OpenStreetMap Nominatim for place lookup and Open-Meteo for current weather.
// This is a compact place locator, not a raster street-tile renderer.
String mapName = "New Delhi", mapDisplay = "New Delhi, Delhi, India", mapLat = "28.6139", mapLon = "77.2090", mapErr = "";
String weatherLine = "", weatherErr = "";
float mapLatF = 28.6139f, mapLonF = 77.2090f;
bool mapHasPlace = true;

bool mapGeocode(const String& query) {
  if (!wifiConnect()) { mapErr = "Connect Wi-Fi first"; return false; }
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(12000);
  String url = String("https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=") + urlEnc(query);
  if (!http.begin(client, url)) { mapErr = "Network start failed"; return false; }
  http.setUserAgent("ESP-DECK/1.0 (personal device)");
  int code = http.GET();
  if (code != 200) { http.end(); mapErr = "Map search failed"; return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) { mapErr = "Place not found"; return false; }
  JsonObject item = doc[0];
  mapLat = item["lat"].as<String>(); mapLon = item["lon"].as<String>();
  mapDisplay = item["display_name"].as<String>();
  if (mapDisplay.length() > 100) mapDisplay = mapDisplay.substring(0, 100);
  mapLatF = mapLat.toFloat(); mapLonF = mapLon.toFloat();
  mapName = query; mapHasPlace = true; mapErr = "";
  return true;
}

void mapSearchUpdate() {
  if (!kbUpdate()) return;
  String q = kbText; q.trim();
  if (!q.length()) { mapErr = "Enter a place name"; return; }
  themeBackground(); banner("SEARCHING MAP", q.substring(0, 18).c_str()); present();
  if (mapGeocode(q)) scene = MAP_RESULT;
}

void mapSearchDraw() {
  kbDraw("MAPS - FIND PLACE");
  if (mapErr.length()) { fb.setTextColor(T().accent); fb.setCursor(4, 101); fb.print(mapErr.substring(0, 25)); }
}

void mapResultUpdate() {
  if (pressed(BTN_LEFT) || pressed(BTN_RIGHT)) { scene = WEATHER; weatherErr = ""; weatherLine = ""; }
  if (pressed(BTN_A)) {
    if (!sdReady) { mapErr = "No SD card"; return; }
    useSdBus(); SD.remove("/map_fav.txt"); File f = SD.open("/map_fav.txt", FILE_WRITE);
    if (f) { f.println(mapName); f.println(mapLat); f.println(mapLon); f.close(); mapErr = "Saved favourite"; }
    else mapErr = "Save failed";
    useDisplayBus();
  }
}

void mapResultDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 13, T().accent); fb.setTextColor(rgb(20,10,30)); fb.setCursor(4,3); fb.print("MAPS / PLACE");
  // Schematic locator panel: grid + centred marker, coordinates identify the searched place.
  fb.fillRoundRect(5, 18, 150, 66, 5, rgb(20,45,48));
  for (int x=12; x<154; x+=18) fb.drawFastVLine(x,22,58,rgb(45,85,78));
  for (int y=25; y<82; y+=14) fb.drawFastHLine(9,y,142,rgb(45,85,78));
  fb.drawLine(12,72,52,48,rgb(220,190,100)); fb.drawLine(52,48,90,58,rgb(220,190,100)); fb.drawLine(90,58,148,30,rgb(220,190,100));
  fb.fillCircle(80,49,5,ST77XX_RED); fb.drawCircle(80,49,8,ST77XX_WHITE);
  fb.setTextColor(T().text); fb.setCursor(5,90); fb.print(mapName.substring(0,24));
  fb.setCursor(5,101); fb.print(mapLat.substring(0,10)); fb.print(","); fb.print(mapLon.substring(0,10));
  fb.setCursor(5,112); fb.setTextColor(T().accent); fb.print("L/R weather  A save fav");
  fb.setCursor(5,122); fb.setTextColor(rgb(170,170,190)); fb.print("Data: OpenStreetMap");
}

bool mapFetchWeather() {
  if (!mapHasPlace || !wifiConnect()) {
    weatherErr = "No place / Wi-Fi";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
    mapLat + "&longitude=" + mapLon +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&timezone=auto";
  if (!http.begin(client, url)) {
    weatherErr = "Could not start request";
    return false;
  }
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    weatherErr = String("HTTP error ") + httpCode;
    http.end();
    return false;
  }

  // Parse the response body once; keep only the fields needed for this screen.
  String payload = http.getString();
  http.end();
  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["apparent_temperature"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["wind_speed_10m"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    weatherErr = String("JSON error: ") + err.c_str();
    return false;
  }
  JsonObject c = doc["current"].as<JsonObject>();
  if (c.isNull() || c["temperature_2m"].isNull() ||
      c["relative_humidity_2m"].isNull() || c["weather_code"].isNull() ||
      c["wind_speed_10m"].isNull()) {
    weatherErr = "Missing current fields";
    return false;
  }
  int wc = c["weather_code"].as<int>();
  const char* condition = "Cloudy";
  if (wc == 0) condition = "Clear";
  else if (wc <= 3) condition = "Partly cloudy";
  else if (wc == 45 || wc == 48) condition = "Fog";
  else if (wc >= 51 && wc <= 67) condition = "Drizzle / rain";
  else if (wc >= 71 && wc <= 77) condition = "Snow";
  else if (wc >= 80 && wc <= 82) condition = "Rain showers";
  else if (wc >= 95) condition = "Thunderstorm";

  weatherLine = String(c["temperature_2m"].as<float>(), 1);
  weatherErr = String(condition) +
    "  Feels " + String(c["apparent_temperature"].as<float>(), 1) + "C" +
    "  H " + String(c["relative_humidity_2m"].as<int>()) + "%" +
    "  Wind " + String(c["wind_speed_10m"].as<float>(), 1);
  return true;
}
void weatherEnter() {
  mapName = "New Delhi";
  mapDisplay = "New Delhi, Delhi, India";
  mapLat = "28.6139";
  mapLon = "77.2090";
  mapLatF = 28.6139f;
  mapLonF = 77.2090f;
  mapHasPlace = true;
  weatherLine = "";
  weatherErr = "Connecting...";
  mapFetchWeather();
}
void weatherUpdate() {
  if (pressed(BTN_A)) { weatherLine = ""; weatherErr = "Refreshing..."; mapFetchWeather(); }
  if (pressed(BTN_LEFT) || pressed(BTN_RIGHT)) scene = MENU;
}
void weatherDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 15, T().accent);
  fb.setTextColor(rgb(20, 10, 30));
  fb.setCursor(5, 4);
  fb.print("WEATHER / NEW DELHI");

  fb.drawRoundRect(4, 21, 152, 49, 5, T().accent);
  fb.setTextColor(T().text);
  fb.setCursor(11, 27);
  fb.print("CURRENT");
  if (weatherLine.length()) {
    fb.setTextSize(3);
    fb.setCursor(11, 39);
    fb.print(weatherLine);
    fb.setTextSize(1);
    fb.setCursor(112, 51);
    fb.print("C");
  } else {
    fb.setCursor(11, 47);
    fb.print("Loading temperature...");
  }

  fb.setTextColor(T().text);
  if (weatherLine.length()) {
    int split = weatherErr.indexOf("  Feels ");
    String condition = split >= 0 ? weatherErr.substring(0, split) : weatherErr;
    fb.setCursor(7, 78);
    fb.print(condition.substring(0, 24));
    String metrics = split >= 0 ? weatherErr.substring(split + 2) : "";
    fb.setCursor(7, 94);
    fb.print(metrics.substring(0, 25));
    fb.setCursor(7, 107);
    fb.print("Open-Meteo / Live data");
  } else {
    fb.setCursor(7, 82);
    fb.print(weatherErr.substring(0, 25));
    fb.setCursor(7, 99);
    fb.print("Check Wi-Fi, press A");
  }
  fb.setTextColor(T().accent);
  fb.setCursor(5, 119);
  fb.print("L/R menu   A refresh");
}
// ================= SYSTEM INFO =================
void systemInfoDraw() {
  themeBackground();
  fb.fillRect(0, 0, 160, 13, T().accent);
  fb.setTextColor(rgb(20, 10, 30)); fb.setCursor(4, 3); fb.print("SYSTEM INFO");
  fb.setTextColor(T().text);
  fb.setCursor(6, 24); fb.print("ESP32-C3 SuperMini");
  fb.setCursor(6, 42); fb.print("Free heap:");
  fb.setCursor(82, 42); fb.print(ESP.getFreeHeap()); fb.print(" B");
  fb.setCursor(6, 58); fb.print("Uptime:");
  fb.setCursor(82, 58); fb.print(millis() / 1000); fb.print(" sec");
  fb.setCursor(6, 74); fb.print("SD card:");
  fb.setCursor(82, 74); fb.print(sdReady ? "READY" : "NOT FOUND");
  fb.setCursor(6, 86); fb.print("Wi-Fi:");
  fb.setCursor(82, 86); fb.print(wifiEnabled ? "ENABLED" : "DISABLED");
  fb.setCursor(6, 100); fb.print("Draw/max:");
  fb.setCursor(82, 100); fb.print(lastPresentMs); fb.print('/'); fb.print(maxPresentMs); fb.print("ms");
  fb.setCursor(6, 116); fb.print("B = back to menu");
}


// ================= NEON RALLY (pseudo-3D road racer) =================
float nrPlayer = 80.0f, nrRoad = 0.0f, nrSpeed = 1.0f;
int nrScore = 0, nrCrash = 0;
struct NRCar { float x, z, lane; bool on; };
NRCar nrCars[5];
void nrInit() {
  nrPlayer = 80; nrRoad = 0; nrSpeed = 1; nrScore = 0; nrCrash = 0;
  for (int i=0;i<5;i++) { nrCars[i].on=false; nrCars[i].z=0; }
}
void nrUpdate() {
  if (held(BTN_LEFT)) nrPlayer -= 2.4f;
  if (held(BTN_RIGHT)) nrPlayer += 2.4f;
  nrPlayer = constrain(nrPlayer, 20.0f, 140.0f);
  nrSpeed = pressed(BTN_A) ? 2.8f : 1.7f;
  nrRoad += nrSpeed; if (nrRoad > 10000) nrRoad = 0;
  if (random(100) < 8) for (int i=0;i<5;i++) if (!nrCars[i].on) {
    nrCars[i].on=true; nrCars[i].z=0; nrCars[i].lane=random(0,3); break;
  }
  for (int i=0;i<5;i++) if (nrCars[i].on) {
    nrCars[i].z += nrSpeed * 1.25f;
    if (nrCars[i].z > 100) { nrCars[i].on=false; nrScore++; }
    float cx = 80 + (nrCars[i].lane-1)*((nrCars[i].z+10)*0.48f);
    if (nrCars[i].z > 82 && nrCars[i].z < 96 && fabsf(cx-nrPlayer)<13) {
      nrCars[i].on=false; nrCrash++; nrScore=max(0,nrScore-2);
    }
  }
}
void nrDraw() {
  fb.fillScreen(rgb(4,8,22));
  for (int y=0;y<54;y+=8) fb.drawFastHLine(0,y,160,rgb(12,20,42));
  // Neon skyline
  for (int i=0;i<9;i++) {
    int x=i*20, h=10+(i*17%24);
    fb.fillRect(x,52-h,14,h,rgb(12,18,42));
    fb.drawFastVLine(x+2,54-h, h-2, rgb(30,70,110));
  }
  // Perspective road trapezoid and animated lane dashes
  fb.fillTriangle(57,35,103,35,158,127,rgb(28,32,55));
  fb.fillTriangle(57,35,80,35,62,127,rgb(18,24,43));
  fb.drawLine(57,35,5,127,rgb(0,220,255)); fb.drawLine(103,35,155,127,rgb(0,220,255));
  for (int lane=0;lane<2;lane++) for(int k=0;k<5;k++) {
    int y=38+(int)((k*21+(int)nrRoad)%90), w=1+(y-35)/22;
    int x=80+(lane?1:-1)*(4+(y-35)/2);
    fb.fillRect(x-w,y,w*2,5,rgb(50,90,130));
  }
  for(int i=0;i<5;i++) if(nrCars[i].on) {
    int y=38+(int)nrCars[i].z*0.88f, x=80+(int)((nrCars[i].lane-1)*(nrCars[i].z+10)*0.48f);
    int w=4+(int)nrCars[i].z/18;
    fb.fillRoundRect(x-w,y,w*2,w+4,2,rgb(255,70,110));
    fb.fillRect(x-w+1,y+2,2,2,rgb(255,230,120)); fb.fillRect(x+w-3,y+2,2,2,rgb(255,230,120));
  }
  fb.fillRoundRect((int)nrPlayer-7,108,14,17,4,rgb(0,220,255));
  fb.fillRect((int)nrPlayer-4,111,8,4,rgb(180,245,255));
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(3,3); fb.print("NEON RALLY");
  fb.setCursor(105,3); fb.print(nrScore);
  fb.setTextColor(rgb(255,100,130)); fb.setCursor(3,15); fb.print("HITS "); fb.print(nrCrash);
  fb.setTextColor(rgb(100,220,255)); fb.setCursor(3,117); fb.print("L/R steer  A boost");
}

// ================= ASTEROIDS (particle arcade) =================
float asX=80, asY=90, asVX=0, asVY=0;
int asScore=0, asLives=3, asFire=0;
struct ASRock { float x,y,vx,vy; uint8_t size; bool on; };
ASRock asRocks[8];
struct ASShot { float x,y,vy; bool on; };
ASShot asShots[4];
void asInit() {
  asX=80; asY=92; asVX=asVY=0; asScore=0; asLives=3; asFire=0;
  for(int i=0;i<8;i++) asRocks[i].on=false;
  for(int i=0;i<4;i++) asShots[i].on=false;
}
void asUpdate() {
  if (held(BTN_LEFT)) asX-=2.2f;
  if (held(BTN_RIGHT)) asX+=2.2f;
  asX=constrain(asX,8.0f,152.0f);
  if (pressed(BTN_A)) for(int i=0;i<4;i++) if(!asShots[i].on) {
    asShots[i]={asX,asY-8,-4.8f,true}; break;
  }
  if (random(100)<7) for(int i=0;i<8;i++) if(!asRocks[i].on) {
    asRocks[i]={ (float)random(8,152), -8, (float)random(-8,9)/10.0f, (float)random(8,18)/10.0f, (uint8_t)random(4,9), true }; break;
  }
  for(int i=0;i<4;i++) if(asShots[i].on) {
    asShots[i].y+=asShots[i].vy; if(asShots[i].y<0) asShots[i].on=false;
  }
  for(int i=0;i<8;i++) if(asRocks[i].on) {
    ASRock &r=asRocks[i]; r.x+=r.vx; r.y+=r.vy;
    if(r.x<4||r.x>156) r.vx=-r.vx;
    if(r.y>132) { r.on=false; if(--asLives<=0) asLives=3; }
    if(fabsf(r.x-asX)<r.size+5 && r.y>asY-8 && r.y<asY+8) {
      r.on=false; asLives--; if(asLives<=0) { asLives=3; asScore=max(0,asScore-5); }
    }
    for(int j=0;j<4;j++) if(r.on && asShots[j].on && fabsf(r.x-asShots[j].x)<r.size+2 && fabsf(r.y-asShots[j].y)<r.size+3) {
      r.on=false; asShots[j].on=false; asScore+=10;
    }
  }
}
void asDraw() {
  fb.fillScreen(rgb(2,4,18));
  for(int i=0;i<38;i++) {
    int x=(i*43+7)%160, y=(i*29+(int)(frameNo*(1+i%3)))%128;
    fb.drawPixel(x,y, (i%4)?rgb(75,95,145):rgb(180,210,255));
  }
  // Planet glow
  fb.fillCircle(126,24,17,rgb(22,28,75)); fb.drawCircle(126,24,20,rgb(55,90,160));
  for(int i=0;i<8;i++) if(asRocks[i].on) {
    ASRock &r=asRocks[i]; int x=(int)r.x,y=(int)r.y,sz=r.size;
    fb.drawLine(x-sz,y,x-sz/2,y-sz,rgb(160,180,210));
    fb.drawLine(x-sz/2,y-sz,x+sz/2,y-sz+2,rgb(160,180,210));
    fb.drawLine(x+sz/2,y-sz+2,x+sz,y+sz/2,rgb(160,180,210));
    fb.drawLine(x+sz,y+sz/2,x-sz/2,y+sz,rgb(160,180,210));
    fb.drawLine(x-sz/2,y+sz,x-sz,y,rgb(160,180,210));
  }
  for(int i=0;i<4;i++) if(asShots[i].on) {
    fb.drawFastVLine((int)asShots[i].x,(int)asShots[i].y,7,rgb(80,240,255));
    fb.drawPixel((int)asShots[i].x-1,(int)asShots[i].y+3,ST77XX_WHITE);
  }
  // Ship and thruster
  fb.fillTriangle((int)asX,(int)asY-9,(int)asX-8,(int)asY+7,(int)asX+8,(int)asY+7,rgb(90,220,255));
  fb.fillTriangle((int)asX,(int)asY+12,(int)asX-3,(int)asY+6,(int)asX+3,(int)asY+6,rgb(255,130,55));
  fb.setTextColor(ST77XX_WHITE); fb.setCursor(3,3); fb.print("ASTEROIDS");
  fb.setCursor(108,3); fb.print(asScore);
  fb.setCursor(3,15); fb.print("SHIELDS "); fb.print(asLives);
  fb.setCursor(3,117); fb.print("L/R move  A fire");
}

// ================= MENU =================
const int MENU_COUNT = 26;
const char* gameNames[MENU_COUNT] = {
  "CATCH", "3D MAZE", "FOOTBALL", "PIXEL JUMP", "3D RUNNER", "SNAKE",
  "WIKIPEDIA", "DICTIONARY", "WI-FI", "LIBRARY", "PHOTOS", "NOTES",
  "STOPWATCH", "CALCULATOR", "DICE", "SETTINGS", "SLEEP",
  "MESSAGES", "TIC-TAC-TOE", "SIMON", "SHOOTER", "SYSTEM INFO", "MAPS", "WEATHER",
  "NEON RALLY", "ASTEROIDS"
};
int menuSel = 0;
float carX = 0;
float tileScale[MENU_COUNT];

uint16_t tileCol(int i) {
  static const uint16_t C[MENU_COUNT] = {
    rgb(255, 90, 120), rgb(80, 180, 255), rgb(80, 220, 120), rgb(255, 170, 60),
    rgb(90, 200, 255), rgb(80, 220, 150), rgb(230, 230, 230), rgb(120, 220, 255),
    rgb(60, 150, 255), rgb(255, 190, 60), rgb(190, 110, 255), rgb(80, 220, 200),
    rgb(255, 140, 90), rgb(255, 200, 90), rgb(255, 120, 200), rgb(150, 160, 200),
    rgb(120, 110, 220), rgb(120, 230, 255), rgb(255, 240, 150), rgb(160, 255, 200),
    rgb(255, 90, 70), rgb(120, 220, 160), rgb(80, 210, 190), rgb(90, 170, 255),
    rgb(0, 220, 255), rgb(180, 150, 255)
  };
  return C[i];
}

void drawIcon(int i, int cx, int cy, uint16_t c) {
  switch (i) {
    case 0:
      fb.fillRect(cx - 8, cy - 10, 6, 6, c);
      fb.fillRect(cx + 2, cy - 2, 6, 6, c);
      fb.fillRect(cx - 10, cy + 8, 20, 3, c);
      break;
    case 1:
      fb.drawRect(cx - 10, cy - 10, 20, 20, c);
      fb.drawFastVLine(cx, cy - 10, 10, c);
      fb.drawFastHLine(cx, cy, 10, c);
      break;
    case 2:
      fb.drawCircle(cx, cy, 9, c); fb.fillCircle(cx, cy, 3, c);
      break;
    case 3:
      fb.fillRect(cx - 10, cy + 4, 20, 4, c);
      fb.fillRect(cx - 4, cy - 6, 6, 10, c);
      break;
    case 4:
      fb.drawLine(cx - 2, cy - 8, cx - 9, cy + 9, c);
      fb.drawLine(cx + 2, cy - 8, cx + 9, cy + 9, c);
      fb.drawLine(cx - 6, cy + 4, cx + 6, cy + 4, c);
      break;
    case 5:
      fb.fillRect(cx - 9, cy - 9, 18, 4, c);
      fb.fillRect(cx - 9, cy - 9, 4, 18, c);
      fb.fillRect(cx - 9, cy + 5, 18, 4, c);
      break;
    case 6:
      fb.setTextSize(2); fb.setTextColor(c);
      fb.setCursor(cx - 6, cy - 8); fb.print("W");
      fb.setTextSize(1);
      break;
    case 7:
      fb.drawRect(cx - 10, cy - 8, 9, 16, c);
      fb.drawRect(cx + 1, cy - 8, 9, 16, c);
      fb.drawFastVLine(cx, cy - 8, 16, c);
      break;
    case 8:
      fb.drawCircle(cx, cy + 6, 9, c);
      fb.drawCircle(cx, cy + 6, 6, c);
      fb.drawCircle(cx, cy + 6, 3, c);
      fb.fillCircle(cx, cy + 6, 2, c);
      break;
    case 9:
      fb.fillRect(cx - 10, cy - 6, 20, 15, c);
      fb.fillRect(cx - 10, cy - 9, 9, 4, c);
      break;
    case 10:
      fb.drawRect(cx - 10, cy - 9, 20, 17, c);
      fb.fillTriangle(cx - 8, cy + 6, cx - 1, cy - 3, cx + 6, cy + 6, c);
      break;
    case 11:
      fb.drawLine(cx - 8, cy + 8, cx + 6, cy - 6, c);
      fb.drawLine(cx - 7, cy + 8, cx + 7, cy - 6, c);
      fb.drawLine(cx + 6, cy - 6, cx + 9, cy - 9, c);
      break;
    case 12:
      fb.drawCircle(cx, cy, 9, c);
      fb.drawLine(cx, cy, cx, cy - 6, c);
      fb.drawLine(cx, cy, cx + 4, cy, c);
      fb.fillRect(cx - 2, cy - 12, 4, 3, c);
      break;
    case 13:
      fb.drawRoundRect(cx - 8, cy - 10, 16, 20, 2, c);
      fb.fillRect(cx - 5, cy - 7, 10, 4, c);
      for (int r = 0; r < 2; r++) for (int k = 0; k < 2; k++)
        fb.fillRect(cx - 5 + k * 6, cy + r * 4 + 1, 3, 2, c);
      break;
    case 14:
      fb.fillRoundRect(cx - 9, cy - 4, 8, 8, 2, c);
      fb.fillRoundRect(cx + 1, cy - 9, 8, 8, 2, c);
      fb.fillRoundRect(cx + 1, cy + 1, 8, 8, 2, c);
      break;
    case 15:
      fb.drawCircle(cx, cy, 6, c);
      for (int k = 0; k < 6; k++) {
        float a = k * 1.0472f;
        fb.drawLine(cx + (int)(cosf(a) * 6), cy + (int)(sinf(a) * 6),
                    cx + (int)(cosf(a) * 9), cy + (int)(sinf(a) * 9), c);
      }
      break;
    case 16:
      fb.fillCircle(cx, cy, 9, c);
      fb.fillCircle(cx + 4, cy - 3, 7, rgb(40, 30, 70));
      break;
    case 17:
      fb.fillRoundRect(cx - 10, cy - 8, 20, 14, 4, c);
      fb.fillTriangle(cx - 6, cy + 6, cx - 2, cy + 6, cx - 8, cy + 11, c);
      break;
    case 18:
      fb.drawFastVLine(cx - 4, cy - 9, 18, c); fb.drawFastVLine(cx + 4, cy - 9, 18, c);
      fb.drawFastHLine(cx - 9, cy - 3, 18, c); fb.drawFastHLine(cx - 9, cy + 3, 18, c);
      break;
    case 19:
      fb.fillRect(cx - 9, cy - 9, 8, 8, c);
      fb.fillRect(cx + 1, cy - 9, 8, 8, c);
      fb.fillRect(cx - 4, cy + 1, 8, 8, c);
      break;
    case 20:
      fb.drawCircle(cx, cy, 10, c);
      fb.drawCircle(cx, cy, 4, c);
      fb.drawFastHLine(cx - 14, cy, 8, c);
      fb.drawFastHLine(cx + 6, cy, 8, c);
      fb.drawFastVLine(cx, cy - 14, 8, c);
      fb.drawFastVLine(cx, cy + 6, 8, c);
      break;
    case 21:
      fb.drawRoundRect(cx - 10, cy - 9, 20, 18, 3, c);
      fb.drawFastHLine(cx - 6, cy - 4, 12, c);
      fb.drawFastHLine(cx - 6, cy, 12, c);
      fb.drawFastHLine(cx - 6, cy + 4, 8, c);
      break;
    case 22:
      fb.drawCircle(cx, cy, 9, c); fb.fillCircle(cx, cy, 3, c);
      fb.drawLine(cx, cy-13, cx, cy-6, c); fb.drawLine(cx, cy+6, cx, cy+13, c);
      break;
    case 23:
      fb.drawCircle(cx, cy, 9, c); fb.drawFastHLine(cx-12,cy,24,c);
      fb.drawFastVLine(cx,cy-12,24,c); fb.drawLine(cx-6,cy-6,cx+6,cy+6,c);
      break;
    case 24:
      fb.drawLine(cx-10,cy+9,cx-5,cy-5,c); fb.drawLine(cx-5,cy-5,cx+2,cy+2,c);
      fb.drawLine(cx+2,cy+2,cx+10,cy-10,c); fb.drawFastHLine(cx-12,cy+11,24,c);
      break;
    case 25:
      fb.drawCircle(cx,cy,10,c); fb.drawLine(cx-10,cy-4,cx+5,cy-10,c);
      fb.drawLine(cx+5,cy-10,cx+10,cy+4,c); fb.drawLine(cx+10,cy+4,cx-3,cy+10,c);
      break;
  }
}

void enterSleepScreen() {
  wifiOn = wifiEnabled && (WiFi.status() == WL_CONNECTED);
  scene = wifiOn ? CLOCK : OFFLINE_SLEEP;
}

void menuUpdate() {
  if (pressed(BTN_LEFT))  menuSel = (menuSel + MENU_COUNT - 1) % MENU_COUNT;
  if (pressed(BTN_RIGHT)) menuSel = (menuSel + 1) % MENU_COUNT;
  if (!pressed(BTN_A)) return;
  switch (menuSel) {
    case 0:  ctInit(); scene = CATCH; break;
    case 1:  mazeInit(); scene = MAZE; break;
    case 2:  fbInit(); scene = FOOTBALL; break;
    case 3:  pjInit(); scene = PJ; break;
    case 4:  runInit(); scene = RUNNER; break;
    case 5:  snInit(); scene = SNAKE; break;
    case 6:  kbText = ""; kbIdx = 0; wikiErr = ""; scene = WIKI_SEARCH; break;
    case 7:  kbText = ""; kbIdx = 0; dictErr = ""; scene = DICT_SEARCH; break;
    case 8:  scanNetworks(); wifiMsg = ""; scene = WIFI_LIST; break;
    case 9:  scanStories(); fileSel = 0; scene = FILES; break;
    case 10: photosInit(); scene = PHOTOS; break;
    case 11: kbText = ""; kbIdx = 0; noteMsg = ""; scene = NOTES; break;
    case 12: swRun = false; swAccum = 0; scene = STOPWATCH; break;
    case 13: calcExpr = ""; calcOut = ""; calcIdx = 0; scene = CALC; break;
    case 14: diceAnim = false; scene = DICE; break;
    case 15: setRow = 0; settingsMsg = ""; scene = SETTINGS; break;
    case 16: enterSleepScreen(); break;
    case 17: msgCompose = false; msgNextPoll = 0; msgStatus = ""; scene = MSG; break;
    case 18: tttInit(); scene = TTT; break;
    case 19: simInit(); scene = SIMON; break;
    case 20: shInit(); scene = SHOOTER; break;
    case 21: scene = SYSTEM_INFO; break;
    case 22: kbText = ""; kbIdx = 0; mapErr = ""; scene = MAP_SEARCH; break;
    case 23: weatherEnter(); scene = WEATHER; break;
    case 24: nrInit(); scene = NEON_RALLY; break;
    case 25: asInit(); scene = ASTEROIDS; break;
  }
}

void menuDraw() {
  // Borderless animated midnight launcher. Motion is time-based and lightweight.
  float target = (float)menuSel;
  float delta = target - carX;
  if (delta > MENU_COUNT / 2.0f) carX += MENU_COUNT;
  if (delta < -MENU_COUNT / 2.0f) carX -= MENU_COUNT;
  carX += (target - carX) * 0.20f;
  if (fabsf(target - carX) < 0.025f) carX = target;

  // Slow animated blue-violet wallpaper gradient.
  const float wave = sinf(frameNo * 0.025f);
  for (int y = 0; y < 128; ++y) {
    uint8_t r = 4 + (uint8_t)(2.0f * (1.0f + wave)) + y / 45;
    uint8_t g = 6 + y / 36;
    uint8_t b = 19 + y / 5 + (uint8_t)(3.0f * (1.0f + sinf(frameNo * 0.018f + y * 0.035f)));
    fb.drawFastHLine(0, y, 160, rgb(r, g, b));
  }

  // Drifting star field with twinkle; positions move slowly across the screen.
  for (int i = 0; i < 30; ++i) {
    int x = (int)((i * 47 + 13 + frameNo * (1 + (i % 3))) % 168) - 4;
    int y = (i * 29 + 7 + (int)(frameNo / (5 + i % 4))) % 112;
    bool twinkle = ((frameNo / 5 + i * 3) % 9) < 4;
    uint16_t c = twinkle ? rgb(190, 205, 255) : rgb(65, 82, 139);
    fb.drawPixel(x, y, c);
    if (twinkle && (i % 6 == 0)) fb.drawPixel(x + 1, y, c);
  }

  // Slowly orbiting ambient glow and a small moon-like highlight.
  int gx = 122 + (int)(5.0f * sinf(frameNo * 0.035f));
  int gy = 28 + (int)(3.0f * cosf(frameNo * 0.028f));
  fb.fillCircle(gx, gy, 28, rgb(10, 17, 47));
  fb.fillCircle(gx, gy, 18, rgb(14, 22, 59));
  fb.fillCircle(gx + 5, gy - 4, 7, rgb(23, 34, 78));

  fb.setTextColor(rgb(155, 177, 230)); fb.setTextSize(1);
  fb.setCursor(8, 7); fb.print("ESP DECK");
  fb.setTextColor(rgb(110, 132, 190)); fb.setCursor(119, 7);
  if (menuSel + 1 < 10) fb.print('0');
  fb.print(menuSel + 1); fb.print(" / 24");

  int prev = (menuSel + MENU_COUNT - 1) % MENU_COUNT;
  int next = (menuSel + 1) % MENU_COUNT;
  float pulse = 1.0f + 0.07f * sinf(frameNo * 0.11f);
  int iconR = (int)(18.0f * pulse);
  float sideDrift = 2.0f * sinf(frameNo * 0.07f);
  fb.fillCircle(20, 63 + (int)sideDrift, 13, rgb(20, 30, 66));
  drawIcon(prev, 20, 63 + (int)sideDrift, rgb(105, 124, 178));
  fb.fillCircle(140, 63 - (int)sideDrift, 13, rgb(20, 30, 66));
  drawIcon(next, 140, 63 - (int)sideDrift, rgb(105, 124, 178));

  uint16_t accent = tileCol(menuSel);
  // Expanding/contracting halo, with no enclosing card or border.
  int halo = 26 + (int)(3.0f * (1.0f + sinf(frameNo * 0.09f)));
  fb.fillCircle(80, 57, halo + 5, rgb(10, 17, 42));
  fb.drawCircle(80, 57, halo, rgb(38, 54, 105));
  fb.drawCircle(80, 57, halo - 3, rgb(27, 41, 83));
  fb.fillCircle(80, 57, iconR, accent);
  drawIcon(menuSel, 80, 57, ST77XX_WHITE);

  // App title gently slides into place after selection changes.
  const char* name = gameNames[menuSel];
  int len = (int)strlen(name);
  int titleY = 88 + (int)(2.0f * sinf(frameNo * 0.08f));
  fb.setTextColor(ST77XX_WHITE); fb.setTextSize(1);
  fb.setCursor(80 - (len * 3), titleY); fb.print(name);

  // Animated page dots and a subtle moving underline.
  for (int i = 0; i < 7; ++i) {
    int dx = 59 + i * 7;
    uint16_t dc = (i == (menuSel % 7)) ? accent : rgb(48, 62, 103);
    int rr = (i == (menuSel % 7)) ? 2 : 1;
    fb.fillCircle(dx, 105, rr, dc);
  }
  int shimmer = 18 + (int)((frameNo * 2) % 124);
  fb.drawFastVLine(shimmer, 114, 2, rgb(35, 50, 91));
  fb.setTextColor(rgb(143, 158, 199));
  fb.setCursor(13, 116); fb.print("<  LEFT / RIGHT  >");
  fb.setTextColor(rgb(205, 214, 240));
  fb.setCursor(42, 124); fb.print("A OPEN   B SLEEP");
}

// ================= NAVIGATION =================
Scene backTarget(Scene s) {
  switch (s) {
    case READER:    return FILES;
    case WIKI_READ: return WIKI_SEARCH;
    case DICT_READ: return DICT_SEARCH;
    case WIFI_PASS: return WIFI_LIST;
    case HOME:      return MENU;
    default:        return MENU;
  }
}

// ================= SETUP / LOOP =================
void setup() {
  for (int p : {PIN_LEFT, PIN_RIGHT, PIN_A, PIN_B}) pinMode(p, INPUT_PULLUP);
  useDisplayBus();
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(16000000); // Conservative SPI clock for stable ST7735 operation on jumper wires
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  useSdBus();
  sdReady = SD.begin(SD_CS);
  useDisplayBus();
  loadSettings();
  loadNtfyTopic();
  bmLoad();
  loadWifiCreds();
  for (int i = 0; i < MENU_COUNT; i++) tileScale[i] = 0.78f;
  carX = (float)menuSel;
  randomSeed(micros());
  lastActivity = millis();
  wifiApplyEnabled();
}

void loop() {
  unsigned long now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;
  frameNo++;
  readPad();
  if (pad) lastActivity = now;

  if (scene == HOME) {
    if (pressed(BTN_A)) scene = MENU;
    if (pressed(BTN_B)) enterSleepScreen();
    if (scene == HOME) { homeDraw(); present(); return; }
  }

  if (scene == CLOCK) {
    if (pad && !padPrev) { scene = HOME; lastActivity = now; return; }
    clockDraw();
    present();
    return;
  }

  if (scene == OFFLINE_SLEEP) {
    if (pad && !padPrev) { scene = HOME; lastActivity = now; return; }
    offlineSleepDraw();
    present();
    return;
  }

  if (scene == MENU && IDLE_OPT[idleIdx] && now - lastActivity > IDLE_OPT[idleIdx]) {
    enterSleepScreen();
    return;
  }

  static unsigned long bDownAt = 0;
  static bool bLong = false;
  if (scene == READER) {
    if (pressed(BTN_B)) { bDownAt = now; bLong = false; }
    if (held(BTN_B) && !bLong && now - bDownAt >= 600) { bLong = true; bmSaveCurrent(); }
    if (!held(BTN_B) && (padPrev & BTN_B) && !bLong) {
      closeBook();
      scene = FILES;
      return;
    }
  } else if (scene == MENU && pressed(BTN_B)) {
    enterSleepScreen();
    return;
  } else if (scene == MSG && msgCompose && pressed(BTN_B)) {
    msgCompose = false;
  } else if (scene == SETTINGS && ntfyEditMode && pressed(BTN_B)) {
    ntfyEditMode = false;
    settingsMsg = "Topic unchanged";
  } else if (scene != MENU && scene != SHOOTER && pressed(BTN_B)) {
    scene = backTarget(scene);
    return;
  }

  switch (scene) {
    case MENU:
      menuUpdate();
      if (scene == MENU) menuDraw();
      break;
    case CATCH:       ctUpdate();        ctDraw();        break;
    case MAZE:        mazeUpdate();      mazeDraw();      break;
    case FOOTBALL:    fbUpdate();        fbDraw();        break;
    case PJ:          pjUpdate();        pjDraw();        break;
    case RUNNER:      runUpdate();       runDraw();       break;
    case SNAKE:       snUpdate();        snDraw();        break;
    case WIKI_SEARCH:
      wikiSearchUpdate();
      if (scene == WIKI_SEARCH) wikiSearchDraw();
      break;
    case WIKI_READ:   wikiReadUpdate();  wikiReadDraw();  break;
    case DICT_SEARCH:
      dictSearchUpdate();
      if (scene == DICT_SEARCH) dictSearchDraw();
      break;
    case DICT_READ:   dictReadUpdate();  dictReadDraw();  break;
    case WIFI_LIST:   wifiListUpdate();  wifiListDraw();  break;
    case WIFI_PASS:
      wifiPassUpdate();
      if (scene == WIFI_PASS) wifiPassDraw();
      break;
    case FILES:       fileListUpdate();  fileListDraw();  break;
    case READER:      readerUpdate();    readerDraw();    break;
    case PHOTOS:      photosUpdate();    photosDraw();    break;
    case NOTES:       notesUpdate();     notesDraw();     break;
    case STOPWATCH:   swUpdate();        swDraw();        break;
    case CALC:        calcUpdate();      calcDraw();      break;
    case DICE:        diceUpdate();      diceDraw();      break;
    case SETTINGS:    settingsUpdate();  settingsDraw();  break;
    case MSG:         msgUpdate();       msgDraw();       break;
    case TTT:         tttUpdate();       tttDraw();       break;
    case SIMON:       simUpdate();       simDraw();       break;
    case SHOOTER:     shUpdate();         shDraw();         break;
    case SYSTEM_INFO: systemInfoDraw(); break;
    case MAP_SEARCH: mapSearchUpdate(); if (scene == MAP_SEARCH) mapSearchDraw(); break;
    case MAP_RESULT: mapResultUpdate(); if (scene == MAP_RESULT) mapResultDraw(); break;
    case WEATHER: weatherUpdate(); weatherDraw(); break;
    case NEON_RALLY: nrUpdate(); nrDraw(); break;
    case ASTEROIDS: asUpdate(); asDraw(); break;
    default: break;
  }
  present();
}