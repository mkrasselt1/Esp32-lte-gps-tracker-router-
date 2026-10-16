// LTE-Router mit GPS-Tracker für LilyGO T-SIM7600 / T-A7670:
//  - Mobilfunk per PPP, als WLAN-Hotspot mit NAT weitergegeben
//  - Position aus dem GNSS des Modems an eine URL (Home Assistant GPSLogger
//    oder JSON), dazu Netz-/Akkubetrieb und Akkustand
//  - Weboberfläche mit Login für alle Einstellungen
//  - Energiesparen auf Akku in Stufen bis zum Tiefschlaf, siehe README

#include <Arduino.h>
#include <PPP.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "Clients.h"
#include "EventLog.h"
#include "Modem.h"
#include "Report.h"
#include "Settings.h"
#include "Sleep.h"
#include "Status.h"
#include "WebUi.h"
#include "config.h"

Status status;

const char *modeName(Mode m) {
  switch (m) {
    case Mode::Normal: return "Netzbetrieb";
    case Mode::Battery: return "Akkubetrieb";
    case Mode::Tracker: return "Sparmodus";
    case Mode::WakeReport: return "aus Tiefschlaf geweckt";
  }
  return "";
}

// Überlebt den Tiefschlaf.
struct RtcFix {
  bool valid;
  double lat, lon;
  float altitude, hdop;
  int satellites;
};
RTC_DATA_ATTR static RtcFix rtcFix;

static bool apActive = false;
static uint32_t nextReportAt = 0;
static uint32_t pppLostSince = 0;   // 0 = verbunden oder nie verbunden gewesen
static uint32_t batterySince = 0;
static uint32_t lastClientAt = 0;   // zuletzt ein Gerät im Hotspot
static uint32_t lastMovedAt = 0;
static uint32_t trackerSince = 0;
static uint32_t wakeStartedAt = 0;

static bool moving() { return status.hasFix && status.fix.speedKmh >= MOVING_SPEED_KMH; }

// Für das Ereignis-Log: Stromquelle bzw. Akkuspannung.
static String powerText() {
  if (status.power.externalPower) return "am Netz";
  char buf[16];
  snprintf(buf, sizeof(buf), "Akku %.2f V", status.power.batteryMv / 1000.0f);
  return buf;
}

// ---------------------------------------------------------------------------
// Netzwerk
// ---------------------------------------------------------------------------

static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  clientsOnEvent(event, info);
  switch (event) {
    case ARDUINO_EVENT_PPP_GOT_IP:
      Serial.print("Mobilfunk verbunden: ");
      Serial.println(PPP.localIP());
      PPP.setDefault();
      status.online = true;
      if (apActive) WiFi.AP.enableNAPT(true);
      pppLostSince = 0;
      logEvent("Mobilfunk verbunden nach %lu s", millis() / 1000);
      break;
    case ARDUINO_EVENT_PPP_LOST_IP:
    case ARDUINO_EVENT_PPP_DISCONNECTED:
      if (!status.online) break;
      Serial.println("Mobilfunk getrennt");
      logEvent("Mobilfunk getrennt");
      status.online = false;
      if (apActive) WiFi.AP.enableNAPT(false);
      pppLostSince = millis() | 1;
      break;
    default:
      break;
  }
}

static void startHotspot() {
  WiFi.AP.begin();
  WiFi.AP.config(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0),
                 IPAddress(192, 168, 4, 2), IPAddress(1, 1, 1, 1));
  WiFi.AP.create(settings.apSsid.c_str(), settings.apPass.c_str(), settings.apChannel, 0, settings.apMaxClients);
  WiFi.setSleep(false);
  apActive = true;
  webStart();
  if (status.online) WiFi.AP.enableNAPT(true);
  lastClientAt = millis();
  Serial.printf("Hotspot \"%s\" an\n", settings.apSsid.c_str());
}

static void stopHotspot() {
  if (!apActive) return;
  WiFi.AP.enableNAPT(false);
  WiFi.AP.end();
  WiFi.mode(WIFI_OFF);
  apActive = false;
  Serial.println("Hotspot aus");
}

// ---------------------------------------------------------------------------
// Betriebsstufen
// ---------------------------------------------------------------------------

static void setMode(Mode m, const char *reason) {
  if (status.mode == m) return;
  Serial.printf("Betrieb: %s (%s)\n", modeName(m), reason);
  logEvent("%s: %s, %s", modeName(m), reason, powerText().c_str());
  status.mode = m;
  status.modeReason = reason;
  status.modeSince = millis();
  nextReportAt = millis();  // Wechsel sofort melden
}

