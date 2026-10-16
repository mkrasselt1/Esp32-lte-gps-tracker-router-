#include "Sleep.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp32-hal-periman.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sys/time.h>
#include <esp32/ulp.h>
#include <soc/sens_reg.h>
#include <ulp_adc.h>

#include "Power.h"
#include "Settings.h"
#include "config.h"

// Überlebt den Tiefschlaf. Zeiten in Sekunden der RTC-Uhr, die im Schlaf
// weiterläuft.
RTC_DATA_ATTR static bool rtcSleeping;
RTC_DATA_ATTR static int64_t rtcNextReportAt;
RTC_DATA_ATTR static int64_t rtcMotionArmedAt;

static int64_t rtcSeconds() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}

#if BATTERY_ADC_ZERO_ON_VBUS
#define ULP_PIN PIN_BATTERY_ADC
#define ULP_WAKE_BELOW 1
#define ULP_THRESHOLD ULP_VBUS_RAW_BELOW
#elif PIN_POWER_SENSE >= 0
#define ULP_PIN PIN_POWER_SENSE
#define ULP_WAKE_BELOW 0
#define ULP_THRESHOLD ((int)(POWER_SENSE_MIN_MV / POWER_SENSE_DIVIDER * 4095 / 3100))
#endif

#ifdef ULP_PIN
// Zwei Datenworte im RTC-Speicher hinter dem ULP-Programm, zur Diagnose:
// Zahl der Läufe und zuletzt gemessener ADC-Rohwert.
#define ULP_DATA 100

// Kleines ULP-Programm: ADC lesen, bei Netzstrom den ESP32 wecken. Läuft
// jede Sekunde für ein paar Mikrosekunden; der Schlafstrom steigt dadurch
// nur wenig.
static bool startUlp() {
  // Arduino hält den ADC1 für analogRead belegt. Freigeben, sonst bekommt
  // der ULP ihn nicht.
  perimanClearPinBus(PIN_BATTERY_ADC);
#if PIN_POWER_SENSE >= 0
  perimanClearPinBus(PIN_POWER_SENSE);
#endif
  adc_unit_t unit;
  adc_channel_t channel;
  if (adc_oneshot_io_to_channel(ULP_PIN, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) return false;

  ulp_adc_cfg_t cfg = {
      .adc_n = ADC_UNIT_1,
      .channel = channel,
      .atten = ADC_ATTEN_DB_12,
      .width = ADC_BITWIDTH_12,
      .ulp_mode = ADC_ULP_MODE_FSM,
  };
  if (ulp_adc_init(&cfg) != ESP_OK) return false;

  RTC_SLOW_MEM[ULP_DATA] = 0;
  RTC_SLOW_MEM[ULP_DATA + 1] = 0;
  const ulp_insn_t program[] = {
      // ESP-IDF schaltet den SAR-ADC beim Tiefschlaf hart ab (FORCE_XPD_SAR
      // = 2); dann misst der ULP 4095 und weckt nie. Deshalb für jede Messung
      // selbst einschalten (3), kurz einschwingen lassen (~125 µs) und danach
      // wieder abschalten.
      I_WR_REG(SENS_SAR_MEAS_WAIT2_REG, SENS_FORCE_XPD_SAR_S, SENS_FORCE_XPD_SAR_S + 1, 3),
      I_DELAY(1000),
      I_ADC(R0, 0, (uint32_t)channel),
      I_WR_REG(SENS_SAR_MEAS_WAIT2_REG, SENS_FORCE_XPD_SAR_S, SENS_FORCE_XPD_SAR_S + 1, 2),
      I_MOVI(R2, ULP_DATA),
      I_ST(R0, R2, 1),  // letzter ADC-Wert
      I_LD(R1, R2, 0),
      I_ADDI(R1, R1, 1),
      I_ST(R1, R2, 0),  // Läufe
#if ULP_WAKE_BELOW
      M_BGE(1, ULP_THRESHOLD),  // noch Akku -> schlafen lassen
#else
      M_BL(1, ULP_THRESHOLD),
#endif
      I_WAKE(),
      M_LABEL(1),
      I_HALT(),
  };
  size_t size = sizeof(program) / sizeof(ulp_insn_t);
  if (ulp_process_macros_and_load(0, program, &size) != ESP_OK) return false;
  ulp_set_wakeup_period(0, ULP_CHECK_INTERVAL_MS * 1000);
  if (ulp_run(0) != ESP_OK) return false;
  esp_sleep_enable_ulp_wakeup();
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  return true;
}
#endif

void sleepInit() {
#ifdef ULP_PIN
  // Der ULP läuft nach dem Aufwachen (und nach einem Software-Reset) weiter
  // und schaltet den SAR-ADC jede Sekunde ab. Vor der ersten Messung der
  // Firmware anhalten, sonst hängt analogRead() und der Watchdog schlägt zu.
  ulp_timer_stop();
  delay(1);  // eine laufende ULP-Messung zu Ende kommen lassen
  REG_SET_FIELD(SENS_SAR_MEAS_WAIT2_REG, SENS_FORCE_XPD_SAR, 3);
#endif
}

static const char *wakeText = "";
static char wakeBuf[80];

// Hängt die ULP-Diagnose aus dem letzten Schlaf an den Weckgrund an.
static const char *withUlpDiag(const char *text) {
#ifdef ULP_PIN
  snprintf(wakeBuf, sizeof(wakeBuf), "%s (ULP %ux, ADC %u)", text, (unsigned)(RTC_SLOW_MEM[ULP_DATA] & 0xFFFF),
           (unsigned)(RTC_SLOW_MEM[ULP_DATA + 1] & 0xFFFF));
  return wakeBuf;
#else
  return text;
#endif
}

static const char *resetText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "eingeschaltet";
    case ESP_RST_SW: return "Neustart durch die Firmware";
    case ESP_RST_PANIC: return "Neustart nach Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Neustart durch Watchdog";
    case ESP_RST_BROWNOUT: return "Neustart nach Unterspannung";
    case ESP_RST_EXT: return "Reset-Pin";
    default: return "Neustart";
  }
}

