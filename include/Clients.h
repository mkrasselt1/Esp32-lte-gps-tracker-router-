#pragma once
#include <Arduino.h>
#include <Network.h>

// Geräte im Hotspot, aus den WLAN-Ereignissen mitgeschrieben. Damit lässt
// sich in der Weboberfläche das Gerät wählen, dessen Abmelden den Hotspot
// beendet, und erkennen, wann es gegangen ist.
void clientsOnEvent(arduino_event_id_t event, arduino_event_info_t info);

int clientsConnected();
// Ist das Gerät verbunden? Sonst: seit wann (millis) weg, 0 = nie gesehen.
bool clientIsConnected(const String &mac, uint32_t *goneSince = nullptr);
// MAC zur IP eines verbundenen Geräts, leer wenn unbekannt.
String clientMacForIp(IPAddress ip);
// JSON-Liste für die Weboberfläche.
String clientsJson(IPAddress requester);
