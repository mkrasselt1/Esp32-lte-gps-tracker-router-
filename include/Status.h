#pragma once
#include <Arduino.h>

#include "Gnss.h"
#include "Power.h"

// Betriebsstufen, siehe README "Energiesparen".
enum class Mode : uint8_t {
  Normal,      // am Netz: Hotspot und Tracker
  Battery,     // auf Akku, Hotspot noch an
  Tracker,     // auf Akku, Hotspot aus, nur noch Meldungen
  WakeReport,  // kurz aus dem Tiefschlaf geweckt, um zu melden
};

// Laufzeitzustand, den Modem-Task, Hauptschleife und Weboberfläche teilen.
// Die Modem-Task schreibt fix/hasFix/rssi/modemMv/modemState, der Rest liest.
struct Status {
  GnssFix fix;              // letzte gültige Position
  bool hasFix = false;      // die letzte Abfrage hatte eine Position
  uint32_t fixAt = 0;       // millis() der letzten gültigen Position
  PowerState power;
  int modemMv = 0;
  int rssi = 99;            // AT+CSQ, 99 = unbekannt
  const char *modemState = "startet";
  char operatorName[32] = "";  // Netzbetreiber beim Einbuchen, z. B. "FONIC+"
  bool online = false;      // PPP hat eine IP

  Mode mode = Mode::Normal;
  const char *modeReason = "";
  uint32_t modeSince = 0;

  int lastHttpCode = 0;
  uint32_t lastReportOkAt = 0;
};

extern Status status;

const char *modeName(Mode m);
