#include "WebUi.h"

#include <WebServer.h>
#include <WiFi.h>

#include "Clients.h"
#include "Report.h"
#include "Settings.h"
#include "Status.h"

static WebServer server(80);
static uint32_t restartAt = 0;
static bool started = false;
static const char *REALM = "LTE-Router";

// ---------------------------------------------------------------------------
// Seiten
// ---------------------------------------------------------------------------

#define PAGE_HEAD(title)                                                                                    \
  "<!doctype html><html lang=de><head><meta charset=utf-8>"                                                 \
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>" title "</title><style>"        \
  ":root{--fg:#1d1d1f;--mut:#6e6e73;--bg:#fff;--card:#f5f5f7;--acc:#0a66c2;--bad:#c62828;--ok:#2e7d32}"     \
  "@media(prefers-color-scheme:dark){:root{--fg:#f5f5f7;--mut:#a1a1a6;--bg:#111;--card:#1c1c1e;"            \
  "--acc:#4a9eff;--bad:#ff6b6b;--ok:#66bb6a}}"                                                              \
  "body{font:16px/1.4 system-ui,sans-serif;color:var(--fg);background:var(--bg);margin:0 auto;"             \
  "max-width:560px;padding:16px}h1{font-size:1.4em;margin:0 0 12px}h2{font-size:1.05em;margin:20px 0 8px}"  \
  "nav a{margin-right:16px;color:var(--acc)}section{background:var(--card);border-radius:10px;"              \
  "padding:12px 14px;margin:12px 0}table{width:100%;border-collapse:collapse}td{padding:4px 0;"             \
  "vertical-align:top}td:first-child{color:var(--mut);width:42%}label{display:block;margin:10px 0 2px;"     \
  "color:var(--mut);font-size:.9em}input,select{width:100%;box-sizing:border-box;padding:8px;font:inherit;" \
  "border:1px solid #8884;border-radius:6px;background:var(--bg);color:var(--fg)}"                          \
  "button{font:inherit;padding:10px 16px;border:0;border-radius:6px;background:var(--acc);color:#fff;"      \
  "margin-top:14px}button.small{padding:4px 10px;margin:0;font-size:.85em}small{color:var(--mut)}"          \
  ".bad{color:var(--bad)}.ok{color:var(--ok)}a{color:var(--acc)}"                                           \
  "</style></head><body>"

static const char STATUS_HTML[] PROGMEM = PAGE_HEAD("LTE-Router") R"HTML(
<h1 id=name>LTE-Router</h1><nav><a href="/">Status</a><a href="/settings">Einstellungen</a></nav>
<section><table id=t><tr><td>Lade …</td></tr></table></section>
<script>
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const age=s=>s<0?'nie':s<90?'vor '+s+' s':s<5400?'vor '+Math.round(s/60)+' min':'vor '+Math.round(s/3600)+' h';
async function upd(){try{const s=await(await fetch('/api/status')).json();const g=s.gps,p=s.power,m=s.mobile;
document.getElementById('name').textContent=s.device;
const pos=g.valid?`<a href="https://www.openstreetmap.org/?mlat=${g.latitude}&mlon=${g.longitude}#map=16/${g.latitude}/${g.longitude}">${g.latitude.toFixed(5)}, ${g.longitude.toFixed(5)}</a>`
 +(g.fix?'':` <small>(letzte, ${age(g.age_s)})</small>`):'<span class=bad>noch kein GPS-Fix</span>';
const rows=[['Betrieb',esc(s.mode.name)+(s.mode.reason?` <small>${esc(s.mode.reason)}</small>`:'')],
['Position',pos],['Höhe',g.valid?g.altitude_m.toFixed(0)+' m':'–'],
['Genauigkeit',g.valid?'± '+g.accuracy_m+' m <small>('+g.satellites+' Satelliten)</small>':'–'],
['Tempo / Richtung',g.valid?g.speed_kmh.toFixed(1)+' km/h'+(g.speed_kmh>=3?', '+g.course+'°':''):'–'],
['Strom',(p.external?'Netz':'Akku')+(p.sensed?'':' <small>(geschätzt)</small>')],
['Akku',p.battery_percent+' % <small>'+(p.battery_mv/1000).toFixed(2)+' V'+(p.charging?', lädt':'')+'</small>'],
['Mobilfunk',`<span class=${m.connected?'ok':'bad'}>${esc(m.state)}</span>`+(m.rssi<99?` <small>Signal ${m.rssi}/31</small>`:'')],
['WLAN',s.hotspot.active?esc(s.hotspot.ssid)+', '+s.hotspot.clients+' Gerät(e)':'aus'],
['Meldungen',!s.report.enabled?'aus':s.report.last_ok_s_ago<0?'noch keine'+(s.report.last_http_code?' <small class=bad>(HTTP '+s.report.last_http_code+')</small>':''):age(s.report.last_ok_s_ago)]];
document.getElementById('t').innerHTML=rows.map(r=>`<tr><td>${r[0]}</td><td>${r[1]}</td></tr>`).join('');}catch(e){}}
upd();setInterval(upd,5000);
</script></body></html>)HTML";

