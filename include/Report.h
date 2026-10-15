#pragma once
#include <Arduino.h>

// Schickt die aktuelle Position an die eingestellte URL. true bei HTTP 2xx.
bool reportSend();

// Zustand als JSON, für /api/status und das Meldeformat "JSON".
String statusJson();
