#include "Clients.h"

#include "Settings.h"

struct ApClient {
  char mac[18] = "";
  IPAddress ip;
  bool connected = false;
  uint32_t changedAt = 0;  // millis() des letzten Verbindens/Trennens
};

static const int MAX_CLIENTS = 10;
static ApClient clients[MAX_CLIENTS];

static String macStr(const uint8_t *m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return buf;
}

// Eintrag zur MAC, sonst den am längsten getrennten überschreiben.
static ApClient &slotFor(const String &mac) {
  ApClient *oldest = nullptr;
  for (auto &c : clients) {
    if (mac == c.mac) return c;
  }
  for (auto &c : clients) {
    if (!c.mac[0]) return c;
    if (!c.connected && (!oldest || c.changedAt < oldest->changedAt)) oldest = &c;
  }
  return oldest ? *oldest : clients[MAX_CLIENTS - 1];
}

void clientsOnEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      String mac = macStr(info.wifi_ap_staconnected.mac);
      ApClient &c = slotFor(mac);
      strlcpy(c.mac, mac.c_str(), sizeof(c.mac));
      c.connected = true;
      c.changedAt = millis();
      Serial.printf("Hotspot: %s verbunden\n", c.mac);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      String mac = macStr(info.wifi_ap_stadisconnected.mac);
      ApClient &c = slotFor(mac);
      strlcpy(c.mac, mac.c_str(), sizeof(c.mac));
      c.connected = false;
      c.changedAt = millis();
      Serial.printf("Hotspot: %s getrennt\n", c.mac);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      String mac = macStr(info.wifi_ap_staipassigned.mac);
      ApClient &c = slotFor(mac);
      strlcpy(c.mac, mac.c_str(), sizeof(c.mac));
      c.ip = IPAddress(info.wifi_ap_staipassigned.ip.addr);
      break;
    }
    default:
      break;
  }
}

int clientsConnected() {
  int n = 0;
  for (auto &c : clients) n += c.connected;
  return n;
}

bool clientIsConnected(const String &mac, uint32_t *goneSince) {
  for (auto &c : clients) {
    if (mac == c.mac) {
      if (goneSince) *goneSince = c.connected ? 0 : c.changedAt;
      return c.connected;
    }
  }
  if (goneSince) *goneSince = 0;
  return false;
}

String clientMacForIp(IPAddress ip) {
  for (auto &c : clients) {
    if (c.connected && c.ip == ip) return c.mac;
  }
  return String();
}

String clientsJson(IPAddress requester) {
  String out = "[";
  bool first = true;
  uint32_t now = millis();
  for (auto &c : clients) {
    if (!c.mac[0]) continue;
    char buf[200];
    snprintf(buf, sizeof(buf),
             "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"connected\":%s,\"seconds\":%lu,\"you\":%s,\"trigger\":%s}",
             first ? "" : ",", c.mac, c.ip ? c.ip.toString().c_str() : "", c.connected ? "true" : "false",
             (unsigned long)((now - c.changedAt) / 1000), (c.connected && c.ip == requester) ? "true" : "false",
             settings.triggerMac == c.mac ? "true" : "false");
    out += buf;
    first = false;
  }
  return out + "]";
}