static const char SETTINGS_HTML[] PROGMEM = PAGE_HEAD("Einstellungen") R"HTML(
<h1>Einstellungen</h1><nav><a href="/">Status</a><a href="/settings">Einstellungen</a></nav>
<form id=f>
<section><h2>Mobilfunk</h2>
<label>APN</label><input name=apn required>
<label>APN-Benutzer <small>(meist leer)</small></label><input name=apnUser autocomplete=off>
<label>APN-Passwort <small>(leer lassen = unverändert)</small></label><input name=apnPass type=password autocomplete=new-password>
<label><input type=checkbox name=apnPassClear value=1 style="width:auto"> APN-Passwort löschen</label>
<label>SIM-PIN <small>(leer lassen = unverändert)</small></label><input name=simPin type=password inputmode=numeric autocomplete=off>
<label><input type=checkbox name=simPinClear value=1 style="width:auto"> SIM hat keine PIN</label>
<small id=pinInfo></small>
</section>
<section><h2>WLAN-Hotspot</h2>
<label>Name (SSID)</label><input name=apSsid required maxlength=32>
<label>Passwort <small>(mind. 8 Zeichen, leer lassen = unverändert)</small></label><input name=apPass type=password minlength=8 autocomplete=new-password>
<label>Höchstzahl Geräte</label><input name=apMaxClients type=number min=1 max=8>
<label>Kanal</label><input name=apChannel type=number min=1 max=13>
</section>
<section><h2>Positionsmeldungen</h2>
<label>Ziel-URL <small>(leer = keine Meldungen)</small></label><input name=reportUrl type=url placeholder="https://…/api/webhook/…">
<label>Format</label><select name=reportFormat><option value=0>Home Assistant GPSLogger</option><option value=1>JSON</option></select>
<label>Gerätename</label><input name=deviceName required>
<label>Intervall in Fahrt (Sekunden)</label><input name=reportMovingS type=number min=10 max=3600>
<label>Intervall im Stand (Sekunden)</label><input name=reportParkedS type=number min=30 max=86400>
</section>
<section><h2>Energiesparen auf Akku</h2>
<small>Minuten; 0 schaltet die jeweilige Regel ab. Am Netz läuft immer alles.</small>
<label>Hotspot höchstens so lange auf Akku</label><input name=hotspotMaxBatteryMin type=number min=0 max=1440>
<label>Hotspot aus, wenn so lange kein Gerät verbunden</label><input name=hotspotIdleMin type=number min=0 max=1440>
<label>Hotspot aus, wenn dieses Gerät geht</label><select name=triggerMac id=trig></select>
<label>… und so lange wegbleibt</label><input name=triggerGraceMin type=number min=0 max=60>
<label>Sparmodus: Meldung in Fahrt alle (Sekunden)</label><input name=trackerMovingS type=number min=10 max=3600>
<label>Sparmodus: Meldung im Stand alle (Minuten)</label><input name=trackerParkedMin type=number min=1 max=1440>
<label>Tiefschlaf nach Stillstand von</label><input name=sleepAfterParkedMin type=number min=0 max=10080>
<label>Tiefschlaf: Position melden alle</label><input name=sleepReportMin type=number min=10 max=10080>
<label>Sofort Tiefschlaf unter Akkustand (%)</label><input name=lowBatteryPercent type=number min=0 max=90>
</section>
<section><h2>Display</h2>
<small>Optionales 128×64-OLED an I²C (SDA 21, SCL 22). Bild verschoben oder Streifen am Rand: anderen Controller wählen.</small>
<label>Controller</label><select name=displayType><option value=0>kein Display</option><option value=1>SSD1306 (meist 0,96")</option><option value=2>SH1106 (meist 1,3")</option></select>
<input type=hidden name=displayForm value=1>
<label><input type=checkbox name=displayFlip value=1 style="width:auto"> um 180° drehen</label>
<label><input type=checkbox name=displayWifi value=1 style="width:auto"> WLAN-Name, Passwort und QR-Code anzeigen <small>(für jeden lesbar, der aufs Display schaut)</small></label>
</section>
<section><h2>Zugang</h2>
<label>Neues Admin-Passwort <small>(leer lassen = unverändert)</small></label><input name=adminPass type=password minlength=8 autocomplete=new-password>
</section>
<button>Speichern und neu starten</button> <small id=msg></small>
</form>
<section><h2>Geräte im WLAN</h2><table id=cl></table></section>
<section><h2>Werkseinstellungen</h2><small>Löscht alle Einstellungen samt Passwörtern. Danach gilt wieder das WLAN-Passwort <b>lte-router</b>.</small><br>
<button type=button id=fr style="background:var(--bad)">Zurücksetzen</button></section>
<script>
const f=document.getElementById('f'),esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let cur={},clients=[];
function fillTrig(){const t=document.getElementById('trig'),v=t.value||cur.triggerMac||'';const macs=new Set(clients.map(c=>c.mac));if(v)macs.add(v);
t.innerHTML='<option value="">— keins —</option>'+[...macs].map(m=>{const c=clients.find(x=>x.mac==m);
return `<option value="${m}">${m}${c&&c.you?' (dieses Gerät)':''}${c?(c.connected?'':' (getrennt)'):' (nicht gesehen)'}</option>`}).join('');t.value=v;}
async function load(){cur=await(await fetch('/api/settings')).json();
for(const[k,v]of Object.entries(cur)){const e=f.elements[k];if(!e||e.type=='password')continue;if(e.type=='checkbox')e.checked=!!v;else e.value=v;}
document.getElementById('pinInfo').textContent=cur.simPinSet?'Eine PIN ist hinterlegt.':'Keine PIN hinterlegt.';await cl();}
async function cl(){clients=await(await fetch('/api/clients')).json();fillTrig();
document.getElementById('cl').innerHTML=clients.length?clients.map(c=>`<tr><td>${c.mac}${c.you?'<br><small>dieses Gerät</small>':''}</td><td>${c.connected?'verbunden, '+esc(c.ip):'getrennt'}`
+` <button type=button class=small onclick="document.getElementById('trig').value='${c.mac}'">als Auslöser</button></td></tr>`).join(''):'<tr><td>keine</td></tr>';}
f.onsubmit=async e=>{e.preventDefault();const m=document.getElementById('msg');m.textContent='Speichere …';
const r=await fetch('/api/settings',{method:'POST',body:new URLSearchParams(new FormData(f))});const j=await r.json();
m.textContent=j.ok?'Gespeichert, startet neu …':(j.error||'Fehler');m.className=j.ok?'ok':'bad';};
document.getElementById('fr').onclick=async()=>{if(!confirm('Wirklich alle Einstellungen löschen?'))return;
await fetch('/api/factory-reset',{method:'POST'});document.body.innerHTML='<p>Zurückgesetzt, startet neu.</p>';};
load();setInterval(cl,10000);
</script></body></html>)HTML";

