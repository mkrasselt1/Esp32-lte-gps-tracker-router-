#include "Modem.h"

#include <PPP.h>
#include <esp_netif_ppp.h>

#include "Settings.h"
#include "Status.h"
#include "config.h"

static TaskHandle_t task = nullptr;
static SemaphoreHandle_t lock = nullptr;  // schützt die AT-Befehle über PPP.cmd

// ---------------------------------------------------------------------------
// AT über Serial1, solange PPP den UART noch nicht hat
// ---------------------------------------------------------------------------

static String rawCmd(const String &cmd, uint32_t timeoutMs) {
  while (Serial1.available()) Serial1.read();
  Serial1.print(cmd);
  Serial1.print("\r\n");
  String resp;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    while (Serial1.available()) resp += (char)Serial1.read();
    if (resp.indexOf("\r\nOK") >= 0 || resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  resp.trim();
  return resp;
}

static bool responds(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (rawCmd("AT", 500).indexOf("OK") >= 0) return true;
  }
  return false;
}

static void pulsePwrKey(uint32_t ms) {
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  delay(ms);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
}

static bool statusPinOn() {
#if PIN_MODEM_STATUS >= 0
  return digitalRead(PIN_MODEM_STATUS) == HIGH;
#else
  return false;
#endif
}

static void setupPins() {
  gpio_hold_dis((gpio_num_t)PIN_BOARD_POWERON);
  gpio_hold_dis((gpio_num_t)PIN_MODEM_PWRKEY);
  pinMode(PIN_BOARD_POWERON, OUTPUT);
  digitalWrite(PIN_BOARD_POWERON, HIGH);
  pinMode(PIN_MODEM_DTR, OUTPUT);
  digitalWrite(PIN_MODEM_DTR, LOW);  // LOW = nicht schlafen
#if PIN_MODEM_FLIGHT >= 0
  pinMode(PIN_MODEM_FLIGHT, OUTPUT);
  digitalWrite(PIN_MODEM_FLIGHT, HIGH);  // Funk an
#endif
#if PIN_MODEM_STATUS >= 0
  pinMode(PIN_MODEM_STATUS, INPUT);
#endif
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
#if PIN_MODEM_RESET >= 0
  gpio_hold_dis((gpio_num_t)PIN_MODEM_RESET);
  pinMode(PIN_MODEM_RESET, OUTPUT);
  digitalWrite(PIN_MODEM_RESET, !MODEM_RESET_LEVEL);
#endif
}

// Beendet den CMUX-Modus mit dem Close-Down-Rahmen (CLD auf Kanal 0, 3GPP TS
// 27.010). Danach versteht das Modem wieder AT, ohne dass es ausgeschaltet
// werden muss — das GNSS behält dabei seine Bahndaten und hat sofort wieder
// eine Position, statt minutenlang neu zu suchen.
static bool leaveCmux() {
  static const uint8_t CLD[] = {0xF9, 0x03, 0xEF, 0x05, 0xC3, 0x01, 0xF2, 0xF9};
  Serial1.write(CLD, sizeof(CLD));
  Serial1.flush();
  delay(500);
  return responds(3000);
}

