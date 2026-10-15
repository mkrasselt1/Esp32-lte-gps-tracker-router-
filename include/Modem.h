#pragma once

// Startet die Modem-Task: Modem einschalten, SIM entsperren, GNSS an,
// Mobilfunk per PPP (CMUX), danach alle paar Sekunden GNSS, Signal und
// Versorgungsspannung abfragen. Schlägt etwas fehl, versucht sie es später
// erneut — außer bei falscher PIN, um die SIM nicht zu sperren.
void modemStart();

// Hält die Task an und schaltet das Modem ab (für den Tiefschlaf).
void modemShutdown();
