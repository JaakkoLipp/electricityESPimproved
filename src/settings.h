#pragma once
#include <Arduino.h>

struct WifiCreds {
  String ssid;
  String pass;
};

bool loadWifiCreds(WifiCreds& out);  // NVS namespace "wifi", keys "ssid", "pass"; false if none saved
void saveWifiCreds(const WifiCreds& c);
void clearWifiCreds();

struct Settings {
  float extraCents = 5.0f;          // added to the spot price (transfer, electricity tax, margin), c/kWh
  bool nightEnabled = false;
  uint16_t nightStartMin = 22 * 60;  // minutes after local midnight
  uint16_t nightEndMin = 7 * 60;
  uint8_t brightness = 2;            // 0 low, 1 medium, 2 high
};

constexpr float EXTRA_CENTS_MIN = -50.0f;
constexpr float EXTRA_CENTS_MAX = 200.0f;

void loadSettings(Settings& s);  // NVS namespace "settings": "extra", "nightOn", "nightStart", "nightEnd", "bright"
void saveSettings(const Settings& s);
void clampSettings(Settings& s);
