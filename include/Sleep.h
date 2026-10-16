#pragma once

// Warum der ESP32 gerade läuft.
enum class Wake {
  Normal,     // Kaltstart, Reset, oder aus dem Schlaf mit Netzstrom
  Report,     // Meldezeit im Tiefschlaf erreicht
  Motion,     // Bewegungssensor hat geweckt
  SleepAgain  // nur nachgesehen, nichts zu tun
};

// In setup() als Erstes: wertet den Weckgrund aus.
Wake sleepWakeReason();

// Weckgrund bzw. Grund des Starts als Text, nach sleepWakeReason().
const char *sleepWakeText();

// Nächste Meldung aus dem Tiefschlaf ab jetzt planen.
void sleepScheduleReport();

// Tiefschlaf: Weckquellen (ULP für Netzstrom, Bewegung, Timer) einrichten und
// schlafen. Modem und WLAN müssen vorher aus sein.
[[noreturn]] void sleepStart();
