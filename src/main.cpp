#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include "config.h"
#include "settings.h"

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

constexpr int MAX_SLOTS = 200;  // today + tomorrow in 15 min slots, with room for a 25 h day
constexpr time_t SLOT_S = 15 * 60;
constexpr const char* TZ_INFO = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Finland, automatic DST
constexpr const char* HOSTNAME = "spotprice";
constexpr const char* SETUP_AP_NAME = "SpotPrice-Setup";
constexpr uint8_t CONTRAST[] = {0x01, 0x40, 0xFF};  // brightness low, medium, high

// Contiguous 15 min price slots. Indexing by UTC epoch keeps DST days (23 h / 25 h) right.
struct PriceData {
  float cents[MAX_SLOTS];  // spot price incl. VAT, c/kWh, without settings.extraCents
  time_t start = 0;        // UTC epoch of slot 0
  int count = 0;
};

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);
DNSServer dns;
Settings settings;
PriceData prices;

bool setupMode = false;
String fetchStatus = "Not fetched yet";
uint32_t nextFetchMs = 0;
int backoffStep = 0;
uint32_t lastActivityMs = 0;  // setup mode: last web request
uint32_t infoUntilMs = 0;     // info screen shown until
uint32_t wakeUntilMs = 0;     // screen forced on during night mode until
int lastDrawKey = -1;         // what is on screen now; -1 forces a redraw
bool screenOn = true;

// ---------- time helpers ----------

bool timeValid() { return time(nullptr) > 1700000000; }

static bool timeReached(uint32_t deadlineMs) { return (int32_t)(millis() - deadlineMs) >= 0; }

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// "2026-11-02T00:15:00+02:00" -> UTC epoch, 0 on failure
static time_t parseIsoTime(const char* s) {
  int y, mo, d, h, mi, sec, oh = 0, om = 0;
  char sign = 'Z';
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &sec, &sign, &oh, &om) < 6) return 0;
  int64_t t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  if (sign == '+') t -= oh * 3600 + om * 60;
  else if (sign == '-') t += oh * 3600 + om * 60;
  return (time_t)t;
}

