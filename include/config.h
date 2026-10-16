#pragma once
// Hardware und feste Zeiten. Alles, was der Betreiber einstellt (APN, PIN,
// WLAN, Meldeziel, Energiesparen), steht in den Einstellungen der Weboberfläche.

// ---------------------------------------------------------------------------
// Board: genau eins auswählen. Pins laut LilyGO (examples/*/utilities.h und
// docs/en/esp32/ im Repo Xinyuan-LilyGO/LilyGO-T-A76XX).
// ---------------------------------------------------------------------------
#define BOARD_T_SIM7600   // T-SIM7600E/G (ESP32)
// #define BOARD_T_A7670  // T-A7670E/G (ESP32)

#if defined(BOARD_T_SIM7600)
#define PIN_MODEM_TX      27  // ESP32 sendet -> Modem
#define PIN_MODEM_RX      26  // ESP32 empfängt <- Modem
#define PIN_MODEM_PWRKEY   4
#define PIN_MODEM_RESET   -1  // hat keinen
#define PIN_MODEM_DTR     32
#define PIN_MODEM_FLIGHT  25  // muss HIGH sein, sonst Flugmodus
#define PIN_MODEM_STATUS  34  // HIGH = Modem an
#define PIN_BOARD_POWERON 12  // hier nur die LED
#define MODEM_PWRKEY_PULSE_MS     500
#define MODEM_POWEROFF_PULSE_MS  3000
// Bei USB-C bzw. VBUS trennt das Board den Teiler an GPIO35, der ADC liest 0.
// Genau daran erkennt die Firmware den Netzbetrieb.
#define BATTERY_ADC_ZERO_ON_VBUS 1

#elif defined(BOARD_T_A7670)
#define PIN_MODEM_TX      26
#define PIN_MODEM_RX      27
#define PIN_MODEM_PWRKEY   4
#define PIN_MODEM_RESET    5
#define MODEM_RESET_LEVEL HIGH
#define PIN_MODEM_DTR     25
#define PIN_MODEM_FLIGHT  -1
#define PIN_MODEM_STATUS  -1
#define PIN_BOARD_POWERON 12  // schaltet die Modemversorgung
#define MODEM_PWRKEY_PULSE_MS     100
#define MODEM_POWEROFF_PULSE_MS  3000
#define BATTERY_ADC_ZERO_ON_VBUS 0

#else
#error "BOARD_T_SIM7600 oder BOARD_T_A7670 in config.h wählen"
#endif

#define MODEM_BAUD 115200

// ---------------------------------------------------------------------------
// Stromversorgung
// ---------------------------------------------------------------------------
// Akku hängt über einen 1:1-Teiler (2 x 100k) an GPIO35.
#define PIN_BATTERY_ADC     35
#define BATTERY_DIVIDER     2.0f

// Netzbetrieb erkennen. Beim T-SIM7600 ergibt sich das aus GPIO35 (siehe
// oben), dann bleibt PIN_POWER_SENSE auf -1. Sonst: Spannungsteiler vom
// 5-V-Eingang auf einen freien ADC1-Pin, z. B. 5 V -- 100k -- GPIO34 --
// 100k -- GND, und hier Pin und Teiler eintragen. Ohne beides wird aus der
// Akkuspannung geschätzt: über BATTERY_CHARGING_MV gilt als "lädt".
#define PIN_POWER_SENSE     -1
#define POWER_SENSE_DIVIDER 2.0f
#define POWER_SENSE_MIN_MV  4300
#define BATTERY_ADC_ZERO_MV 500   // darunter gilt GPIO35 als abgetrennt
#define BATTERY_CHARGING_MV 4180

// ---------------------------------------------------------------------------
// Feste Zeiten
// ---------------------------------------------------------------------------
#define GNSS_POLL_INTERVAL_S  5
#define MOVING_SPEED_KMH      3.0f
#define HTTP_TIMEOUT_MS       15000
#define MODEM_RETRY_S         30    // nach Fehler beim Modemstart
// Ohne Mobilfunk so lange warten, dann neu starten.
#define PPP_RESTART_AFTER_S   300
// Akku gilt erst als leer, wenn die Ruhespannung (höchster Wert der letzten
// Minute) so lange unter der Schwelle liegt. Beim Senden zieht das Modem bis
// zu 2 A, dann bricht die Spannung kurz um mehrere hundert mV ein.
#define LOW_BATTERY_HOLD_S    120
// ---------------------------------------------------------------------------
// Tiefschlaf
// ---------------------------------------------------------------------------
// Netzstrom erkennt im Tiefschlaf der ULP-Koprozessor: Er misst jede Sekunde
// den ADC (GPIO35 bzw. PIN_POWER_SENSE) und weckt den ESP32, sobald Strom da
// ist. Rohwerte des 12-Bit-ADC bei 12 dB Dämpfung, grob 4095 = 3,1 V.
#define ULP_CHECK_INTERVAL_MS   1000
#define ULP_VBUS_RAW_BELOW      600   // T-SIM7600: GPIO35 unter ~0,45 V = Netz
// Boards ohne beides: so oft per Timer aufwachen und nachsehen.
#define SLEEP_FALLBACK_CHECK_MIN 10

// Optionaler Bewegungs-/Erschütterungssensor, dessen Interrupt den ESP32
// weckt (z. B. LIS3DH INT1, SW-420-Modul). Muss ein RTC-GPIO sein; auf dem
// T-SIM7600 ist GPIO39 frei. -1 = keiner.
#define PIN_MOTION_WAKE   -1
#define MOTION_WAKE_LEVEL HIGH
// Nach einem Bewegungsalarm so lange nicht erneut darauf reagieren, damit
// Wind oder Stöße den Akku nicht leerwecken.
#define MOTION_COOLDOWN_MIN 30

// Aus dem Tiefschlaf geweckt: so lange auf einen GPS-Fix warten.
#define WAKE_FIX_TIMEOUT_S    150
// ... und insgesamt höchstens so lange wach bleiben.
#define WAKE_MAX_AWAKE_S      300
