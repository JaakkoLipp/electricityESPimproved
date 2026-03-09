#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "secrets.h"

struct PriceData {
    double priceArray[96];
    double topPrice;
    int priceCount;
    bool isValid;
};

struct tm timeinfo;

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define LED_BUILTIN 2  // Define the built-in LED pin for ESP32

// Function prototypes
void drawDisplay(PriceData data, int hour, int minute);
bool settime();
PriceData fetchData();
bool sendDiscordWebhook(const String& message);
void checkAndSendPriceAlert(const PriceData& data, int hour, int minute);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const char* priceApiUrl = "https://api.spot-hinta.fi/Today";

// discord ping
const bool pingUserOnAlert = true;

const double transferprice = 5.0; // Transfer and/or additional costs in eurocents
const int offset = 3;             // Offset in pixels for price chart
const double alertThreshold = 20.0; // Alert threshold in c/kWh (same value shown on display)

WebServer server(80);
Preferences prefs;

int date, minute, sleepstart, sleepend;
int64_t sleepTimeMinutes;
bool sleeps;
int lastAlertSlot = -1;
PriceData pricedata;

// HTML
String makePage() {
  String checked = sleeps ? "checked" : "";
  String page = R"rawliteral(
<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
  body { font-family: system-ui; max-width: 420px; margin: 40px auto; padding: 10px; text-align:center; }
  input[type=number], button { width:100%; font-size:1.1rem; margin:.4rem 0; padding:.4rem; }
  .toggle-btn {
    display:inline-block; padding:0.7rem 1.2rem; border:2px solid #888; border-radius:0.6rem;
    cursor:pointer; user-select:none; transition:all .2s;
    background:#f4f4f4; color:#333;
  }
  input[type=checkbox] { display:none; } /* hide the checkbox */
  input[type=checkbox]:checked + .toggle-btn {
    background:#4caf50; border-color:#4caf50; color:white;
  }
</style></head><body>
<h2>ESP32 Sleep Settings</h2>
<form method='POST' action='/save'>
  <label>Sleep Start (0-23):</label><br>
  <input type='number' name='sleep_start' min='0' max='23' value=')rawliteral" + String(sleepstart) + R"rawliteral('><br>
  <label>Sleep End (0-23):</label><br>
  <input type='number' name='sleep_end' min='0' max='23' value=')rawliteral" + String(sleepend) + R"rawliteral('><br>

  <!-- Hidden checkbox + styled label -->
  <input type='checkbox' id='sleep_enabled' name='sleep_enabled' )rawliteral" + checked + R"rawliteral(>
  <label for='sleep_enabled' class='toggle-btn'>Enable Sleep</label><br><br>

  <button type='submit'>Save</button>
</form>
<p style='opacity:.7;font-size:.9rem'>
  Current: )rawliteral" + String(sleepstart) + " - " + String(sleepend) +
  " , Sleep currently " + (sleeps ? "ON" : "OFF") + R"rawliteral(
</p>
</body></html>
)rawliteral";
  return page;
}

// Handlers
void handleRoot() {
  server.send(200, "text/html", makePage());
}

void handleSave() {
  if (server.hasArg("sleep_start")) sleepstart = server.arg("sleep_start").toInt();
  if (server.hasArg("sleep_end"))   sleepend   = server.arg("sleep_end").toInt();
  sleeps = server.hasArg("sleep_enabled"); // checkbox exists if checked

  // Save to flash memory
  prefs.begin("sleepcfg", false);
  prefs.putInt("start", sleepstart);
  prefs.putInt("end", sleepend);
  prefs.putBool("enabled", sleeps);
  prefs.end();

  // Tell browser to go back to "/"
  server.sendHeader("Location", "/");
  server.send(303); // 303 = "See Other" redirect after POST
}