const char *sleepWakeText() { return wakeText; }

Wake sleepWakeReason() {
  bool wasSleeping = rtcSleeping;
  rtcSleeping = false;
  if (!wasSleeping) {
    wakeText = resetText();
    return Wake::Normal;
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_ULP) {
    wakeText = withUlpDiag("geweckt durch Netzstrom");
    return Wake::Normal;
  }
  if (powerRead(0).externalPower) {
    wakeText = withUlpDiag("geweckt, Netzstrom erkannt");
    return Wake::Normal;
  }

  int64_t now = rtcSeconds();
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    rtcMotionArmedAt = now + MOTION_COOLDOWN_MIN * 60;
    wakeText = withUlpDiag("geweckt durch Bewegung");
    return Wake::Motion;
  }
  if (cause == ESP_SLEEP_WAKEUP_TIMER && now >= rtcNextReportAt) {
    wakeText = withUlpDiag("geweckt zur Meldung");
    return Wake::Report;
  }
  // Zeitgeber für Bewegungssperre oder Netzstrom-Nachsehen: weiterschlafen.
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wakeText = "kurz geweckt, schläft weiter";
    return Wake::SleepAgain;
  }
  wakeText = withUlpDiag("per Reset aus dem Tiefschlaf");  // Reset-Taste o. ä.
  return Wake::Normal;
}

void sleepScheduleReport(uint32_t inSeconds) {
  rtcNextReportAt = rtcSeconds() + (inSeconds ? inSeconds : settings.sleepReportMin * 60LL);
}

void sleepStart() {
  int64_t now = rtcSeconds();
  int64_t wakeAt = rtcNextReportAt > now ? rtcNextReportAt : now + settings.sleepReportMin * 60LL;

  bool ulp = false;
#ifdef ULP_PIN
  ulp = startUlp();
#endif
  if (!ulp) {
    Serial.println("ULP nicht verfügbar, Netzstrom per Zeitgeber prüfen");
    wakeAt = min(wakeAt, now + SLEEP_FALLBACK_CHECK_MIN * 60LL);
  }

#if PIN_MOTION_WAKE >= 0
  if (now >= rtcMotionArmedAt) {
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_MOTION_WAKE,
                                 MOTION_WAKE_LEVEL == HIGH ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  } else {
    wakeAt = min(wakeAt, rtcMotionArmedAt);  // danach wieder scharf schalten
  }
#endif

  int64_t seconds = max<int64_t>(wakeAt - now, 1);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  rtcSleeping = true;
  Serial.printf("Tiefschlaf, spätestens wach in %lld s%s\n", seconds, ulp ? ", Netzstrom per ULP" : "");
  Serial.flush();
  esp_deep_sleep_start();
}
