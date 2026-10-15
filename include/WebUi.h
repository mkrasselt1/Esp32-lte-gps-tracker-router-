#pragma once

// Weboberfläche im Hotspot unter http://192.168.4.1/
//  /           Status
//  /settings   Einstellungen (APN, PIN, WLAN, Meldungen, Energiesparen)
//  /setup      Ersteinrichtung, solange kein Admin-Passwort gesetzt ist
// Login per HTTP-Digest, Benutzer "admin".
void webStart();
void webLoop();