void setup() {
  Serial.begin(9600);  // Serial init
  Serial.println("\nStarting up...");
  Wire.begin(21, 22); // GPIO 21 is SDA, GPIO 22 is SCL
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Initialize the display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // If the initialization fails the program enters the forever loop
  }

  display.setTextColor(SSD1306_WHITE);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 5);
  display.println("SPOT Price Meter v4.2");
  display.setCursor(0, 15);
  display.println("Connecting to WiFi...");
  display.setCursor(0, 25);
  display.print("SSID: ");
  display.println(ssid);
  display.display();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi..");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  digitalWrite(LED_BUILTIN, LOW);

  display.setCursor(0, 35);
  display.println("WiFi connected");
  display.setCursor(0, 45);
  display.println("Fetching data...");
  display.display();

  // Load saved preferences
  prefs.begin("sleepcfg", false);
  sleepstart = prefs.getInt("start", 0);
  sleepend   = prefs.getInt("end", 0);
  sleeps     = prefs.getBool("enabled", false);
  prefs.end();

  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/favicon.ico", HTTP_GET, [](){
    server.send(204); // No Content
  });
  server.begin();
  Serial.printf("Web server started at: ");
  Serial.println(WiFi.localIP());

  display.setCursor(0, 55);
  display.print("Time ");
  display.display();

  while(settime()) {
    Serial.println("Time data fetch failed, retry in 20s");
    delay(20000);
  }
  display.print("OK, Price ");
  display.display();

  pricedata = fetchData();
  while(!pricedata.isValid) {
    Serial.println("Price data fetch failed, retry in 20s");
    delay(20000);
    pricedata = fetchData();
  }

  date = timeinfo.tm_mday;
  minute = timeinfo.tm_min;

  display.print("OK ");
  display.display();
  delay(1500);

  drawDisplay(pricedata, timeinfo.tm_hour, timeinfo.tm_min);
}

void loop() {
  if (getLocalTime(&timeinfo)) {
    server.handleClient();

    if(timeinfo.tm_mday != date) { // Fetch data again once a day and update screen
      Serial.println("Date change, fetching new data ");
      while(settime()) {
        Serial.println("Time data fetch failed, retry in 10s ");
        delay(10000);
      }
      pricedata = fetchData();
      while(pricedata.isValid != true) {
        Serial.println("Failed to fetch valid price data, retrying in 10s... ");
        delay(10000);
        pricedata = fetchData();
      }
      date = timeinfo.tm_mday;
      lastAlertSlot = -1;
      delay((60 - timeinfo.tm_sec) * 1000); // Clock sync
    }

    if(timeinfo.tm_min != minute) { // Update screen once a minute. Delay is synced
      minute = timeinfo.tm_min;

      checkAndSendPriceAlert(pricedata, timeinfo.tm_hour, timeinfo.tm_min);

      // Determine if we should sleep based on sleep window
      bool shouldSleep = false;
      if(sleeps && sleepstart != sleepend) {
        if(sleepstart > sleepend) {
          // Sleep spans midnight (e.g., 23:00 to 07:00)
          shouldSleep = (timeinfo.tm_hour >= sleepstart || timeinfo.tm_hour < sleepend);
        } else {
          // Sleep within same day (e.g., 01:00 to 06:00)
          shouldSleep = (timeinfo.tm_hour >= sleepstart && timeinfo.tm_hour < sleepend);
        }
      }

      if(shouldSleep) {
        // Calculate remaining sleep time until sleepend
        if(sleepstart > sleepend && timeinfo.tm_hour >= sleepstart) {
          // Currently after midnight crossing start, sleep until tomorrow's end
          sleepTimeMinutes = ((24 - timeinfo.tm_hour) + sleepend) * 60;
          sleepTimeMinutes -= timeinfo.tm_min;
        } else {
          // Sleep until today's end time
          sleepTimeMinutes = (sleepend - timeinfo.tm_hour) * 60;
          sleepTimeMinutes -= timeinfo.tm_min;
        }
        // Make sure sleep lasts even with slight clock drift
        sleepTimeMinutes += 3;

        display.clearDisplay();
        display.display();
        Serial.printf("Entering sleep for %lld minutes\n", sleepTimeMinutes);
        delay(1000);
        esp_sleep_enable_timer_wakeup(sleepTimeMinutes * 60 * 1000000LL); // Convert minutes to microseconds
        esp_light_sleep_start();
        Serial.println("Exiting sleep");

        // Reconnect WiFi after light sleep
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("Reconnecting WiFi...");
          WiFi.reconnect();
          int wifiRetry = 0;
          while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
            delay(500);
            wifiRetry++;
          }
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi reconnect failed, restarting...");
            ESP.restart();
          }
          Serial.println("WiFi reconnected");
        }
      } else {
        drawDisplay(pricedata, timeinfo.tm_hour, timeinfo.tm_min);
      }
    }

    delay(3000); // Loop every 3 seconds
  }
}