static const char SETUP_HTML[] PROGMEM = PAGE_HEAD("Einrichtung") R"HTML(
<h1>Einrichtung</h1>
<p>Willkommen. Lege zuerst ein Passwort für diese Oberfläche fest (Benutzer <b>admin</b>) und trage die Daten deiner SIM-Karte ein. Alles Weitere unter <i>Einstellungen</i>.</p>
<form id=f><section>
<label>Admin-Passwort (mind. 8 Zeichen)</label><input name=adminPass type=password minlength=8 required autocomplete=new-password>
<label>Admin-Passwort wiederholen</label><input name=adminPass2 type=password minlength=8 required autocomplete=new-password>
<label>Neues WLAN-Passwort <small>(empfohlen, mind. 8 Zeichen)</small></label><input name=apPass type=password minlength=8 autocomplete=new-password>
<label>APN</label><input name=apn value="internet" required>
<label>SIM-PIN <small>(leer, wenn die SIM keine hat)</small></label><input name=simPin type=password inputmode=numeric autocomplete=off>
</section><button>Speichern und neu starten</button> <small id=msg></small></form>
<script>
const f=document.getElementById('f');f.onsubmit=async e=>{e.preventDefault();const m=document.getElementById('msg');
if(f.adminPass.value!=f.adminPass2.value){m.textContent='Passwörter stimmen nicht überein';m.className='bad';return;}
const r=await fetch('/setup',{method:'POST',body:new URLSearchParams(new FormData(f))});const j=await r.json();
m.textContent=j.ok?'Gespeichert. Startet neu – danach mit dem neuen WLAN-Passwort verbinden und als admin anmelden.':(j.error||'Fehler');m.className=j.ok?'ok':'bad';};
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Hilfen
// ---------------------------------------------------------------------------

