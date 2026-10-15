#include "Settings.h"

#include <Preferences.h>
#include <esp_mac.h>

Settings settings;

static Preferences prefs;
static const char *NS = "router";

static String defaultSsid() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char buf[24];
  snprintf(buf, sizeof(buf), "LTE-Router-%02X%02X", mac[4], mac[5]);
  return buf;
}

void settingsLoad() {
  prefs.begin(NS, true);
  Settings &s = settings;
  s.apSsid = prefs.getString("apSsid", defaultSsid());
  s.apPass = prefs.getString("apPass", "lte-router");
  s.apMaxClients = prefs.getUChar("apMax", 2);
  s.apChannel = prefs.getUChar("apChan", 6);

  s.simPin = prefs.getString("simPin", "");
  s.apn = prefs.getString("apn", "internet");
  s.apnUser = prefs.getString("apnUser", "");
  s.apnPass = prefs.getString("apnPass", "");

  s.adminPass = prefs.getString("admin", "");

  s.reportUrl = prefs.getString("repUrl", "");
  s.deviceName = prefs.getString("devName", "lte_router");
  s.reportFormat = prefs.getUChar("repFmt", REPORT_GPSLOGGER);
  s.reportMovingS = prefs.getUShort("repMov", 30);
  s.reportParkedS = prefs.getUShort("repPark", 300);

  s.hotspotMaxBatteryMin = prefs.getUShort("hsMaxBat", 45);
  s.hotspotIdleMin = prefs.getUShort("hsIdle", 10);
  s.triggerMac = prefs.getString("trigMac", "");
  s.triggerGraceMin = prefs.getUShort("trigGrace", 2);
  s.trackerMovingS = prefs.getUShort("trkMov", 60);
  s.trackerParkedMin = prefs.getUShort("trkPark", 15);
  s.sleepAfterParkedMin = prefs.getUShort("slpAfter", 60);
  s.sleepReportMin = prefs.getUShort("slpRep", 120);
  s.lowBatteryPercent = prefs.getUChar("lowBat", 15);
  prefs.end();
}

void settingsSave() {
  prefs.begin(NS, false);
  const Settings &s = settings;
  prefs.putString("apSsid", s.apSsid);
  prefs.putString("apPass", s.apPass);
  prefs.putUChar("apMax", s.apMaxClients);
  prefs.putUChar("apChan", s.apChannel);
  prefs.putString("simPin", s.simPin);
  prefs.putString("apn", s.apn);
  prefs.putString("apnUser", s.apnUser);
  prefs.putString("apnPass", s.apnPass);
  prefs.putString("admin", s.adminPass);
  prefs.putString("repUrl", s.reportUrl);
  prefs.putString("devName", s.deviceName);
  prefs.putUChar("repFmt", s.reportFormat);
  prefs.putUShort("repMov", s.reportMovingS);
  prefs.putUShort("repPark", s.reportParkedS);
  prefs.putUShort("hsMaxBat", s.hotspotMaxBatteryMin);
  prefs.putUShort("hsIdle", s.hotspotIdleMin);
  prefs.putString("trigMac", s.triggerMac);
  prefs.putUShort("trigGrace", s.triggerGraceMin);
  prefs.putUShort("trkMov", s.trackerMovingS);
  prefs.putUShort("trkPark", s.trackerParkedMin);
  prefs.putUShort("slpAfter", s.sleepAfterParkedMin);
  prefs.putUShort("slpRep", s.sleepReportMin);
  prefs.putUChar("lowBat", s.lowBatteryPercent);
  prefs.end();
}

void settingsFactoryReset() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
}

bool settingsConfigured() { return !settings.adminPass.isEmpty(); }

String normalizeMac(const String &mac) {
  String hex;
  for (char c : mac) {
    if (isxdigit((unsigned char)c)) hex += (char)tolower(c);
  }
  if (hex.length() != 12) return String();
  String out;
  for (int i = 0; i < 12; i += 2) {
    if (i) out += ':';
    out += hex.substring(i, i + 2);
  }
  return out;
}
