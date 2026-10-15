#include "Display.h"

#include <PPP.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Wire.h>
#include <qrcode.h>

#include "Clients.h"
#include "Settings.h"
#include "Status.h"
#include "config.h"

static U8G2 *oled = nullptr;
static bool on = false;
static uint32_t lastDraw = 0;
static uint32_t pageStart = 0;
static int page = 0;

static bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void displayStart() {
  if (settings.displayType == DISPLAY_OFF) return;
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  uint8_t addr = probe(0x3C) ? 0x3C : probe(0x3D) ? 0x3D : 0;
  if (!addr) {
    Serial.println("Kein Display gefunden");
    return;
  }
  const u8g2_cb_t *rot = settings.displayFlip ? U8G2_R2 : U8G2_R0;
  if (settings.displayType == DISPLAY_SH1106) oled = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(rot);
  else oled = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(rot);
  oled->setI2CAddress(addr * 2);
  oled->begin();
  oled->enableUTF8Print();
  on = true;
  Serial.printf("Display an 0x%02X\n", addr);
}

void displayOff() {
  if (oled && on) oled->setPowerSave(1);
  on = false;
}

// ---------------------------------------------------------------------------
// Zeichenhilfen
// ---------------------------------------------------------------------------

// Akku-Symbol 14x7 mit Füllstand, bei Netzstrom mit Blitz daneben.
static void drawBattery(int x, int y) {
  oled->drawFrame(x, y, 12, 7);
  oled->drawBox(x + 12, y + 2, 2, 3);
  int fill = status.power.batteryPercent * 10 / 100;
  if (fill > 0) oled->drawBox(x + 1, y + 1, fill, 5);
  if (status.power.externalPower) {
    oled->drawLine(x - 4, y, x - 6, y + 3);
    oled->drawLine(x - 6, y + 3, x - 3, y + 3);
    oled->drawLine(x - 3, y + 3, x - 5, y + 6);
  }
}

// Vier Balken nach AT+CSQ (0..31, 99 = unbekannt).
static void drawSignal(int x, int y) {
  int r = status.rssi;
  int bars = r == 99 ? 0 : r >= 20 ? 4 : r >= 15 ? 3 : r >= 10 ? 2 : r >= 5 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    if (i < bars) oled->drawBox(x + i * 3, y + 8 - h, 2, h);
    else oled->drawPixel(x + i * 3, y + 7);
  }
}

static void header() {
  oled->setFont(u8g2_font_6x10_tf);
  String name = settings.deviceName.substring(0, 12);
  oled->drawUTF8(0, 8, name.c_str());
  drawSignal(80, 0);
  char pct[5];
  snprintf(pct, sizeof(pct), "%d", status.power.batteryPercent);
  int right = status.power.externalPower ? 104 : 110;  // Platz für den Blitz
  oled->drawStr(right - oled->getStrWidth(pct), 8, pct);
  drawBattery(113, 1);
  oled->drawHLine(0, 11, 128);
}

// Beschriftung links, Wert ab x=38. Zu lange Werte in kleinerer Schrift,
// notfalls abgeschnitten.
static void line(int row, const char *label, const String &value) {
  const int x = 38, width = 128 - x;
  int y = 22 + row * 11;
  oled->setFont(u8g2_font_6x10_tf);
  oled->drawUTF8(0, y, label);
  String v = value;
  if (oled->getUTF8Width(v.c_str()) > width) oled->setFont(u8g2_font_5x8_tf);
  while (v.length() && oled->getUTF8Width(v.c_str()) > width) {
    v.remove(v.length() - 1);
    // keine halben UTF-8-Zeichen stehen lassen
    while (v.length() && ((uint8_t)v[v.length() - 1] & 0xC0) == 0x80) v.remove(v.length() - 1);
    if (v.length() && (uint8_t)v[v.length() - 1] >= 0xC0) v.remove(v.length() - 1);
  }
  oled->drawUTF8(x, y, v.c_str());
  oled->setFont(u8g2_font_6x10_tf);
}

// ---------------------------------------------------------------------------
// Seiten
// ---------------------------------------------------------------------------

static void pageOverview() {
  header();
  line(0, "Netz", status.online ? String("verbunden") : String(status.modemState));
  String gps = status.hasFix ? String(status.fix.satellites) + " Sat, ±" + String((int)status.fix.accuracyM()) + " m"
                             : status.fix.valid ? String("kein Fix (alt)") : String("sucht …");
  line(1, "GPS", gps);
  line(2, "WLAN", String(clientsConnected()) + " Gerät(e)");
  line(3, "Modus", modeName(status.mode));
}

