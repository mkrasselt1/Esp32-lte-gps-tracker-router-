#include "EventLog.h"

#include <Preferences.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

// Ringpuffer im NVS-Flash, je Eintrag ein eigener Schlüssel: Ein neues
// Ereignis schreibt so nur ein paar Dutzend Bytes statt des ganzen Logs.
static const int LOG_SIZE = 32;
static const char *NS = "evlog";

struct LogEntry {
  int64_t t;      // Sekunden der RTC-Uhr; ab CLOCK_VALID echte UTC-Zeit
  uint16_t boot;  // Startzähler
  char msg[72];
};

// Ab hier gilt die Uhr als gestellt (Nov. 2023); davor zählt sie die Sekunden
// seit dem Einschalten.
static const int64_t CLOCK_VALID = 1700000000;

static SemaphoreHandle_t lock = nullptr;  // Ereignisse kommen auch aus der Netzwerk-Task
static uint16_t bootNo = 0;

static int64_t clockNow() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}

static void entryKey(char *key, size_t len, int idx) { snprintf(key, len, "e%02d", idx); }

static void append(const char *msg) {
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  Preferences p;
  if (p.begin(NS, false)) {
    uint8_t head = p.getUChar("head", 0) % LOG_SIZE;
    uint8_t count = p.getUChar("count", 0);
    LogEntry e = {clockNow(), bootNo, {}};
    strlcpy(e.msg, msg, sizeof(e.msg));
    char key[6];
    entryKey(key, sizeof(key), head);
    p.putBytes(key, &e, sizeof(e));
    p.putUChar("head", (head + 1) % LOG_SIZE);
    if (count < LOG_SIZE) p.putUChar("count", count + 1);
    p.end();
  }
  xSemaphoreGive(lock);
}

void logBegin() {
  if (!lock) lock = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin(NS, false)) {
    bootNo = p.getUShort("boot", 0) + 1;
    p.putUShort("boot", bootNo);
    p.end();
  }
}

void logEvent(const char *fmt, ...) {
  char msg[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  append(msg);
}

static void formatTime(int64_t t, char *out, size_t len) {
  if (t < CLOCK_VALID) {
    snprintf(out, len, "+%llds", (long long)t);
    return;
  }
  time_t tt = (time_t)t;
  tm tmv;
  gmtime_r(&tt, &tmv);
  snprintf(out, len, "%02d.%02d. %02d:%02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void logPrint() {
  Preferences p;
  if (!p.begin(NS, true)) {
    Serial.println("Ereignis-Log ist leer");
    return;
  }
  uint8_t head = p.getUChar("head", 0) % LOG_SIZE;
  uint8_t count = p.getUChar("count", 0);
  Serial.printf("--- Ereignis-Log: %u Einträge, jetzt Start #%u, Zeiten in UTC ---\n", count, bootNo);
  for (int i = 0; i < count; i++) {
    char key[6];
    entryKey(key, sizeof(key), (head - count + i + LOG_SIZE) % LOG_SIZE);
    LogEntry e;
    if (p.getBytes(key, &e, sizeof(e)) != sizeof(e)) continue;
    e.msg[sizeof(e.msg) - 1] = 0;
    char when[20];
    formatTime(e.t, when, sizeof(when));
    Serial.printf("#%-4u %-15s %s\n", e.boot, when, e.msg);
  }
  Serial.println("---");
  p.end();
}

void logClear() {
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  Preferences p;
  if (p.begin(NS, false)) {
    uint16_t boot = p.getUShort("boot", 0);
    p.clear();
    p.putUShort("boot", boot);  // Startzähler weiterzählen
    p.end();
  }
  if (lock) xSemaphoreGive(lock);
  Serial.println("Ereignis-Log gelöscht");
}

// Tage seit 1970-01-01 für ein Datum im gregorianischen Kalender (nach Howard
// Hinnant); timegm() gibt es hier nicht.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

void clockFromGps(const char *utc) {
  int y, mo, d, h, mi, s;
  if (!utc || sscanf(utc, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return;
  int64_t gps = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  if (gps < CLOCK_VALID) return;
  int64_t now = clockNow();
  int64_t diff = now - gps;
  if (now >= CLOCK_VALID && diff > -5 && diff < 5) return;
  timeval tv = {(time_t)gps, 0};
  settimeofday(&tv, nullptr);
  if (now >= CLOCK_VALID) logEvent("Uhr nach GPS korrigiert, sie ging %+lld s", (long long)diff);
}
