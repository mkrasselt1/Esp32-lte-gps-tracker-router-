# ESP32 LTE-Router mit GPS-Tracker

Firmware für LilyGO-Boards mit ESP32 und LTE-Modem (T-SIM7600E/G, T-A7670E/G).
Das Board wird damit zu einem kleinen mobilen Router mit Ortung:

- **WLAN-Hotspot** über die Mobilfunkverbindung (NAT) für ein paar Geräte.
- **Positionsmeldungen** aus dem GNSS des Modems an eine beliebige URL, zum
  Beispiel an die [GPSLogger-Integration](https://www.home-assistant.io/integrations/gpslogger/)
  von Home Assistant oder als JSON an einen eigenen Server.
- **Strom und Akku**: ob das Board am Netz hängt oder vom Akku läuft, und
  wie voll der Akku ist.
- **Energiesparen auf Akku** in Stufen: Hotspot aus, nur noch Tracker,
  schließlich Tiefschlaf mit gelegentlichen Meldungen.
- **Weboberfläche mit Login** für alle Einstellungen: APN samt Benutzer und
  Passwort, SIM-PIN, WLAN, Meldeziel und die Regeln fürs Energiesparen.

## Inbetriebnahme

1. In `include/config.h` das Board wählen: `BOARD_T_SIM7600` (voreingestellt)
   oder `BOARD_T_A7670`.
2. Beim T-SIM7600 die DIP-Schalter stellen: P1 und P2 auf ON (Modem-UART am
   ESP32), P4 auf OFF (der ESP32 schaltet das Modem ein).
3. Bauen und flashen:

   ```sh
   pio run -t upload
   pio device monitor
   ```

4. Mit dem WLAN **LTE-Router-XXXX** verbinden, Passwort **lte-router**.
5. `http://192.168.4.1/` öffnen. Beim ersten Aufruf erscheint die
   Einrichtung: Admin-Passwort festlegen, WLAN-Passwort ändern, APN und
   SIM-PIN eintragen. Danach startet das Gerät neu.
6. Unter **Einstellungen** (Benutzer `admin`) den Rest einstellen.

Gebaut wird mit Arduino-Core 3.x über [pioarduino](https://github.com/pioarduino/platform-espressif32).
Das offizielle `espressif32`-Paket von PlatformIO liefert nur Core 2.x, dem
die PPP- und NAT-Unterstützung fehlt.

## Weboberfläche

| Seite | Inhalt |
|---|---|
| `/` | Status: Betriebsstufe, Position, Höhe, Genauigkeit, Tempo und Richtung, Strom, Akku, Mobilfunk, WLAN, letzte Meldung |
| `/settings` | alle Einstellungen, Liste der Geräte im WLAN, Werkseinstellungen |
| `/api/status` | Status als JSON |

Die Anmeldung läuft per HTTP-Digest (Benutzer `admin`), das Passwort geht
also nicht im Klartext übers WLAN.

**Passwort vergessen?** Per USB den seriellen Monitor öffnen (115200 Baud)
und `factory-reset` eingeben. Danach gelten wieder die Werkseinstellungen.

## Mobilfunk

- **SIM-PIN**: Die Firmware gibt die PIN genau einmal pro Start ein. Ist sie
  falsch, versucht sie es nicht erneut, damit die SIM nicht gesperrt wird.
  Die Statusseite zeigt dann „SIM-PIN falsch oder fehlt“. Neue PIN eintragen
  und speichern.
- **APN-Benutzer und -Passwort** werden per PAP übergeben und zusätzlich im
  Modem hinterlegt (`AT+CGAUTH`), damit es auch bei Netzen mit CHAP klappt.

## Positionsmeldungen

**Home Assistant GPSLogger:** Unter *Einstellungen → Geräte & Dienste →
Integration hinzufügen → GPSLogger* die Webhook-URL holen und als Ziel-URL
eintragen. Der Gerätename bestimmt den `device_tracker`.

| Attribut | Inhalt |
|---|---|
| Position | Breite und Länge, `gps_accuracy` grob aus HDOP geschätzt |
| `altitude` | Höhe in Metern |
| `speed`, `direction` | Tempo in m/s, Richtung in Grad (nur in Fahrt brauchbar) |
| `battery` | Akkustand in Prozent |
| `activity` | `netzbetrieb` oder `akkubetrieb` |
| `provider` | `gps`, oder `letzte_position` ohne aktuellen Fix |

GPSLogger verwirft Felder, die es nicht kennt. Der Strom-Status steckt
deshalb in `activity`:

```yaml
template:
  - binary_sensor:
      - name: LTE-Router am Netz
        device_class: plug
        state: "{{ state_attr('device_tracker.lte_router', 'activity') == 'netzbetrieb' }}"
```

**JSON:** POST mit demselben Inhalt wie `/api/status`.

HTTPS-Ziele werden verschlüsselt angesprochen, das Serverzertifikat wird aber
nicht geprüft.

## Energiesparen

Am Netz läuft immer alles. Auf Akku geht es in Stufen weiter, alle Zeiten
sind in der Weboberfläche einstellbar (0 schaltet eine Regel ab):

1. **Akkubetrieb**: Der Wechsel wird sofort gemeldet. Der Hotspot bleibt an,
   bis das Erste davon eintritt:
   - die Höchstzeit auf Akku ist um (Standard 45 min),
   - so lange war kein Gerät im WLAN (Standard 10 min),
   - das gewählte Auslöser-Gerät hat sich getrennt und ist so lange
     weggeblieben (Standard 2 min). Gewählt wird es unter *Einstellungen →
     Geräte im WLAN*; das Gerät, mit dem man die Seite gerade aufruft, ist
     markiert.
2. **Sparmodus**: WLAN aus, LTE und GPS an. Meldungen im Stand alle 15 min,
   in Fahrt jede Minute.
3. **Tiefschlaf** nach so langem Stillstand im Sparmodus (Standard 60 min)
   oder sofort, wenn der Akku unter den Mindeststand fällt (Standard 15 %).
   Modem aus, ESP32 im Tiefschlaf.
   - Alle 10 min wacht der ESP32 für Sekundenbruchteile auf und prüft, ob
     wieder Netzstrom da ist. Wenn ja, startet alles normal.
   - Alle 120 min schaltet er das Modem ein, wartet auf eine Position,
     meldet und schläft weiter. Bewegt sich das Gerät dabei, bleibt es wach
     im Sparmodus.

Ohne Bewegungssensor merkt das Board im Tiefschlaf erst bei der nächsten
Meldung, dass es bewegt wurde.

### Laufzeit auf Akku (geschätzt, nicht gemessen)

Mit einer 18650-Zelle von etwa 3000 mAh, davon rund 2400 mAh nutzbar:

| Zustand | Strom | Laufzeit |
|---|---|---|
| Hotspot, 1–2 Geräte surfen | 300–450 mA | 5–8 h |
| Hotspot an, niemand verbunden | ca. 200 mA | ca. 12 h |
| Sparmodus | ca. 100 mA | ca. 1 Tag |
| Tiefschlaf, Meldung alle 2 h | ca. 5–10 mA | 1–2 Wochen |

Eine Marken-Zelle mit hoher Strombelastbarkeit verwenden, das Modem zieht
beim Senden kurz bis zu 2 A. Lithium-Zellen dürfen unter 0 °C nicht geladen
werden.

## Netzbetrieb erkennen

**T-SIM7600:** Ohne Zusatzteile. Sobald Strom über USB-C oder den VBUS-Pin
kommt, trennt das Board den Akku-Spannungsteiler an GPIO35 ab und der ADC
liest 0. Die Akkuspannung kommt in dieser Zeit vom Modem (`AT+CBC`); weil der
Akku dabei lädt, liegt der Prozentwert zu hoch.

Externe Versorgung nur über USB-C oder VBUS, **höchstens 5 V**, mit
mindestens 2 A. Der Solar-Eingang lädt nur den Akku. Laut LilyGO kann das
Board beim Umschalten zwischen Netz und Akku neu starten; die Firmware meldet
nach jedem Start sofort.

**T-A7670 und andere:** Ohne Zusatz wird aus der Akkuspannung geschätzt
(über 4,18 V gilt als „lädt“), das ist ungenau. Zuverlässig wird es mit zwei
Widerständen: vom 5-V-Eingang über 100 kΩ auf GPIO34, von dort 100 kΩ nach
GND, und in `config.h` `PIN_POWER_SENSE` auf 34 setzen.

## Wie es funktioniert

- Eine eigene FreeRTOS-Task kümmert sich ums Modem: einschalten, SIM
  entsperren, GNSS an, Mobilfunk per PPP. So bleibt die Weboberfläche
  erreichbar, auch wenn das Modem hängt oder die PIN falsch ist.
- PPP läuft im **CMUX**-Modus: Datenverbindung und AT-Befehle teilen sich den
  UART. So lässt sich `AT+CGNSSINFO` abfragen, während der Hotspot online ist.
- Der Hotspot leitet per NAPT ins Mobilnetz.
- Ist die Mobilfunkverbindung länger als 5 min weg, startet das Board neu.

## Stand

Die Firmware baut für beide Boards. Der GNSS-Parser ist gegen
Beispielantworten von SIM7600 und A7670 geprüft, die Weboberfläche gegen eine
nachgebaute API im Browser. **Auf echter Hardware lief sie noch nicht.**
Am ehesten hakt es bei:

- **Pins**: Sie stammen aus LilyGOs Beispielen. Bei Schweigen des Modems
  zuerst die DIP-Schalter prüfen, dann TX/RX in `config.h` tauschen.
- **CMUX auf dem A7670**: esp-modem unterstützt CMUX für den SIM7600, beim
  A7670 wird derselbe Treiber verwendet.
- **Tiefschlaf**: Ruhestrom des Boards und sicheres Abschalten des Modems
  sind ungemessen.