static void pagePosition() {
  header();
  if (!status.fix.valid) {
    line(1, "", "noch keine Position");
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%.5f", status.fix.lat);
  line(0, "Breite", buf);
  snprintf(buf, sizeof(buf), "%.5f", status.fix.lon);
  line(1, "Länge", buf);
  snprintf(buf, sizeof(buf), "%.0f m", status.fix.altitude);
  line(2, "Höhe", buf);
  if (status.hasFix && status.fix.speedKmh >= MOVING_SPEED_KMH) {
    snprintf(buf, sizeof(buf), "%.0f km/h %.0f°", status.fix.speedKmh, status.fix.course);
  } else {
    snprintf(buf, sizeof(buf), "steht");
  }
  line(3, "Tempo", buf);
}

static int qrOffsetX = 0;
static bool qrDrawn = false;

static void drawQr(esp_qrcode_handle_t qr) {
  int size = esp_qrcode_get_size(qr);
  int scale = 64 / (size + 2);  // mit einem Modul Rand
  if (scale < 1) return;
  int px = (size + 2) * scale;
  int ox = qrOffsetX, oy = (64 - px) / 2;
  oled->setDrawColor(1);
  oled->drawBox(ox, oy, px, px);  // weißer Grund
  oled->setDrawColor(0);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qr, x, y)) oled->drawBox(ox + (x + 1) * scale, oy + (y + 1) * scale, scale, scale);
    }
  }
  oled->setDrawColor(1);
  qrDrawn = scale >= 2;  // kleiner lässt er sich kaum scannen
}

static String qrEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

// WLAN-Zugang: QR-Code zum Scannen mit der Handykamera, daneben im Klartext.
static void pageWifi() {
  String text = "WIFI:T:WPA;S:" + qrEscape(settings.apSsid) + ";P:" + qrEscape(settings.apPass) + ";;";
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = drawQr;
  cfg.max_qrcode_version = 3;  // 29 Module: passt mit 2 px pro Modul auf 64 px
  qrOffsetX = 0;
  qrDrawn = false;
  esp_qrcode_generate(&cfg, text.c_str());
  if (!qrDrawn) oled->clearBuffer();  // zu lang für einen lesbaren Code

  int x = qrDrawn ? 66 : 0;
  oled->setFont(u8g2_font_5x8_tf);
  oled->drawUTF8(x, 8, "WLAN");
  // Lange Namen auf zwei Zeilen verteilen.
  int chars = (128 - x) / 5;
  oled->drawUTF8(x, 19, settings.apSsid.substring(0, chars).c_str());
  oled->drawUTF8(x, 28, settings.apSsid.substring(chars, chars * 2).c_str());
  oled->drawUTF8(x, 41, "Passwort");
  oled->drawUTF8(x, 52, settings.apPass.substring(0, chars).c_str());
  oled->drawUTF8(x, 61, settings.apPass.substring(chars, chars * 2).c_str());
}

// Noch nicht eingerichtet: alles, was man zum Loslegen braucht.
static void pageSetup() {
  oled->setFont(u8g2_font_6x10_tf);
  oled->drawUTF8(0, 9, "Einrichtung:");
  oled->drawUTF8(0, 22, settings.apSsid.c_str());
  oled->drawUTF8(0, 34, ("PW " + settings.apPass).c_str());
  oled->drawUTF8(0, 48, "Browser:");
  oled->drawUTF8(0, 60, "192.168.4.1");
}

void displayLoop(uint32_t now) {
  if (!oled) return;
  bool wanted = status.mode == Mode::Normal || status.mode == Mode::Battery;
  if (!wanted) {
    displayOff();
    return;
  }
  if (!on) {
    oled->setPowerSave(0);
    on = true;
  }
  if (now - lastDraw < 1000) return;
  lastDraw = now;

  int pages = settings.displayWifi ? 3 : 2;
  if (now - pageStart >= DISPLAY_PAGE_MS) {
    pageStart = now;
    page = (page + 1) % pages;
  }
  if (page >= pages) page = 0;

  oled->clearBuffer();
  if (!settingsConfigured()) pageSetup();
  else if (page == 0) pageOverview();
  else if (page == 1) pagePosition();
  else pageWifi();
  oled->sendBuffer();
}
