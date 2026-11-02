#include "settings.h"
#include <Preferences.h>

bool loadWifiCreds(WifiCreds& out) {
  Preferences p;
  p.begin("wifi", true);
  out.ssid = p.getString("ssid", "");
  out.pass = p.getString("pass", "");
  p.end();
  return out.ssid.length() > 0;
}

void saveWifiCreds(const WifiCreds& c) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", c.ssid);
  p.putString("pass", c.pass);
  p.end();
}

void clearWifiCreds() {
  Preferences p;
  p.begin("wifi", false);
  p.clear();
  p.end();
}

void clampSettings(Settings& s) {
  if (isnan(s.extraCents)) s.extraCents = 0.0f;
  s.extraCents = constrain(s.extraCents, EXTRA_CENTS_MIN, EXTRA_CENTS_MAX);
  if (s.nightStartMin >= 24 * 60) s.nightStartMin = 22 * 60;
  if (s.nightEndMin >= 24 * 60) s.nightEndMin = 7 * 60;
  if (s.brightness > 2) s.brightness = 2;
}

void loadSettings(Settings& s) {
  Preferences p;
  p.begin("settings", true);
  s.extraCents = p.getFloat("extra", s.extraCents);
  s.nightEnabled = p.getBool("nightOn", s.nightEnabled);
  s.nightStartMin = p.getUShort("nightStart", s.nightStartMin);
  s.nightEndMin = p.getUShort("nightEnd", s.nightEndMin);
  s.brightness = p.getUChar("bright", s.brightness);
  p.end();
  clampSettings(s);
}

void saveSettings(const Settings& s) {
  Preferences p;
  p.begin("settings", false);
  p.putFloat("extra", s.extraCents);
  p.putBool("nightOn", s.nightEnabled);
  p.putUShort("nightStart", s.nightStartMin);
  p.putUShort("nightEnd", s.nightEndMin);
  p.putUChar("bright", s.brightness);
  p.end();
}