// Bringt das Modem dazu, auf AT zu antworten. Nach einem Neustart des ESP32
// läuft es womöglich noch im CMUX-Modus und versteht kein AT — dann den
// Multiplexer beenden, und erst wenn das nicht hilft, zurücksetzen bzw. aus-
// und wieder einschalten.
static bool powerOn() {
  setupPins();
  Serial1.begin(MODEM_BAUD, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  if (responds(2000)) return true;
  if (leaveCmux()) {
    Serial.println("Modem war noch im CMUX-Modus, Multiplexer beendet");
    return true;
  }

#if PIN_MODEM_RESET >= 0
  Serial.println("Modem zurücksetzen");
  digitalWrite(PIN_MODEM_RESET, MODEM_RESET_LEVEL);
  delay(2600);
  digitalWrite(PIN_MODEM_RESET, !MODEM_RESET_LEVEL);
#else
  if (statusPinOn()) {
    Serial.println("Modem an, aber stumm: ausschalten");
    pulsePwrKey(MODEM_POWEROFF_PULSE_MS);
    for (int i = 0; i < 150 && statusPinOn(); i++) delay(100);
    delay(1000);
  }
#endif

  Serial.println("Modem einschalten");
  pulsePwrKey(MODEM_PWRKEY_PULSE_MS);
  if (responds(30000)) return true;

  // Ohne Statuspin wissen wir nicht, ob es vorher an war und der Puls es
  // eben ausgeschaltet hat.
  pulsePwrKey(MODEM_PWRKEY_PULSE_MS);
  return responds(30000);
}

enum class Sim { Ready, NoSim, PinWrong, Puk, Error };

// PIN selbst eingeben statt über PPP.begin: Das würde es bei jedem
// Neuversuch wiederholen und die SIM nach drei Fehlern sperren.
static Sim unlockSim() {
  String r = rawCmd("AT+CPIN?", 5000);
  for (int i = 0; i < 5 && r.indexOf("+CPIN:") < 0; i++) {
    delay(1000);  // SIM noch nicht bereit
    r = rawCmd("AT+CPIN?", 5000);
  }
  if (r.indexOf("READY") >= 0) return Sim::Ready;
  if (r.indexOf("PUK") >= 0) return Sim::Puk;
  if (r.indexOf("SIM PIN") >= 0) {
    if (settings.simPin.isEmpty()) return Sim::PinWrong;
    r = rawCmd("AT+CPIN=\"" + settings.simPin + "\"", 10000);
    if (r.indexOf("OK") < 0) return Sim::PinWrong;
    delay(3000);
    return Sim::Ready;
  }
  if (r.indexOf("+CPIN:") < 0) return Sim::NoSim;
  return Sim::Error;
}

// ---------------------------------------------------------------------------
// APN automatisch nach SIM: wie ein Handy aus MCC+MNC (den ersten fünf Ziffern
// der IMSI) den passenden Zugangspunkt wählen. Nur die gängigen Netze; ein im
// Setup eingetragener APN hat immer Vorrang. MVNOs (Aldi Talk, 1&1 …) teilen
// sich MCC/MNC mit ihrem Wirtsnetz und bekommen dessen APN — notfalls von Hand
// setzen.
// ---------------------------------------------------------------------------
struct ApnEntry {
  const char *plmn;  // MCC+MNC, die ersten Ziffern der IMSI
  const char *apn;
  const char *user;
  const char *pass;
};
static const ApnEntry APN_TABLE[] = {
    // Deutschland
    {"26201", "internet.telekom", "", ""},  // Telekom
    {"26206", "internet.telekom", "", ""},  // Telekom
    {"26202", "web.vodafone.de", "", ""},   // Vodafone
    {"26204", "web.vodafone.de", "", ""},   // Vodafone
    {"26209", "web.vodafone.de", "", ""},   // Vodafone
    {"26203", "internet", "", ""},          // o2/Telefónica
    {"26207", "internet", "", ""},          // o2/Telefónica
    {"26208", "internet", "", ""},          // o2/Telefónica
    {"26211", "internet", "", ""},          // o2/Telefónica
    {"26277", "internet", "", ""},          // o2/Telefónica
    // einige Nachbarländer
    {"23201", "a1.net", "", ""},                       // A1 (AT)
    {"23203", "internet.t-mobile", "t-mobile", "tm"},  // Magenta (AT)
    {"22801", "gprs.swisscom.ch", "", ""},             // Swisscom (CH)
};

// Zugangsdaten des laufenden Verbindungsversuchs (aus Einstellung oder Tabelle).
static String effApn, effUser, effPass;

// IMSI vom Modem holen (AT+CIMI). Liefert die reinen Ziffern, sonst leer.
static String readImsi() {
  String r = rawCmd("AT+CIMI", 3000);
  String digits;
  for (char c : r)
    if (c >= '0' && c <= '9') digits += c;
  return digits.length() >= 10 ? digits : String();
}

// Setzt effApn/effUser/effPass: eingestellter APN hat Vorrang, sonst nach IMSI
// aus der Tabelle, sonst der generische Rückfall "internet".
static void resolveApn() {
  if (!settings.apn.isEmpty() && !settings.apn.equalsIgnoreCase("auto")) {
    effApn = settings.apn;
    effUser = settings.apnUser;
    effPass = settings.apnPass;
    Serial.printf("APN eingestellt: %s\n", effApn.c_str());
    return;
  }
  String imsi = readImsi();
  for (const ApnEntry &e : APN_TABLE) {
    if (imsi.startsWith(e.plmn)) {
      effApn = e.apn;
      effUser = e.user;
      effPass = e.pass;
      Serial.printf("APN automatisch: %s (PLMN %s)\n", effApn.c_str(), e.plmn);
      return;
    }
  }
  effApn = "internet";
  effUser = "";
  effPass = "";
  Serial.printf("APN nicht erkannt (IMSI %s), Rückfall \"internet\"\n", imsi.isEmpty() ? "?" : imsi.c_str());
}

static void configureModem() {
  // Volle Funktion und automatische Netzregistrierung erzwingen. Das Modem
  // kann aus einer früheren Sitzung abgemeldet sein ("AT+COPS: 2") und bucht
  // sich dann von allein nicht wieder ein, obwohl SIM und Empfang da sind.
  rawCmd("AT+CFUN=1", 10000);
  rawCmd("AT+COPS=0", 5000);
#if defined(BOARD_T_SIM7600)
  // Die GNSS-Antenne hängt an AUXVDD des Modems.
  rawCmd("AT+CVAUXV=2800", 2000);
  rawCmd("AT+CVAUXS=1", 2000);
  rawCmd("AT+CGPS=1", 5000);
#else
  rawCmd("AT+CGNSSPWR=1", 5000);
#endif
  // APN-Zugangsdaten auch im Modem hinterlegen: lwIP kann nur PAP, manche
  // Netze wollen CHAP. Typ 1 = PAP, 2 = CHAP, 3 = beides.
  if (!effUser.isEmpty()) {
    rawCmd("AT+CGAUTH=1,3,\"" + effPass + "\",\"" + effUser + "\"", 5000);
  } else {
    rawCmd("AT+CGAUTH=1,0", 5000);
  }
}

static bool startData() {
  Serial1.end();  // PPP öffnet den UART selbst
  PPP.setApn(effApn.c_str());
  PPP.setPin(NULL);  // schon entsperrt
  PPP.setPins(PIN_MODEM_TX, PIN_MODEM_RX);
  if (!PPP.begin(PPP_MODEM_SIM7600, 1, MODEM_BAUD)) return false;
  if (!effUser.isEmpty()) {
    esp_netif_ppp_set_auth(PPP.netif(), NETIF_PPP_AUTHTYPE_PAP, effUser.c_str(), effPass.c_str());
  }

  status.modemState = "sucht Netz";
  uint32_t start = millis();
  while (!PPP.attached() && millis() - start < 120000) delay(1000);
  if (!PPP.attached()) return false;
  strlcpy(status.operatorName, PPP.operatorName().c_str(), sizeof(status.operatorName));
  Serial.printf("Netz: %s, RSSI %d\n", status.operatorName, PPP.RSSI());

  // CMUX: Daten und AT-Befehle gleichzeitig, sonst kein GNSS bei laufendem Hotspot.
  status.modemState = "verbindet";
  return PPP.mode(ESP_MODEM_MODE_CMUX);
}

// SIM7600: "+CBC: 4.123V", andere: "+CBC: 0,85,4123" (mV). 0 = unbekannt.
static int supplyMv() {
  String r = PPP.cmd("AT+CBC", 2000);
  int colon = r.indexOf(':');
  if (colon < 0) return 0;
  String v = r.substring(colon + 1);
  int nl = v.indexOf('\n');
  if (nl >= 0) v = v.substring(0, nl);
  v.trim();
  if (v.indexOf('V') >= 0) return (int)(v.toFloat() * 1000);
  int comma = v.lastIndexOf(',');
  return comma >= 0 ? v.substring(comma + 1).toInt() : 0;
}

static void waitRetry(const char *state) {
  status.modemState = state;
  Serial.printf("Modem: %s, neuer Versuch in %d s\n", state, MODEM_RETRY_S);
  delay(MODEM_RETRY_S * 1000);
}

static void modemTask(void *) {
  for (;;) {
    status.modemState = "schaltet ein";
    if (!powerOn()) {
      waitRetry("antwortet nicht");
      continue;
    }
    Sim sim = unlockSim();
    if (sim == Sim::PinWrong || sim == Sim::Puk) {
      // Nicht wiederholen: Jeder Versuch kostet einen der drei PIN-Versuche.
      status.modemState = sim == Sim::Puk ? "SIM gesperrt (PUK nötig)" : "SIM-PIN falsch oder fehlt";
      Serial.printf("Modem: %s\n", status.modemState);
      vTaskSuspend(nullptr);
    }
    if (sim != Sim::Ready) {
      waitRetry(sim == Sim::NoSim ? "keine SIM-Karte" : "SIM-Fehler");
      continue;
    }
    resolveApn();
    configureModem();
    if (!startData()) {
      PPP.end();
      waitRetry("keine Verbindung");
      continue;
    }
    break;
  }

  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    GnssFix fix;
    bool ok = gnssRead(fix);
    int rssi = PPP.RSSI();
    int mv = supplyMv();
    xSemaphoreGive(lock);

    status.hasFix = ok;
    if (ok) {
      status.fix = fix;
      status.fixAt = millis();
    }
    status.rssi = rssi;
    status.modemMv = mv;
    status.modemState = status.online ? "verbunden" : "ohne Verbindung";
    delay(GNSS_POLL_INTERVAL_S * 1000);
  }
}