static bool authorized() {
  if (!settingsConfigured()) {
    server.sendHeader("Location", "/setup");
    server.send(302, "text/plain", "");
    return false;
  }
  if (!server.authenticate("admin", settings.adminPass.c_str())) {
    server.requestAuthentication(DIGEST_AUTH, REALM, "Anmeldung erforderlich");
    return false;
  }
  return true;
}

static void sendJson(const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void fail(const char *msg) { server.send(400, "application/json", String("{\"ok\":false,\"error\":\"") + msg + "\"}"); }

// Anführungszeichen und Steuerzeichen raus: Die Werte landen in AT-Befehlen
// und JSON.
static String clean(const String &s) {
  String out;
  for (char c : s) {
    if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) out += c;
  }
  out.trim();
  return out;
}

static bool argNum(const char *name, long lo, long hi, long &out) {
  if (!server.hasArg(name)) return false;
  out = constrain(server.arg(name).toInt(), lo, hi);
  return true;
}

#define NUM(field, lo, hi)                                    \
  do {                                                        \
    long v;                                                   \
    if (argNum(#field, lo, hi, v)) settings.field = (decltype(settings.field))v; \
  } while (0)

#define STR(field)                                          \
  do {                                                      \
    if (server.hasArg(#field)) settings.field = clean(server.arg(#field)); \
  } while (0)

static void scheduleRestart() { restartAt = millis() + 1500; }

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------

static void handleSettingsGet() {
  if (!authorized()) return;
  const Settings &s = settings;
  char buf[1024];
  auto e = [](const String &v) {
    String o;
    for (char c : v) {
      if (c == '"' || c == '\\') o += '\\';
      o += c;
    }
    return o;
  };
  snprintf(buf, sizeof(buf),
           "{\"apSsid\":\"%s\",\"apMaxClients\":%u,\"apChannel\":%u,\"apn\":\"%s\",\"apnUser\":\"%s\","
           "\"simPinSet\":%s,\"reportUrl\":\"%s\",\"deviceName\":\"%s\",\"reportFormat\":%u,\"reportMovingS\":%u,"
           "\"reportParkedS\":%u,\"hotspotMaxBatteryMin\":%u,\"hotspotIdleMin\":%u,\"triggerMac\":\"%s\","
           "\"triggerGraceMin\":%u,\"trackerMovingS\":%u,\"trackerParkedMin\":%u,\"sleepAfterParkedMin\":%u,"
           "\"sleepReportMin\":%u,\"lowBatteryPercent\":%u,\"displayType\":%u,\"displayFlip\":%s,"
           "\"displayWifi\":%s}",
           e(s.apSsid).c_str(), s.apMaxClients, s.apChannel, e(s.apn).c_str(), e(s.apnUser).c_str(),
           s.simPin.isEmpty() ? "false" : "true", e(s.reportUrl).c_str(), e(s.deviceName).c_str(), s.reportFormat,
           s.reportMovingS, s.reportParkedS, s.hotspotMaxBatteryMin, s.hotspotIdleMin, s.triggerMac.c_str(),
           s.triggerGraceMin, s.trackerMovingS, s.trackerParkedMin, s.sleepAfterParkedMin, s.sleepReportMin,
           s.lowBatteryPercent, s.displayType, s.displayFlip ? "true" : "false", s.displayWifi ? "true" : "false");
  sendJson(buf);
}

static void handleSettingsPost() {
  if (!authorized()) return;
  String apPass = server.arg("apPass");
  if (!apPass.isEmpty() && apPass.length() < 8) return fail("WLAN-Passwort braucht mindestens 8 Zeichen");
  String adminPass = server.arg("adminPass");
  if (!adminPass.isEmpty() && adminPass.length() < 8) return fail("Admin-Passwort braucht mindestens 8 Zeichen");
  if (server.hasArg("apSsid") && clean(server.arg("apSsid")).isEmpty()) return fail("WLAN-Name fehlt");
  String url = clean(server.arg("reportUrl"));
  if (!url.isEmpty() && !url.startsWith("http://") && !url.startsWith("https://")) {
    return fail("Ziel-URL muss mit http:// oder https:// beginnen");
  }

  STR(apSsid);
  if (!apPass.isEmpty()) settings.apPass = clean(apPass);
  NUM(apMaxClients, 1, 8);
  NUM(apChannel, 1, 13);
  STR(apn);
  STR(apnUser);
  if (server.hasArg("apnPassClear")) settings.apnPass = "";
  else if (!server.arg("apnPass").isEmpty()) settings.apnPass = clean(server.arg("apnPass"));
  if (server.hasArg("simPinClear")) settings.simPin = "";
  else if (!server.arg("simPin").isEmpty()) settings.simPin = clean(server.arg("simPin"));
  settings.reportUrl = url;
  STR(deviceName);
  NUM(reportFormat, 0, 1);
  NUM(reportMovingS, 10, 3600);
  NUM(reportParkedS, 30, 65535);
  NUM(hotspotMaxBatteryMin, 0, 1440);
  NUM(hotspotIdleMin, 0, 1440);
  if (server.hasArg("triggerMac")) settings.triggerMac = normalizeMac(server.arg("triggerMac"));
  NUM(triggerGraceMin, 0, 60);
  NUM(trackerMovingS, 10, 3600);
  NUM(trackerParkedMin, 1, 1440);
  NUM(sleepAfterParkedMin, 0, 10080);
  NUM(sleepReportMin, 10, 10080);
  NUM(lowBatteryPercent, 0, 90);
  NUM(displayType, 0, 2);
  if (server.hasArg("displayForm")) {
    settings.displayFlip = server.hasArg("displayFlip");
    settings.displayWifi = server.hasArg("displayWifi");
  }
  if (!adminPass.isEmpty()) settings.adminPass = adminPass;

  settingsSave();
  sendJson("{\"ok\":true}");
  scheduleRestart();
}

static void handleSetupPost() {
  if (settingsConfigured()) return fail("Bereits eingerichtet");
  String admin = server.arg("adminPass");
  if (admin.length() < 8) return fail("Admin-Passwort braucht mindestens 8 Zeichen");
  String apPass = server.arg("apPass");
  if (!apPass.isEmpty() && apPass.length() < 8) return fail("WLAN-Passwort braucht mindestens 8 Zeichen");
  if (!apPass.isEmpty()) settings.apPass = clean(apPass);
  STR(apn);
  settings.simPin = clean(server.arg("simPin"));
  settings.adminPass = admin;
  settingsSave();
  sendJson("{\"ok\":true}");
  scheduleRestart();
}

void webStart() {
  if (started) return;
  started = true;
  server.on("/", HTTP_GET, [] {
    if (authorized()) server.send_P(200, "text/html; charset=utf-8", STATUS_HTML);
  });
  server.on("/settings", HTTP_GET, [] {
    if (authorized()) server.send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
  });
  server.on("/setup", HTTP_GET, [] {
    if (settingsConfigured()) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      return;
    }
    server.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
  });
  server.on("/setup", HTTP_POST, handleSetupPost);
  server.on("/api/status", HTTP_GET, [] {
    if (authorized()) sendJson(statusJson());
  });
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/clients", HTTP_GET, [] {
    if (authorized()) sendJson(clientsJson(server.client().remoteIP()));
  });
  server.on("/api/factory-reset", HTTP_POST, [] {
    if (!authorized()) return;
    settingsFactoryReset();
    sendJson("{\"ok\":true}");
    scheduleRestart();
  });
  server.onNotFound([] { server.send(404, "text/plain", "nicht gefunden"); });
  server.begin();
}

void webLoop() {
  if (!started) return;
  server.handleClient();
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    Serial.println("Einstellungen gespeichert, Neustart");
    ESP.restart();
  }
}
