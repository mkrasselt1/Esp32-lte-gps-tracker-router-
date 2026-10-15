#include "Sleep.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp32-hal-periman.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <esp32/ulp.h>
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

  const ulp_insn_t program[] = {
      I_ADC(R0, 0, (uint32_t)channel),
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

Wake sleepWakeReason() {
  bool wasSleeping = rtcSleeping;
  rtcSleeping = false;
  if (!wasSleeping) return Wake::Normal;

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_ULP) return Wake::Normal;  // Netzstrom zurück
  if (powerRead(0).externalPower) return Wake::Normal;

  int64_t now = rtcSeconds();
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    rtcMotionArmedAt = now + MOTION_COOLDOWN_MIN * 60;
    return Wake::Motion;
  }
  if (cause == ESP_SLEEP_WAKEUP_TIMER && now >= rtcNextReportAt) return Wake::Report;
  // Zeitgeber für Bewegungssperre oder Netzstrom-Nachsehen: weiterschlafen.
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return Wake::SleepAgain;
  return Wake::Normal;  // Reset-Taste o. ä.
}

void sleepScheduleReport() { rtcNextReportAt = rtcSeconds() + settings.sleepReportMin * 60LL; }

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