void drawDisplay(PriceData data, int hour, int minute) {
  int barheight;
  int phase = 0x1A; // Default to -> symbol
  int curindex = 0;
  double curprice = 0.0;
  double nextprice = 0.0;
  double difference = 0.0;

  String printprice;
  // Prevent division by zero
  double heighmlp = data.topPrice > 0 ? 32.0 / data.topPrice : 1.0;

  int i = 0;
  int w = 0; // Chart width tracker
  double curhourtopprice = 0.0;

  display.clearDisplay();
  display.setTextSize(1);

  // Price chart draw
  for (float price : data.priceArray) {
    i++;
    if(i % 4 != 0) {
      // Log 4 prices at a time
      if(price > curhourtopprice) {
        curhourtopprice = price;
      }
    } else {
      // Log current price as well
      if(price > curhourtopprice) {
        curhourtopprice = price;
      }

      barheight = (int)(curhourtopprice * heighmlp);

      // Cull tall bars hours 0-5 to not infer with phase data
      if(i <= 20 && barheight > 26) {
        barheight = 25;
        display.setCursor(w * 5 + offset, 34);
        display.write(0xEF);
      }

      if(w == hour) { // CURRENT HOUR: Draw empty bar with top price and fill with current 15 min price
        curindex = i - 4 + (int)(minute / 15);

        display.drawRect(w * 5 + offset, SCREEN_HEIGHT - barheight, 5, barheight, SSD1306_WHITE);

        // Draw pointer on separator row to help identify current hour
        display.setCursor(w * 5 + offset, 24);
        display.write(0x1F);

        // Calculate fill bar height based on current minute accurate price
        curprice = data.priceArray[curindex];
        barheight = (int)(curprice * heighmlp);
        display.fillRect(w * 5 + offset, SCREEN_HEIGHT - barheight, 5, barheight, SSD1306_WHITE);

        // Phase symbol for graph
        if(curindex + 1 < data.priceCount) {
          nextprice = data.priceArray[curindex + 1];
          // Calculate difference amount
          if(curprice > nextprice) {
            difference = curprice - nextprice;
          } else {
            difference = nextprice - curprice;
          }
          // Calculate difference symbol
          if(difference > 10.0) {
            phase = 0x13; // "!" symbol
          } else if(curprice > nextprice) {
            phase = 0x19; // Down symbol
          } else {
            phase = 0x18; // Up symbol
          }

        } else { // Valid for last quarter of day
          difference = 0.0;
          phase = 0x1A; // -> symbol
        }

      } else { // OTHER HOUR: Draw empty bar based on top price for hour
        display.drawRect(w * 5 + offset, SCREEN_HEIGHT - barheight, 5, barheight, SSD1306_WHITE);
      }
      w += 1;
      curhourtopprice = 0;
    }
  }

  // Current price
  if(curprice > 999.9) {
    curprice = 999.9;
  }
  printprice = curprice < 0 ? String(curprice, 0) : String(curprice, 1);

  // Draw price change and phase data
  display.setTextSize(1);
  display.setCursor(0, 27);
  display.print(difference >= 10.0 ? String(difference, 0) : String(difference, 1));
  display.setCursor(display.getCursorX() + 1, display.getCursorY());
  display.write(phase);

  // Draw price data
  display.setTextSize(3);
  display.setCursor((5 - printprice.length()) * 18 + 6, 2);
  display.print(printprice);

  int curposition = display.getCursorX() + 1;
  display.setTextSize(1);
  display.setCursor(curposition, 2);
  display.print("c/kWh");
  display.setCursor(curposition, 15);
  display.printf("%02d:%02d", hour, minute);

  // Separator line
  display.setCursor(0, 19);
  display.setTextSize(1);
  display.print("_____________________");

  display.display();
}

