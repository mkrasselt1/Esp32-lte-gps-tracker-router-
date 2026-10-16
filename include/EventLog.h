#pragma once
#include <Arduino.h>

// Ereignis-Log zum Prüfen von Energiesparen und Tiefschlaf: Starts samt
// Weckgrund, Betriebsstufen, Zeit bis zur Verbindung und zum GPS-Fix,
// Akkuspannung. Liegt im NVS-Flash und übersteht Tiefschlaf und Stromausfall.
// Ausgabe über USB mit "log", löschen mit "log-clear".

// In setup(): Startzähler hochzählen. Vorher eingetragene Ereignisse gehen
// verloren.
void logBegin();

// Ein Ereignis eintragen (printf-Format, wird auf gut 70 Zeichen gekürzt).
void logEvent(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Alle Einträge über Serial ausgeben, ältester zuerst.
void logPrint();
void logClear();

// Stellt die Uhr nach der GPS-Zeit ("2026-10-16T11:35:32Z", ageS Sekunden
// alt), damit das Log echte Uhrzeiten hat. Weicht sie um 10 s oder mehr ab
// (Drift des RC-Takts im Tiefschlaf), wird korrigiert und die Abweichung
// eingetragen.
void clockFromGps(const char *utc, uint32_t ageS);