static void holdPinsForSleep() {
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  gpio_hold_en((gpio_num_t)PIN_MODEM_PWRKEY);
  digitalWrite(PIN_BOARD_POWERON, LOW);
  gpio_hold_en((gpio_num_t)PIN_BOARD_POWERON);
#if PIN_MODEM_RESET >= 0
  digitalWrite(PIN_MODEM_RESET, !MODEM_RESET_LEVEL);
  gpio_hold_en((gpio_num_t)PIN_MODEM_RESET);
#endif
  gpio_deep_sleep_hold_en();
}

// Meldet ein letztes Mal, schaltet alles ab und schläft.
[[noreturn]] static void enterDeepSleep(const char *reason) {
  Serial.printf("Tiefschlaf: %s\n", reason);
  status.modeReason = reason;
  bool sent = reportSend();
  logEvent("Tiefschlaf: %s, Meldung %s, %s", reason,
           settings.reportUrl.isEmpty() ? "aus" : (sent ? "ok" : "fehlgeschlagen"), powerText().c_str());
  if (status.fix.valid) {
    rtcFix = {true, status.fix.lat, status.fix.lon, status.fix.altitude, status.fix.hdop, status.fix.satellites};
  }
  sleepScheduleReport();
  stopHotspot();
  modemShutdown();
  holdPinsForSleep();
  sleepStart();
}

static void updateMode(uint32_t now) {
  if (status.power.externalPower) {
    if (status.mode != Mode::Normal) {
      if (!apActive) startHotspot();
      setMode(Mode::Normal, "am Netz");
    }
    return;
  }

  if (status.mode == Mode::Normal) {
    batterySince = now;
    lastClientAt = now;
    setMode(Mode::Battery, "Netzstrom weg");
  }

  if (settings.lowBatteryPercent && status.power.batteryMv > 0 &&
      status.power.batteryPercent <= settings.lowBatteryPercent) {
    enterDeepSleep("Akku fast leer");
  }

  if (status.mode == Mode::Battery) {
    if (clientsConnected() > 0) lastClientAt = now;
    const char *why = nullptr;
    uint32_t gone = 0;
    if (settings.hotspotMaxBatteryMin && now - batterySince >= settings.hotspotMaxBatteryMin * 60000UL) {
      why = "Hotspot-Zeit auf Akku abgelaufen";
    } else if (settings.hotspotIdleMin && now - lastClientAt >= settings.hotspotIdleMin * 60000UL) {
      why = "kein Gerät mehr im WLAN";
    } else if (!settings.triggerMac.isEmpty() && !clientIsConnected(settings.triggerMac, &gone) && gone &&
               now - gone >= settings.triggerGraceMin * 60000UL) {
      why = "Auslöser-Gerät getrennt";
    }
    if (why) {
      stopHotspot();
      trackerSince = now;
      setMode(Mode::Tracker, why);
    }
  }

  if (status.mode == Mode::Tracker && settings.sleepAfterParkedMin) {
    uint32_t still = now - max(lastMovedAt, trackerSince);
    if (still >= settings.sleepAfterParkedMin * 60000UL) enterDeepSleep("Stillstand");
  }
}

static uint32_t reportInterval() {
  if (status.mode == Mode::Tracker) {
    return moving() ? settings.trackerMovingS * 1000UL : settings.trackerParkedMin * 60000UL;
  }
  return (moving() ? settings.reportMovingS : settings.reportParkedS) * 1000UL;
}

static void handleReports(uint32_t now) {
  uint32_t interval = reportInterval();
  // Losfahren: nicht das lange Stand-Intervall abwarten.
  if ((int32_t)(nextReportAt - now) > (int32_t)interval) nextReportAt = now + interval;
  if ((int32_t)(now - nextReportAt) >= 0) {
    // Fehlgeschlagen oder noch offline: in 30 s nochmal.
    nextReportAt = now + (reportSend() ? interval : 30000UL);
  }
}

// Aus dem Tiefschlaf zum Melden geweckt: Fix abwarten, melden, weiterschlafen.
// Bewegt sich das Gerät, bleibt es wach im Sparmodus.
static void handleWake(uint32_t now) {
  static int attempts = 0;
  if (status.power.externalPower) {
    logEvent("Netzstrom beim Melden, Neustart");
    ESP.restart();
  }
  bool fixTimeout = now - wakeStartedAt >= WAKE_FIX_TIMEOUT_S * 1000UL;
  if (status.online && (status.hasFix || fixTimeout) && attempts < 3) {
    attempts++;
    if (reportSend() || attempts == 3) {
      if (moving()) {
        trackerSince = now;
        lastMovedAt = now;
        setMode(Mode::Tracker, "Bewegung erkannt");
        return;
      }
      enterDeepSleep("Meldung erledigt");
    }
    delay(5000);
  }
  if (now - wakeStartedAt >= WAKE_MAX_AWAKE_S * 1000UL) enterDeepSleep("Wachzeit abgelaufen");
}