bool settime() {
  Serial.println("Configuring NTP...");

  // Configure NTP with Finnish timezone (EET/EEST with automatic DST)
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");

  // Wait for time to sync (max 10 seconds)
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(1000);
    retry++;
    Serial.print(".");
  }

  if (retry >= 10) {
    Serial.println("\nNTP sync failed");
    return 1;
  }

  Serial.printf("\nRTC set to: %02d:%02d:%02d %02d/%02d/%d\n",
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec,
      timeinfo.tm_mday,
      timeinfo.tm_mon + 1,
      timeinfo.tm_year + 1900);

  return 0;
}

PriceData fetchData() {
  Serial.println("Price API fetch");
  PriceData data = {
    .priceArray = {0.0},
    .topPrice = 0.0,
    .priceCount = 0,
    .isValid = false
  };

  HTTPClient http;
  http.begin(priceApiUrl);
  int httpResponseCode = http.GET();

  if (httpResponseCode > 0) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonArray prices = doc.as<JsonArray>();
      int priceCount = 0;

      // Process each price entry
      for (JsonObject price : prices) {
        if (priceCount >= 96) break; // Prevent array overflow

        // Store price with tax in cents/kWh + transfer constant
        data.priceArray[priceCount] = (price["PriceWithTax"].as<double>() * 100.0) + transferprice;
        if (data.priceArray[priceCount] > data.topPrice) {
          data.topPrice = data.priceArray[priceCount];
        }
        priceCount++;
      }

      // Mark data as valid when data is parsed
      data.priceCount = priceCount;
      data.isValid = true;
      Serial.println("Price data fetched successfully");

    } else {
      Serial.println("JSON parse failed");
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpResponseCode);
  }

  http.end();
  return data;
}

bool sendDiscordWebhook(const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Webhook skipped: WiFi not connected");
    return false;
  }

  HTTPClient http;
  http.begin(discordWebhookUrl);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["content"] = message;

  // Only allow the specific user mention if pinging is enabled
  JsonArray parse = doc["allowed_mentions"]["parse"].to<JsonArray>();
  if (pingUserOnAlert) {
    JsonArray users = doc["allowed_mentions"]["users"].to<JsonArray>();
    users.add(discordUserIdToPing);
  }

  String payload;
  serializeJson(doc, payload);

  int httpResponseCode = http.POST(payload);
  Serial.printf("Webhook response: %d\n", httpResponseCode);

  http.end();
  return httpResponseCode >= 200 && httpResponseCode < 300;
}

void checkAndSendPriceAlert(const PriceData& data, int hour, int minute) {
  if (!data.isValid || data.priceCount <= 0) {
    Serial.println("Alert check skipped: invalid price data");
    return;
  }

  int curindex = hour * 4 + (minute / 15);
  if (curindex < 0 || curindex >= data.priceCount) {
    Serial.println("Alert check skipped: current price index out of range");
    return;
  }

  double curprice = data.priceArray[curindex];

  if (curprice <= alertThreshold) {
    return;
  }

  // One alert per 15-minute slot
  if (curindex == lastAlertSlot) {
    return;
  }

  String msg = "";
  if (pingUserOnAlert) {
    msg += "<@" + String(discordUserIdToPing) + "> ";
  }
  msg += "⚠️ Electricity price alert\n";
  msg += "Current price: " + String(curprice, 1) + " c/kWh\n";
  msg += "Threshold: " + String(alertThreshold, 1) + " c/kWh\n";
  msg += "Time: ";
  msg += String(hour < 10 ? "0" : "") + String(hour);
  msg += ":";
  msg += String(minute < 10 ? "0" : "") + String(minute);

  if (sendDiscordWebhook(msg)) {
    lastAlertSlot = curindex;
    Serial.println("Discord alert sent successfully");
  } else {
    Serial.println("Discord alert failed to send");
  }
}
