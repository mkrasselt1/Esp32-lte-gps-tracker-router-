#include "Report.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <PPP.h>
#include <WiFi.h>

#include "Clients.h"
#include "Settings.h"
#include "Status.h"
#include "config.h"

static String urlEncode(const String &s) {
  String out;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

static String jsonEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if ((unsigned char)c >= 0x20) out += c;
  }
  return out;
}

String statusJson() {
  const GnssFix &f = status.fix;
  const PowerState &p = status.power;
  long fixAge = status.fixAt ? (long)((millis() - status.fixAt) / 1000) : -1;
  long reportAge = status.lastReportOkAt ? (long)((millis() - status.lastReportOkAt) / 1000) : -1;
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"device\":\"%s\",\"uptime_s\":%lu,"
           "\"mode\":{\"name\":\"%s\",\"reason\":\"%s\",\"since_s\":%lu},"
           "\"gps\":{\"fix\":%s,\"valid\":%s,\"age_s\":%ld,\"latitude\":%.6f,\"longitude\":%.6f,"
           "\"altitude_m\":%.1f,\"accuracy_m\":%.0f,\"speed_kmh\":%.1f,\"course\":%.0f,\"satellites\":%d,"
           "\"hdop\":%.1f,\"utc\":\"%s\"},"
           "\"power\":{\"external\":%s,\"sensed\":%s,\"battery_mv\":%d,\"battery_percent\":%d,\"charging\":%s,\"battery_adc_mv\":%d},"
           "\"mobile\":{\"state\":\"%s\",\"connected\":%s,\"operator\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
           "\"hotspot\":{\"active\":%s,\"ssid\":\"%s\",\"clients\":%d},"
           "\"report\":{\"enabled\":%s,\"last_http_code\":%d,\"last_ok_s_ago\":%ld}}",
           jsonEscape(settings.deviceName).c_str(), (unsigned long)(millis() / 1000), modeName(status.mode),
           status.modeReason, (unsigned long)((millis() - status.modeSince) / 1000), status.hasFix ? "true" : "false",
           f.valid ? "true" : "false", fixAge, f.lat, f.lon, f.altitude, f.accuracyM(), f.speedKmh, f.course,
           f.satellites, f.hdop, f.utc, p.externalPower ? "true" : "false", p.sensed ? "true" : "false", p.batteryMv,
           p.batteryPercent, p.charging ? "true" : "false", p.adcPinMv, status.modemState, status.online ? "true" : "false",
           jsonEscape(status.operatorName).c_str(), status.online ? PPP.localIP().toString().c_str() : "", status.rssi,
           (WiFi.getMode() & WIFI_AP) ? "true" : "false", jsonEscape(settings.apSsid).c_str(), clientsConnected(),
           settings.reportUrl.isEmpty() ? "false" : "true", status.lastHttpCode, reportAge);
  return String(buf);
}

// GPSLogger (Home Assistant) nimmt nur bekannte Felder an und verwirft den
// Rest. Netz-/Akkubetrieb steckt deshalb in "activity".
static String gpsloggerBody() {
  const GnssFix &f = status.fix;
  char buf[400];
  snprintf(buf, sizeof(buf),
           "device=%s&latitude=%.6f&longitude=%.6f&accuracy=%.0f&altitude=%.1f&speed=%.2f"
           "&direction=%.0f&battery=%d&provider=%s&activity=%s",
           urlEncode(settings.deviceName).c_str(), f.lat, f.lon, f.accuracyM(), f.altitude, f.speedKmh / 3.6f,
           f.course, status.power.batteryPercent, status.hasFix ? "gps" : "letzte_position",
           status.power.externalPower ? "netzbetrieb" : "akkubetrieb");
  return buf;
}

bool reportSend() {
  if (settings.reportUrl.isEmpty() || !status.online) return false;
  // GPSLogger braucht eine Position, JSON geht auch ohne.
  if (settings.reportFormat == REPORT_GPSLOGGER && !status.fix.valid) {
    Serial.println("Noch keine Position, nichts gemeldet");
    return false;
  }

  NetworkClientSecure tls;
  NetworkClient plain;
  bool https = settings.reportUrl.startsWith("https");
  // Verschlüsselt, aber ohne Prüfung des Serverzertifikats: Eine CA-Liste
  // für beliebige Ziele passt nicht in den Flash.
  if (https) tls.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(https ? (NetworkClient &)tls : plain, settings.reportUrl)) {
    status.lastHttpCode = -1;
    return false;
  }
  String body;
  if (settings.reportFormat == REPORT_JSON) {
    http.addHeader("Content-Type", "application/json");
    body = statusJson();
  } else {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    body = gpsloggerBody();
  }
  status.lastHttpCode = http.POST(body);
  http.end();

  Serial.printf("Meldung: HTTP %d\n", status.lastHttpCode);
  if (status.lastHttpCode >= 200 && status.lastHttpCode < 300) {
    status.lastReportOkAt = millis();
    return true;
  }
  return false;
}