// Local midnight, dayOffset days from the day of t
static time_t localMidnight(time_t t, int dayOffset) {
  struct tm tm;
  localtime_r(&t, &tm);
  tm.tm_mday += dayOffset;
  tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

static bool coversDay(time_t now, int dayOffset) {
  time_t from = localMidnight(now, dayOffset), to = localMidnight(now, dayOffset + 1);
  return prices.count > 0 && prices.start <= from && prices.start + prices.count * SLOT_S >= to;
}

static int slotAt(time_t t) {
  if (prices.count == 0 || t < prices.start) return -1;
  long i = (t - prices.start) / SLOT_S;
  return i < prices.count ? (int)i : -1;
}

static bool inNightWindow(const struct tm& lt) {
  if (!settings.nightEnabled || settings.nightStartMin == settings.nightEndMin) return false;
  int m = lt.tm_hour * 60 + lt.tm_min;
  int s = settings.nightStartMin, e = settings.nightEndMin;
  return s < e ? (m >= s && m < e) : (m >= s || m < e);  // second case spans midnight
}

static String hhmm(int minutes) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

static int parseHhmm(const String& s, int fallback) {
  int h, m;
  if (sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return h * 60 + m;
}

// ---------- price fetching ----------

bool fetchPrices() {
  Serial.println("Price API fetch");
  WiFiClientSecure client;
  client.setInsecure();  // public, non-secret data; avoids bundling a CA certificate
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);  // no chunked encoding, so the body can be streamed into ArduinoJson
  if (!http.begin(client, PRICE_API_URL)) {
    fetchStatus = "HTTP begin failed";
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    fetchStatus = code > 0 ? "HTTP " + String(code) : "HTTP error " + String(code);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter[0]["DateTime"] = true;
  filter[0]["PriceWithTax"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    fetchStatus = String("JSON ") + err.c_str();
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) {
    fetchStatus = "Empty response";
    return false;
  }
  time_t start = parseIsoTime(arr[0]["DateTime"]);
  if (start == 0) {
    fetchStatus = "Bad DateTime";
    return false;
  }

  // Parse into a temporary so a bad response never overwrites good data
  static PriceData fresh;
  fresh.start = start;
  fresh.count = 0;
  for (JsonObject e : arr) {
    if (fresh.count >= MAX_SLOTS) break;
    if (parseIsoTime(e["DateTime"]) != start + fresh.count * SLOT_S) break;  // keep the contiguous part
    fresh.cents[fresh.count++] = e["PriceWithTax"].as<float>() * 100.0f;
  }
  prices = fresh;

  struct tm lt;
  time_t now = time(nullptr);
  localtime_r(&now, &lt);
  fetchStatus = "OK, " + String(prices.count) + " slots at " + hhmm(lt.tm_hour * 60 + lt.tm_min);
  Serial.println(fetchStatus);
  return true;
}

// Fetch when today is missing, and poll for tomorrow's prices in the afternoon. Never blocks for long.
void updatePrices(time_t now) {
  if (WiFi.status() != WL_CONNECTED || !timeReached(nextFetchMs)) return;

  struct tm lt;
  localtime_r(&now, &lt);
  bool afternoon = lt.tm_hour * 60 + lt.tm_min >= TOMORROW_POLL_FROM_MIN;
  if (coversDay(now, 0) && (!afternoon || coversDay(now, 1))) {
    nextFetchMs = millis() + 60 * 1000;
    return;
  }

  if (fetchPrices()) {
    backoffStep = 0;
    nextFetchMs = millis() + TOMORROW_POLL_EVERY_S * 1000;  // only matters while tomorrow is still missing
  } else {
    uint32_t wait = BACKOFF_S[min(backoffStep, BACKOFF_STEPS - 1)];
    backoffStep++;
    nextFetchMs = millis() + wait * 1000;
    fetchStatus += ", retry in " + String(wait) + " s";
    Serial.println(fetchStatus);
  }
  lastDrawKey = -1;
}

// ---------- screen ----------

void applyBrightness() {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(CONTRAST[settings.brightness]);
}

void drawLines(const String lines[], int n) {
  display.clearDisplay();
  display.setTextSize(1);
  for (int i = 0; i < n; i++) {
    display.setCursor(0, i * 10 + 2);
    display.print(lines[i]);
  }
  display.display();
}

void drawInfo() {
  String lines[] = {
    "Spot Price v" FW_VERSION,
    "IP " + WiFi.localIP().toString(),
    String(HOSTNAME) + ".local",
    fetchStatus.substring(0, 21),
    fetchStatus.substring(21, 42),
    "Hold BOOT 5s: WiFi",
  };
  drawLines(lines, 6);
}

void drawNoData(const struct tm& lt) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(34, 2);
  display.printf("%02d:%02d", lt.tm_hour, lt.tm_min);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print("No price data");
  display.setCursor(0, 36);
  display.print(fetchStatus.substring(0, 42));  // wraps to two lines
  display.setCursor(0, 56);
  display.print(WiFi.localIP().toString());
  display.display();
}

void drawPrices(time_t now) {
  struct tm lt;
  localtime_r(&now, &lt);
  int cur = slotAt(now);
  if (cur < 0 || !coversDay(now, 0)) {
    drawNoData(lt);
    return;
  }

  const float extra = settings.extraCents;
  const int first = (localMidnight(now, 0) - prices.start) / SLOT_S;
  const int hours = (localMidnight(now, 1) - localMidnight(now, 0)) / SLOT_S / 4;  // 23, 24 or 25
  const int offset = max(0, (SCREEN_WIDTH - hours * 5) / 2);                       // chart x offset
  const int curBar = (cur - first) / 4;

  float top = 0.0f;
  for (int i = first; i < first + hours * 4; i++) top = max(top, prices.cents[i] + extra);
  const float heightMlp = top > 0 ? 32.0f / top : 1.0f;

  display.clearDisplay();
  display.setTextSize(1);

  // Price chart: one bar per hour, height from the hour's highest 15 min price
  for (int b = 0; b < hours; b++) {
    float hourTop = 0.0f;
    for (int k = 0; k < 4; k++) hourTop = max(hourTop, prices.cents[first + b * 4 + k] + extra);
    int barheight = max(0, (int)(hourTop * heightMlp));
    int x = b * 5 + offset;

    // Cull tall bars of the first hours so they don't overlap the phase text
    if (b < 5 && barheight > 26) {
      barheight = 25;
      display.setCursor(x, 34);
      display.write(0xEF);
    }
    display.drawRect(x, SCREEN_HEIGHT - barheight, 5, barheight, SSD1306_WHITE);

    if (b == curBar) {  // Current hour: pointer above, filled up to the current 15 min price
      display.setCursor(x, 24);
      display.write(0x1F);
      int fill = max(0, (int)((prices.cents[cur] + extra) * heightMlp));
      display.fillRect(x, SCREEN_HEIGHT - fill, 5, fill, SSD1306_WHITE);
    }
  }

  // Change to the next 15 min slot (may be tomorrow's first slot)
  float curprice = prices.cents[cur] + extra;
  float difference = 0.0f;
  int phase = 0x1A;  // -> symbol when the next price is unknown
  if (cur + 1 < prices.count) {
    float nextprice = prices.cents[cur + 1] + extra;
    difference = fabsf(curprice - nextprice);
    if (difference > 10.0f) phase = 0x13;            // "!!" symbol
    else if (curprice > nextprice) phase = 0x19;     // Down symbol
    else phase = 0x18;                               // Up symbol
  }

  display.setCursor(0, 27);
  display.print(difference >= 10.0f ? String(difference, 0) : String(difference, 1));
  display.setCursor(display.getCursorX() + 1, display.getCursorY());
  display.write(phase);

  // Current price, big
  curprice = constrain(curprice, -99.0f, 999.9f);
  String printprice = curprice < 0 ? String(curprice, 0) : String(curprice, 1);
  display.setTextSize(3);
  display.setCursor((5 - printprice.length()) * 18 + 6, 2);
  display.print(printprice);

  int curposition = display.getCursorX() + 1;
  display.setTextSize(1);
  display.setCursor(curposition, 2);
  display.print("c/kWh");
  display.setCursor(curposition, 15);
  display.printf("%02d:%02d", lt.tm_hour, lt.tm_min);

  // Separator line
  display.setCursor(0, 19);
  display.print("_____________________");

  display.display();
}

// Redraws only when the minute, the screen mode or the data changes
void updateScreen(time_t now) {
  struct tm lt;
  localtime_r(&now, &lt);
  bool info = !timeReached(infoUntilMs);
  bool on = info || !timeReached(wakeUntilMs) || !inNightWindow(lt);

  if (on != screenOn) {
    display.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
    screenOn = on;
    lastDrawKey = -1;
  }
  if (!on) return;

  int key = info ? -2 : lt.tm_hour * 60 + lt.tm_min;
  if (key == lastDrawKey) return;
  lastDrawKey = key;
  if (info) drawInfo();
  else drawPrices(now);
}

// Short press: info screen (or wake the screen at night). Hold 5 s: forget WiFi and restart into setup.
void handleButton(time_t now) {
  static bool down = false;
  static uint32_t downAt = 0;
  bool pressed = digitalRead(PIN_BUTTON) == LOW;

  if (pressed && !down) {
    down = true;
    downAt = millis();
  } else if (pressed && millis() - downAt >= LONG_PRESS_MS) {
    String lines[] = {"WiFi settings", "cleared.", "Restarting into", "setup mode..."};
    drawLines(lines, 4);
    clearWifiCreds();
    delay(1500);
    ESP.restart();
  } else if (!pressed && down) {
    down = false;
    uint32_t held = millis() - downAt;
    if (held < BUTTON_DEBOUNCE_MS || held > SHORT_PRESS_MAX_MS) return;
    struct tm lt;
    localtime_r(&now, &lt);
    if (inNightWindow(lt) && timeReached(wakeUntilMs)) wakeUntilMs = millis() + NIGHT_WAKE_MS;
    else infoUntilMs = millis() + INFO_SCREEN_MS;
    lastDrawKey = -1;
  }
}

// ---------- web pages ----------

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

static String pageStart(const char* title) {
  String p = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  p += title;
  p += F("</title><style>"
         "body{font-family:system-ui;max-width:420px;margin:24px auto;padding:0 12px;color:#222}"
         "label{display:block;margin-top:.8rem;font-weight:600}"
         "input,select,button{width:100%;box-sizing:border-box;font-size:1.05rem;margin:.3rem 0;padding:.45rem}"
         "input[type=checkbox]{width:auto;margin-right:.5rem}"
         ".row{display:flex;gap:.6rem}.row>div{flex:1}"
         ".card{background:#f4f4f4;border-radius:.6rem;padding:.6rem .9rem;margin:1rem 0}"
         ".big{font-size:2rem;font-weight:700}.muted{opacity:.7;font-size:.9rem}"
         "button{background:#2e7d32;color:#fff;border:0;border-radius:.4rem;margin-top:1rem}"
         "button.secondary{background:#888}"
         "</style></head><body>");
  return p;
}

static const char* pageEnd() { return "</body></html>"; }

static String msgPage(const char* title, const String& msg) {
  return pageStart(title) + "<h2>" + title + "</h2><p>" + msg + "</p>" + pageEnd();
}

void handleSettingsPage() {
  time_t now = time(nullptr);
  int cur = slotAt(now);
  String p = pageStart("Spot Price");
  p += "<h2>Spot Price</h2><div class='card'>";
  if (cur >= 0) {
    p += "<div class='big'>" + String(prices.cents[cur] + settings.extraCents, 2) + " c/kWh</div>";
    p += "<div class='muted'>spot " + String(prices.cents[cur], 2) + " + extra " + String(settings.extraCents, 2) + "</div>";
  } else {
    p += "<div class='big'>No price data</div>";
  }
  p += "<div class='muted'>Prices: " + htmlEscape(fetchStatus) + "<br>WiFi: " + htmlEscape(WiFi.SSID()) + " (" +
       String(WiFi.RSSI()) + " dBm), " + WiFi.localIP().toString() + "<br>Uptime " + String(millis() / 60000) +
       " min, firmware " FW_VERSION "</div></div>";

  p += F("<form method='POST' action='/save'>");
  p += "<label>Extra cost, c/kWh</label><input type='number' name='extra' step='0.01' min='" + String(EXTRA_CENTS_MIN, 0) +
       "' max='" + String(EXTRA_CENTS_MAX, 0) + "' value='" + String(settings.extraCents, 2) + "'>";
  p += F("<div class='muted'>Added to the spot price (incl. VAT): transfer, electricity tax, seller margin.</div>");

  p += F("<label>Screen brightness</label><select name='bright'>");
  const char* names[] = {"Low", "Medium", "High"};
  for (int i = 0; i < 3; i++) {
    p += "<option value='" + String(i) + "'" + (settings.brightness == i ? " selected" : "") + ">" + names[i] + "</option>";
  }
  p += F("</select>");

  p += "<label><input type='checkbox' name='night_on'" + String(settings.nightEnabled ? " checked" : "") +
       ">Turn screen off at night</label>";
  p += "<div class='row'><div><label>From</label><input type='time' name='night_start' value='" + hhmm(settings.nightStartMin) +
       "'></div><div><label>To</label><input type='time' name='night_end' value='" + hhmm(settings.nightEndMin) + "'></div></div>";
  p += F("<div class='muted'>At night a short press of BOOT shows the prices for 30 s.</div>"
         "<button type='submit'>Save</button></form>");

  p += F("<form method='POST' action='/wifi-reset' onsubmit=\"return confirm('Forget WiFi and restart into setup mode?')\">"
         "<button class='secondary' type='submit'>Change WiFi network</button></form>");
  p += pageEnd();
  server.send(200, "text/html", p);
}

void handleSave() {
  if (server.hasArg("extra")) {
    String v = server.arg("extra");
    v.replace(',', '.');
    settings.extraCents = v.toFloat();
  }
  if (server.hasArg("bright")) settings.brightness = server.arg("bright").toInt();
  settings.nightEnabled = server.hasArg("night_on");  // checkbox is only sent when checked
  settings.nightStartMin = parseHhmm(server.arg("night_start"), settings.nightStartMin);
  settings.nightEndMin = parseHhmm(server.arg("night_end"), settings.nightEndMin);
  clampSettings(settings);
  saveSettings(settings);
  applyBrightness();
  lastDrawKey = -1;
  Serial.printf("Settings saved: extra %.2f, night %d %d-%d, brightness %d\n", settings.extraCents,
                settings.nightEnabled, settings.nightStartMin, settings.nightEndMin, settings.brightness);

  server.sendHeader("Location", "/");
  server.send(303);  // redirect after POST
}

void handleWifiReset() {
  server.send(200, "text/html", msgPage("Restarting", "Join the WiFi <b>" + String(SETUP_AP_NAME) + "</b> to set up a new network."));
  clearWifiCreds();
  delay(1000);
  ESP.restart();
}

// ---------- setup hotspot ----------

String networkOptions;  // <option>s from a scan done before the hotspot starts

void handleSetupPage() {
  lastActivityMs = millis();
  String p = pageStart("Spot Price setup");
  p += F("<h2>WiFi setup</h2><form method='POST' action='/connect'>"
         "<label>Network</label><select name='ssid_sel'>");
  p += networkOptions;
  p += F("<option value=''>Other (type below)</option></select>"
         "<input name='ssid' placeholder='Network name, if not listed'>"
         "<label>Password</label><input type='password' name='pass'>"
         "<button type='submit'>Connect</button></form>");
  p += pageEnd();
  server.send(200, "text/html", p);
}

void handleConnect() {
  lastActivityMs = millis();
  WifiCreds c;
  c.ssid = server.arg("ssid");
  c.ssid.trim();
  if (c.ssid.isEmpty()) c.ssid = server.arg("ssid_sel");
  c.pass = server.arg("pass");
  if (c.ssid.isEmpty()) {
    server.send(400, "text/html", msgPage("Error", "No network selected. <a href='/'>Back</a>"));
    return;
  }
  saveWifiCreds(c);
  server.send(200, "text/html", msgPage("Saved", "Connecting to <b>" + htmlEscape(c.ssid) +
                                       "</b>. If it fails, the setup hotspot comes back."));
  delay(1000);
  ESP.restart();
}

void startSetupMode() {
  setupMode = true;
  Serial.println("Starting setup hotspot");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  networkOptions = "";
  for (int i = 0; i < n; i++) {
    String s = htmlEscape(WiFi.SSID(i));
    if (s.isEmpty() || networkOptions.indexOf("'" + s + "'") >= 0) continue;  // skip hidden and duplicates
    networkOptions += "<option value='" + s + "'>" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();

  WiFi.softAP(SETUP_AP_NAME);
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);  // captive portal: every name resolves to us

  server.on("/", HTTP_GET, handleSetupPage);
  server.on("/connect", HTTP_POST, handleConnect);
  server.onNotFound([ip]() {
    server.sendHeader("Location", "http://" + ip.toString() + "/");
    server.send(302);
  });
  server.begin();
  lastActivityMs = millis();

  String lines[] = {"WiFi setup", "Join WiFi:", SETUP_AP_NAME, "then open", "http://" + ip.toString()};
  drawLines(lines, 5);
}

// ---------- main ----------

bool connectWifi(const WifiCreds& c) {
  String lines[] = {"Spot Price v" FW_VERSION, "Connecting to WiFi", c.ssid.substring(0, 21), ""};
  drawLines(lines, 4);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > WIFI_CONNECT_TIMEOUT_MS) return false;
    delay(250);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting up...");
  Wire.begin(PIN_SDA, PIN_SCL);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_LED, HIGH);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 allocation failed"));  // keep running: the web page still works
  }
  display.setTextColor(SSD1306_WHITE);

  loadSettings(settings);
  applyBrightness();

  WifiCreds creds;
  if (!loadWifiCreds(creds) || !connectWifi(creds)) {
    Serial.println("No WiFi credentials or connection failed");
    startSetupMode();
    return;
  }
  digitalWrite(PIN_LED, LOW);
  Serial.printf("Connected, IP %s\n", WiFi.localIP().toString().c_str());

  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleSettingsPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi-reset", HTTP_POST, handleWifiReset);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  infoUntilMs = millis() + BOOT_INFO_MS;  // show the address for a moment
}

void loop() {
  server.handleClient();

  if (setupMode) {
    dns.processNextRequest();
    WifiCreds creds;
    // Nobody is using the hotspot: retry the saved network (e.g. after a router outage)
    if (millis() - lastActivityMs > SETUP_IDLE_RESTART_MS && WiFi.softAPgetStationNum() == 0 && loadWifiCreds(creds)) {
      ESP.restart();
    }
    delay(2);
    return;
  }

  time_t now = time(nullptr);
  if (!timeValid()) {
    if (timeReached(infoUntilMs) && lastDrawKey != -3) {
      String lines[] = {"Spot Price v" FW_VERSION, "Waiting for time", "(NTP)...", "", WiFi.localIP().toString()};
      drawLines(lines, 5);
      lastDrawKey = -3;
    }
    delay(10);
    return;
  }

  handleButton(now);
  updatePrices(now);
  updateScreen(now);
  delay(5);
}
