#pragma once
#include <Arduino.h>

// Alles, was über die Weboberfläche einstellbar ist. Liegt im NVS-Flash.
struct Settings {
  // WLAN-Hotspot
  String apSsid;
  String apPass;
  uint8_t apMaxClients;
  uint8_t apChannel;

  // Mobilfunk
  String simPin;
  String apn;
  String apnUser;
  String apnPass;

  // Zugang zur Weboberfläche (Benutzer "admin"). Leer = noch nicht eingerichtet.
  String adminPass;

  // Positionsmeldungen
  String reportUrl;       // leer = keine Meldungen
  String deviceName;
  uint8_t reportFormat;   // REPORT_GPSLOGGER oder REPORT_JSON
  uint16_t reportMovingS;
  uint16_t reportParkedS;

  // Energiesparen auf Akku. Minuten, 0 = Regel aus.
  uint16_t hotspotMaxBatteryMin;  // Hotspot höchstens so lange auf Akku
  uint16_t hotspotIdleMin;        // Hotspot aus, wenn so lange kein Gerät verbunden
  String triggerMac;              // Hotspot aus, wenn dieses Gerät geht ...
  uint16_t triggerGraceMin;       // ... und so lange wegbleibt
  uint16_t trackerMovingS;        // Sparmodus: Meldeintervall in Fahrt (Sekunden)
  uint16_t trackerParkedMin;      // Sparmodus: Meldeintervall im Stand
  uint16_t sleepAfterParkedMin;   // Sparmodus -> Tiefschlaf nach so langem Stillstand
  uint16_t sleepReportMin;        // Tiefschlaf: alle so viele Minuten Position melden
  uint16_t sleepCheckMin;         // Tiefschlaf: so oft auf Netzstrom prüfen
  uint8_t lowBatteryPercent;      // darunter sofort Tiefschlaf
};

enum { REPORT_GPSLOGGER = 0, REPORT_JSON = 1 };

extern Settings settings;

void settingsLoad();
void settingsSave();
void settingsFactoryReset();
// Erst wenn ein Admin-Passwort gesetzt ist, ist das Gerät eingerichtet.
bool settingsConfigured();
// "aa:bb:cc:dd:ee:ff" in Kleinbuchstaben, sonst leer.
String normalizeMac(const String &mac);
