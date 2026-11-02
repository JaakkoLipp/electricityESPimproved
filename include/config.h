#pragma once
#include <stdint.h>

#define FW_VERSION "3.0.0"

// Override for testing failure handling, e.g. -DPRICE_API_URL=\"https://api.spot-hinta.fi/Nope\"
#ifndef PRICE_API_URL
#define PRICE_API_URL "https://api.spot-hinta.fi/TodayAndDayForward"
#endif

constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.google.com";

// Hardware
constexpr int PIN_SDA = 21;
constexpr int PIN_SCL = 22;
constexpr int PIN_LED = 2;
constexpr int PIN_BUTTON = 0;  // BOOT button, active low
constexpr uint8_t OLED_ADDR = 0x3C;

// Price fetching
constexpr uint32_t HTTP_TIMEOUT_MS = 8000;
constexpr int TOMORROW_POLL_FROM_MIN = 14 * 60 + 15;  // look for tomorrow's prices from 14:15
constexpr uint32_t TOMORROW_POLL_EVERY_S = 30 * 60;
constexpr uint32_t BACKOFF_S[] = {60, 120, 300, 600, 900};  // retry delays after failures; the last repeats
constexpr int BACKOFF_STEPS = sizeof(BACKOFF_S) / sizeof(BACKOFF_S[0]);

// WiFi and setup hotspot
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t SETUP_IDLE_RESTART_MS = 5UL * 60 * 1000;   // back to saved WiFi if nobody uses the hotspot

// Button and screen timings
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t SHORT_PRESS_MAX_MS = 1000;
constexpr uint32_t LONG_PRESS_MS = 5000;
constexpr uint32_t BOOT_INFO_MS = 5000;     // info screen after connecting
constexpr uint32_t INFO_SCREEN_MS = 10000;  // info screen after a short press
constexpr uint32_t NIGHT_WAKE_MS = 30000;   // short press at night shows prices this long