String modemAt(const String &cmd, int timeoutMs) {
  if (!lock || !status.online) return "Modem nicht verbunden";
  xSemaphoreTake(lock, portMAX_DELAY);
  String r = PPP.cmd(cmd.c_str(), timeoutMs);
  xSemaphoreGive(lock);
  return r.isEmpty() ? String("(keine Antwort oder ERROR)") : r;
}

void modemStart() {
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(modemTask, "modem", 8192, nullptr, 1, &task, 1);
}

void modemShutdown() {
  if (task) {
    // Nicht mitten in einem AT-Befehl anhalten.
    if (lock) xSemaphoreTake(lock, pdMS_TO_TICKS(10000));
    vTaskDelete(task);
    task = nullptr;
  }
  if (PPP.mode() != ESP_MODEM_MODE_DATA) PPP.cmd("AT+CPOF", 5000);
  PPP.end();
  delay(2000);
  // Antwortete es nicht (z. B. noch im Startvorgang), hart ausschalten.
  if (statusPinOn()) {
    pulsePwrKey(MODEM_POWEROFF_PULSE_MS);
    for (int i = 0; i < 150 && statusPinOn(); i++) delay(100);
  }
  digitalWrite(PIN_BOARD_POWERON, LOW);
  status.modemState = "aus";
}
