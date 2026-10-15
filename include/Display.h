#pragma once
#include <Arduino.h>

// Optionales 128x64-OLED an I²C. Ohne angeschlossenes Display passiert nichts.
void displayStart();
// Einmal pro Durchlauf der Hauptschleife; zeichnet höchstens einmal pro Sekunde.
void displayLoop(uint32_t now);
// Abschalten, z. B. im Sparmodus und vor dem Tiefschlaf.
void displayOff();