// Für das Ereignis-Log: erster Fix nach dem Start, Uhr nach GPS stellen und
// abseits vom Netz Losfahren und Anhalten (höchstens einmal pro Minute, das
// GPS rauscht im Stand um 1–3 km/h).
static void logGnss(uint32_t now) {
  static bool fixLogged = false;
  static uint32_t lastClockAt = 0;
  static bool wasMoving = false;
  static uint32_t lastMoveLogAt = 0;
  if (!status.hasFix) return;
  if (!lastClockAt || now - lastClockAt >= 60000) {
    lastClockAt = now;
    clockFromGps(status.fix.utc);
  }
  if (!fixLogged) {
    fixLogged = true;
    logEvent("GPS-Fix nach %lu s, %d Satelliten, HDOP %.1f", now / 1000, status.fix.satellites, status.fix.hdop);
  }
  bool mv = moving();
  if (mv != wasMoving && status.mode != Mode::Normal && (!lastMoveLogAt || now - lastMoveLogAt >= 60000)) {
    wasMoving = mv;
    lastMoveLogAt = now;
    logEvent(mv ? "fährt (%.1f km/h)" : "steht (%.1f km/h)", status.fix.speedKmh);
  }
}

// ---------------------------------------------------------------------------
// Werkseinstellungen per USB: "factory-reset" im seriellen Monitor. Ein
// Taster an GPIO0 wäre bequemer, aber die Auto-Reset-Schaltung des USB-Chips
// zieht GPIO0 beim Öffnen mancher Terminals auf GND.
// ---------------------------------------------------------------------------

static void checkSerialCommand() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "factory-reset") {
    Serial.println("Werkseinstellungen, Neustart");
    settingsFactoryReset();
    delay(200);
    ESP.restart();
  } else if (line == "status") {
    // Zustand über USB ausgeben, ohne Login und ohne Datenverkehr über die SIM.
    Serial.println(statusJson());
  } else if (line == "log") {
    logPrint();
  } else if (line == "log-clear") {
    logClear();
  } else if (line.startsWith("AT") || line.startsWith("at")) {
    // AT-Befehl ans laufende Modem durchreichen, zum Testen.
    Serial.println(modemAt(line, 10000));
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  powerInit();
  settingsLoad();
  uint32_t now = millis();

  if (rtcFix.valid) {
    status.fix.valid = true;
    status.fix.lat = rtcFix.lat;
    status.fix.lon = rtcFix.lon;
    status.fix.altitude = rtcFix.altitude;
    status.fix.hdop = rtcFix.hdop;
    status.fix.satellites = rtcFix.satellites;
  }

  Wake wake = sleepWakeReason();
  if (wake == Wake::SleepAgain) sleepStart();
  logBegin();
  PowerState startPower = powerRead(0);
  if (startPower.externalPower) {
    logEvent("Start: %s, am Netz", sleepWakeText());
  } else {
    logEvent("Start: %s, Akku %.2f V", sleepWakeText(), startPower.batteryMv / 1000.0f);
  }
  if (wake == Wake::Report || wake == Wake::Motion) {
    Serial.println(wake == Wake::Motion ? "Bewegung: Position melden" : "Aus dem Tiefschlaf: Position melden");
    status.mode = Mode::WakeReport;
    status.modeReason = wake == Wake::Motion ? "Bewegung erkannt" : "Meldung aus dem Tiefschlaf";
    wakeStartedAt = now;
    WiFi.mode(WIFI_OFF);
    Network.onEvent(onNetworkEvent);
    modemStart();
    return;
  }

  Serial.println("\nLTE-Router startet");
  Network.onEvent(onNetworkEvent);
  startHotspot();
  modemStart();
  if (!settingsConfigured()) Serial.println("Noch nicht eingerichtet: http://192.168.4.1/setup");

  status.power = powerRead(0);
  status.modeSince = now;
  lastMovedAt = now;
  nextReportAt = now;
  if (!status.power.externalPower) {
    batterySince = now;
    status.mode = Mode::Battery;
    status.modeReason = "Start auf Akku";
  } else {
    status.modeReason = "am Netz";
  }
}

void loop() {
  uint32_t now = millis();
  static uint32_t lastPowerRead = 0;
  if (now - lastPowerRead >= 1000) {
    lastPowerRead = now;
    status.power = powerRead(status.modemMv);
  }
  if (moving()) lastMovedAt = now;
  checkSerialCommand();
  logGnss(now);

  if (status.mode == Mode::WakeReport) {
    handleWake(now);
    delay(20);
    return;
  }

  webLoop();
  updateMode(now);
  handleReports(now);

  if (pppLostSince && now - pppLostSince > PPP_RESTART_AFTER_S * 1000UL) {
    Serial.println("Mobilfunk zu lange weg, Neustart");
    logEvent("Mobilfunk zu lange weg, Neustart");
    delay(500);
    ESP.restart();
  }
  delay(5);
}
