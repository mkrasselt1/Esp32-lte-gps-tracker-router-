#pragma once
#include <Arduino.h>

// Startet die Modem-Task: Modem einschalten, SIM entsperren, GNSS an,
// Mobilfunk per PPP (CMUX), danach alle paar Sekunden GNSS, Signal und
// Versorgungsspannung abfragen. Schlägt etwas fehl, versucht sie es später
// erneut — außer bei falscher PIN, um die SIM nicht zu sperren.
void modemStart();

// Hält die Task an und schaltet das Modem ab (für den Tiefschlaf).
void modemShutdown();

// Schickt einen AT-Befehl im laufenden Betrieb (über CMUX) und liefert die
// Antwort. Zum Testen über den seriellen Monitor.
String modemAt(const String &cmd, int timeoutMs);
