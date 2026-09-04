/*
  VRC WinForce ESP32 V1.3.3
  Target: classic ESP32 WROOM / NodeMCU / DevKitC, Arduino ESP32 Core 3.x

  Current diagnostic hardware:
  - PWM only
  - Shared 25 kHz PWM output on GPIO4 by default
  - Optional 3-channel PWM support remains prepared on GPIO4/16/17
  - Web PWM direct-test remains available
  - Three display-only tachometer inputs on GPIO25/26/27
  - RPM is telemetry only and never changes PWM/failsafe/calibration

  Wiring, current 3-channel PWM setup:
    GPIO4  -> 1k -> NPN1 base -> Fan1 PWM
    GPIO16 -> 1k -> NPN2 base -> Fan2 PWM
    GPIO17 -> 1k -> NPN3 base -> Fan3 PWM
    All NPN emitters -> GND; collectors -> corresponding PWM input.
    Fan GND, ESP32 GND and 12V supply GND must be common.
    TACH1/2/3 -> GPIO25/26/27, each with its own 10k pull-up to 3.3V.

  UDP commands:
    FAN <percent 0..100> <sequence>
    FANS <fan1%> <fan2%> <fan3%> <sequence>
    FAN1/FAN2/FAN3 <percent> <sequence>
*/


#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>

static const char* FW_VERSION = "1.3.3";
static const char* HOSTNAME = "vrc-winforce";

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------
static const uint8_t FAN_COUNT = 3;
static const uint8_t PWM_PINS[FAN_COUNT]  = {4, 16, 17};

// Three display-only tachometers. They NEVER participate in fan control.
// Typical 4-pin PC fan tach output is open collector.
// Use one external 10k pull-up to 3.3V for EACH tach line.
static const uint8_t TACH_PINS[FAN_COUNT] = {25, 26, 27};
static const uint8_t TACH_PULSES_PER_REV = 2;
static const uint32_t TACH_SAMPLE_MS = 1000;

// Current hardware configuration: three independent PWM channels.
static constexpr bool ENABLE_MULTI_PWM = true;

static const uint32_t PWM_FREQUENCY = 25000;
static const uint8_t PWM_RESOLUTION_BITS = 10;
static const uint16_t PWM_MAX = (1u << PWM_RESOLUTION_BITS) - 1u;


// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------
static const uint16_t COMMAND_PORT = 4210;
static const uint16_t DISCOVERY_PORT = 4211;
static const uint32_t COMMAND_TIMEOUT_MS = 3000;
static const uint32_t ANNOUNCE_INTERVAL_MS = 2000;

Preferences prefs;
WiFiUDP udp;
WebServer web(80);
DNSServer dns;

bool apMode = false;
uint32_t lastCommandMs = 0;
uint32_t lastAnnounceMs = 0;
uint32_t lastSeq = 0;
uint32_t udpRxCount = 0;
uint32_t udpAcceptedCount = 0;
uint32_t udpRejectedCount = 0;
bool udpReady = false;
String lastUdpRaw = "-";
String lastUdpSender = "-";
uint32_t lastUdpRxMs = 0;
uint32_t lastAcceptedMs = 0;
uint32_t webTestUntilMs = 0;

float fanPercent[FAN_COUNT] = {0.0f, 0.0f, 0.0f};

// RPM telemetry only. No control code reads fanRpm[].
volatile uint32_t tachPulseCount[FAN_COUNT] = {0, 0, 0};
portMUX_TYPE tachMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t fanRpm[FAN_COUNT] = {0, 0, 0};
uint32_t lastTachSampleMs = 0;

void IRAM_ATTR tachISR1() {
  portENTER_CRITICAL_ISR(&tachMux);
  tachPulseCount[0]++;
  portEXIT_CRITICAL_ISR(&tachMux);
}

void IRAM_ATTR tachISR2() {
  portENTER_CRITICAL_ISR(&tachMux);
  tachPulseCount[1]++;
  portEXIT_CRITICAL_ISR(&tachMux);
}

void IRAM_ATTR tachISR3() {
  portENTER_CRITICAL_ISR(&tachMux);
  tachPulseCount[2]++;
  portEXIT_CRITICAL_ISR(&tachMux);
}

void updateTachometer() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - lastTachSampleMs;
  if (elapsed < TACH_SAMPLE_MS) return;

  uint32_t pulses[FAN_COUNT] = {0, 0, 0};
  portENTER_CRITICAL(&tachMux);
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    pulses[i] = tachPulseCount[i];
    tachPulseCount[i] = 0;
  }
  portEXIT_CRITICAL(&tachMux);

  lastTachSampleMs = now;

  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    if (elapsed == 0 || TACH_PULSES_PER_REV == 0) {
      fanRpm[i] = 0;
      continue;
    }

    const uint64_t numerator = (uint64_t)pulses[i] * 60000ULL;
    const uint32_t rpm =
        (uint32_t)(numerator / ((uint64_t)TACH_PULSES_PER_REV * elapsed));

    // Noise / impossible-value guard. DISPLAY ONLY.
    fanRpm[i] = (rpm <= 12000U) ? rpm : 0U;
  }
}


String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

float clampPercent(float percent) {
  if (!isfinite(percent)) return 0.0f;
  if (percent < 0.0f) return 0.0f;
  if (percent > 100.0f) return 100.0f;
  return percent;
}

void setFanPercent(uint8_t fanIndex, float percent) {
  if (fanIndex >= FAN_COUNT) return;
  percent = clampPercent(percent);

  if (!ENABLE_MULTI_PWM) {
    // Current V1 hardware: all three fan PWM wires share the one GPIO4/NPN
    // signal. Any logical channel command therefore becomes one common value.
    for (uint8_t i = 0; i < FAN_COUNT; ++i) fanPercent[i] = percent;
    const uint16_t requested = (uint16_t)lroundf((percent / 100.0f) * PWM_MAX);
    ledcWrite(PWM_PINS[0], PWM_MAX - requested);
    return;
  }

  fanPercent[fanIndex] = percent;
  const uint16_t requested = (uint16_t)lroundf((percent / 100.0f) * PWM_MAX);
  ledcWrite(PWM_PINS[fanIndex], PWM_MAX - requested);
}

void setAllFansPercent(float percent) {
  percent = clampPercent(percent);
  for (uint8_t i = 0; i < FAN_COUNT; ++i) setFanPercent(i, percent);
}

void setFansPercent(float p1, float p2, float p3) {
  if (!ENABLE_MULTI_PWM) {
    // Robust fallback: if a future PC sends FANS to today's one-PWM hardware,
    // the first value is used as the common master signal.
    setAllFansPercent(p1);
    return;
  }
  setFanPercent(0, p1);
  setFanPercent(1, p2);
  setFanPercent(2, p3);
}

uint16_t currentPwmValue(uint8_t fanIndex) {
  if (fanIndex >= FAN_COUNT) return 0;
  return (uint16_t)lroundf((fanPercent[fanIndex] / 100.0f) * PWM_MAX);
}

float legacyFanPercent() {
  // Existing PC software expects one "fan" field. If individual control is used,
  // report the average while preserving exact legacy behavior when all are equal.
  return (fanPercent[0] + fanPercent[1] + fanPercent[2]) / 3.0f;
}

String statusJson(const char* type = "winforce_status") {
  String s = "{";
  s.reserve(420);
  s += "\"type\":\""; s += type; s += "\",";
  s += "\"name\":\"VRC-WinForce-ESP32\",";
  s += "\"fw\":\""; s += FW_VERSION; s += "\",";
  s += "\"ip\":\""; s += (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()); s += "\",";

  // Legacy compatibility
  s += "\"fan\":"; s += String(legacyFanPercent(), 2); s += ",";
  s += "\"pwm\":"; s += String(currentPwmValue(0)); s += ",";
  s += "\"multi_pwm_enabled\":"; s += (ENABLE_MULTI_PWM ? "true" : "false"); s += ",";

  // Multi-fan PWM telemetry
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    s += "\"fan"; s += String(i + 1); s += "\":"; s += String(fanPercent[i], 2); s += ",";
    s += "\"pwm"; s += String(i + 1); s += "\":"; s += String(currentPwmValue(i)); s += ",";
  }

  s += "\"frequency\":"; s += String(PWM_FREQUENCY); s += ",";
  s += "\"seq\":"; s += String(lastSeq); s += ",";
  s += "\"rssi\":"; s += String(apMode ? 0 : WiFi.RSSI()); s += ",";
  // Legacy compatibility: "rpm" remains fan 1.
  s += "\"rpm\":"; s += String(fanRpm[0]); s += ",";
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    s += "\"rpm"; s += String(i + 1); s += "\":"; s += String(fanRpm[i]); s += ",";
  }
  s += "\"ota_arduino\":true,";
  s += "\"ota_http\":true,";
  s += "\"udp_ready\":"; s += (udpReady ? "true" : "false"); s += ",";
  s += "\"udp_rx\":"; s += String(udpRxCount); s += ",";
  s += "\"udp_accepted\":"; s += String(udpAcceptedCount); s += ",";
  s += "\"udp_rejected\":"; s += String(udpRejectedCount); s += ",";
  s += "\"uptime_ms\":"; s += String(millis());
  s += "}";
  return s;
}

void sendStatus(IPAddress ip, uint16_t port) {
  String s = statusJson();
  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)s.c_str(), s.length());
  udp.endPacket();
}

void announce() {
  if (apMode || WiFi.status() != WL_CONNECTED || !udpReady) return;
  IPAddress broadcast(255, 255, 255, 255);
  String s = statusJson("winforce_discovery");
  udp.beginPacket(broadcast, DISCOVERY_PORT);
  udp.write((const uint8_t*)s.c_str(), s.length());
  udp.endPacket();
}

bool validPercent(float p) {
  return isfinite(p) && p >= 0.0f && p <= 100.0f;
}

void acceptCommand(uint32_t seq, IPAddress senderIp, uint16_t senderPort) {
  lastSeq = seq;
  lastCommandMs = millis();
  lastAcceptedMs = lastCommandMs;
  udpAcceptedCount++;

  Serial.print("UDP Kommando akzeptiert, Seq=");
  Serial.print(seq);
  Serial.print(" / Fan=");
  Serial.print(legacyFanPercent(), 1);
  Serial.print("% / von ");
  Serial.print(senderIp);
  Serial.print(":");
  Serial.println(senderPort);

  sendStatus(senderIp, senderPort);
}

void handleUdp() {
  if (!udpReady) return;

  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    IPAddress senderIp = udp.remoteIP();
    uint16_t senderPort = udp.remotePort();

    char buf[220];
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';

    udpRxCount++;
    lastUdpRxMs = millis();
    lastUdpSender = senderIp.toString() + ":" + String(senderPort);

    String raw = String(buf);
    raw.trim();
    lastUdpRaw = raw;

    bool accepted = false;
    float p1 = 0.0f, p2 = 0.0f, p3 = 0.0f;
    unsigned long seq = 0;
    int channel = 0;

    // Match exact command prefixes before sscanf.
    // Important: scanf numeric conversions skip whitespace. Without this,
    // "FAN 100.00 4499" can be misread as "FAN100 ...".
    if (raw.startsWith("FANS ")) {
      if (sscanf(raw.c_str(), "FANS %f %f %f %lu", &p1, &p2, &p3, &seq) == 4 &&
          validPercent(p1) && validPercent(p2) && validPercent(p3)) {
        setFansPercent(p1, p2, p3);
        acceptCommand((uint32_t)seq, senderIp, senderPort);
        accepted = true;
      }
    }
    else if (raw.startsWith("FAN ")) {
      if (sscanf(raw.c_str(), "FAN %f %lu", &p1, &seq) == 2 &&
          validPercent(p1)) {
        setAllFansPercent(p1);
        acceptCommand((uint32_t)seq, senderIp, senderPort);
        accepted = true;
      }
    }
    else if (raw.startsWith("FAN1 ") || raw.startsWith("FAN2 ") || raw.startsWith("FAN3 ")) {
      if (sscanf(raw.c_str(), "FAN%d %f %lu", &channel, &p1, &seq) == 3 &&
          channel >= 1 && channel <= 3 && validPercent(p1)) {
        setFanPercent((uint8_t)(channel - 1), p1);
        acceptCommand((uint32_t)seq, senderIp, senderPort);
        accepted = true;
      }
    }
    else if (raw.startsWith("STATUS") || raw.startsWith("PING")) {
      sendStatus(senderIp, senderPort);
      accepted = true;
    }

    if (!accepted) {
      udpRejectedCount++;
    }

    packetSize = udp.parsePacket();
  }
}

String setupPage(const String& message = "") {
  String page = F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'><title>VRC WinForce Setup</title>"
    "<style>body{font-family:Arial,sans-serif;max-width:720px;margin:2rem auto;padding:1rem;background:#111;color:#eee}"
    "input{width:100%;box-sizing:border-box;padding:.7rem;margin:.4rem 0 1rem;border-radius:6px;border:1px solid #555;background:#222;color:#fff}"
    "button{padding:.8rem 1.1rem;border:0;border-radius:6px;background:#2f78ff;color:#fff;font-weight:bold}"
    ".card{background:#1b1b1b;padding:1rem;border-radius:10px;margin-bottom:1rem}small{color:#aaa}</style></head><body>"
    "<h1>VRC WinForce ESP32</h1>"
  );
  if (message.length()) page += "<div class='card'>" + htmlEscape(message) + "</div>";
  page += F("<div class='card'><form method='post' action='/save'>"
            "<label>WLAN-Name (SSID)</label><input name='ssid' maxlength='64' required>"
            "<label>WLAN-Passwort</label><input name='pass' type='password' maxlength='64'>"
            "<button type='submit'>Speichern & neu starten</button>"
            "</form></div>");
  page += "<div class='card'><b>Firmware:</b> " + String(FW_VERSION) + "<br>";
  page += "<b>PWM:</b> GPIO 4 / 16 / 17, " + String(PWM_FREQUENCY) + " Hz<br>";
  page += F("</body></html>");
  return page;
}

String statusPage() {
  String page = F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    ""
    "<meta charset='utf-8'><title>VRC WinForce ESP32</title>"
    "<style>body{font-family:Arial,sans-serif;max-width:720px;margin:2rem auto;padding:1rem;background:#111;color:#eee}"
    ".card{background:#1b1b1b;padding:1rem;border-radius:10px;margin-bottom:1rem}.fan{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.6rem}"
    ".tile{background:#242424;padding:.8rem;border-radius:8px}button{padding:.7rem 1rem;border:0;border-radius:6px;background:#b33;color:#fff}</style></head><body>"
    "<h1>VRC WinForce ESP32</h1><div class='card'>"
  );
  page += "<b>Firmware:</b> " + String(FW_VERSION) + "<br>";
  page += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  page += "<b>WLAN:</b> " + htmlEscape(WiFi.SSID()) + "<br>";
  page += "<b>RSSI:</b> " + String(WiFi.RSSI()) + " dBm<br>";
  page += "<b>UDP:</b> " + String(COMMAND_PORT) + " / Discovery " + String(DISCOVERY_PORT) + "<br>";
  page += "<b>Tacho:</b> nur Anzeige, keine Regelung<br>";
  page += "Lüfter 1: <b>" + String(fanRpm[0]) + " RPM</b> (GPIO25)<br>";
  page += "Lüfter 2: <b>" + String(fanRpm[1]) + " RPM</b> (GPIO26)<br>";
  page += "Lüfter 3: <b>" + String(fanRpm[2]) + " RPM</b> (GPIO27)</div>";

  page += "<div class='card'><div class='fan'>";
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    page += "<div class='tile'><b>Lüfter " + String(i + 1) + "</b><br>";
    page += String(fanPercent[i], 2) + " %<br>";
    page += "PWM GPIO " + String(PWM_PINS[i]) + "</div>";
  }
  page += "</div></div>";

  page += "<div class='card'><b>UDP Diagnose</b><br>";
  page += "UDP Socket: <b>" + String(udpReady ? "AKTIV" : "FEHLER") + "</b><br>";
  page += "RX Pakete: <b>" + String(udpRxCount) + "</b><br>";
  page += "Akzeptiert: <b>" + String(udpAcceptedCount) + "</b><br>";
  page += "Abgelehnt: <b>" + String(udpRejectedCount) + "</b><br>";
  page += "Letzter Absender: <b>" + htmlEscape(lastUdpSender) + "</b><br>";
  page += "Letztes Paket: <code>" + htmlEscape(lastUdpRaw) + "</code>";
  page += "</div>";

  page += F("<div class='card'><b>PWM Direkt-Test</b><br>"
            "<small>Umgeht die PC-Software für 15 Sekunden.</small><br><br>"
            "<a href='/fantest?p=0'><button type='button'>0%</button></a> "
            "<a href='/fantest?p=25'><button type='button'>25%</button></a> "
            "<a href='/fantest?p=50'><button type='button'>50%</button></a> "
            "<a href='/fantest?p=75'><button type='button'>75%</button></a> "
            "<a href='/fantest?p=100'><button type='button'>100%</button></a>"
            "</div>");

  page += F(
    "<div class='card'><b>Firmware Update über WLAN</b><br>"
    "<small>Nur eine für VRC WinForce kompilierte ESP32-.bin-Datei verwenden. "
    "Während des Updates wird PWM auf 0 % gesetzt. USB bleibt nur für Recovery nötig.</small><br><br>"
    "<form id='fwForm' method='POST' action='/update' enctype='multipart/form-data'>"
    "<input id='fwFile' type='file' name='firmware' accept='.bin,application/octet-stream' required> "
    "<button type='submit'>Firmware hochladen</button>"
    "</form>"
    "<div id='fwSelected' style='margin-top:.6rem;color:#9ad'></div>"
    "<script>"
    "const f=document.getElementById('fwFile');"
    "const s=document.getElementById('fwSelected');"
    "let fwBusy=false;"
    "f.addEventListener('change',()=>{"
      "if(f.files&&f.files.length){"
        "fwBusy=true;"
        "s.textContent='Ausgewählt: '+f.files[0].name;"
      "}else{"
        "fwBusy=false;"
        "s.textContent='';"
      "}"
    "});"
    "document.getElementById('fwForm').addEventListener('submit',()=>{fwBusy=true;});"
    "setInterval(()=>{if(!fwBusy) location.reload();},5000);"
    "</script></div>"
  );

  page += F("<div class='card'><form method='post' action='/resetwifi'><button>WLAN-Zugangsdaten löschen</button></form></div>");
  page += F("</body></html>");
  return page;
}

void startWebRoutes() {
  web.on("/", HTTP_GET, []() {
    web.send(200, "text/html; charset=utf-8", apMode ? setupPage() : statusPage());
  });
  web.on("/status.json", HTTP_GET, []() {
    web.send(200, "application/json", statusJson());
  });
  web.on("/fantest", HTTP_GET, []() {
    float p = web.arg("p").toFloat();
    p = clampPercent(p);
    setAllFansPercent(p);
    webTestUntilMs = millis() + 15000UL;
    lastCommandMs = millis();

    Serial.print("WEB PWM TEST: ");
    Serial.print(p, 1);
    Serial.println("%");

    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
  });
  web.on("/save", HTTP_POST, []() {
    if (!apMode) {
      web.send(403, "text/plain", "Setup only in AP mode");
      return;
    }
    String ssid = web.arg("ssid");
    String pass = web.arg("pass");
    ssid.trim();
    if (ssid.length() < 1) {
      web.send(400, "text/html; charset=utf-8", setupPage("SSID fehlt."));
      return;
    }
    prefs.begin("winforce", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    web.send(200, "text/html; charset=utf-8", setupPage("Gespeichert. ESP32 startet neu ..."));
    delay(700);
    ESP.restart();
  });
  // Browser/API firmware update endpoint.
  // Can later also be driven directly by the Windows app using HTTP POST.
  web.on(
    "/update",
    HTTP_POST,
    []() {
      const bool ok = !Update.hasError();

      // Keep outputs off while rebooting into the new firmware.
      setAllFansPercent(0.0f);

      if (ok) {
        web.send(
          200,
          "text/html; charset=utf-8",
          "<!doctype html><html><body style='font-family:Arial;background:#111;color:#eee;padding:2rem'>"
          "<h2>VRC WinForce Firmware Update erfolgreich</h2>"
          "<p>Der ESP32 startet jetzt neu.</p>"
          "<p>Nach einigen Sekunden die Seite neu laden.</p>"
          "</body></html>"
        );
      } else {
        String msg = "Firmware Update fehlgeschlagen. Fehlercode: ";
        msg += String(Update.getError());
        web.send(500, "text/plain; charset=utf-8", msg);
      }

      delay(700);
      if (ok) ESP.restart();
    },
    []() {
      HTTPUpload& upload = web.upload();

      if (upload.status == UPLOAD_FILE_START) {
        Serial.print("OTA Web-Update Start: ");
        Serial.println(upload.filename);

        setAllFansPercent(0.0f);
        webTestUntilMs = 0;
        lastCommandMs = millis();

        // UPDATE_SIZE_UNKNOWN lets the ESP infer the available OTA partition size.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Serial.print("Update.begin Fehler: ");
          Serial.println(Update.getError());
        }
      }
      else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!Update.hasError()) {
          const size_t written = Update.write(upload.buf, upload.currentSize);
          if (written != upload.currentSize) {
            Serial.print("Update.write Fehler: ");
            Serial.println(Update.getError());
          }
        }
      }
      else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.hasError()) {
          if (Update.end(true)) {
            Serial.print("OTA Web-Update fertig, Bytes: ");
            Serial.println(upload.totalSize);
          } else {
            Serial.print("Update.end Fehler: ");
            Serial.println(Update.getError());
          }
        } else {
          Update.abort();
        }
      }
      else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("OTA Web-Update abgebrochen");
      }
    }
  );

  web.on("/resetwifi", HTTP_POST, []() {
    prefs.begin("winforce", false);
    prefs.clear();
    prefs.end();
    web.send(200, "text/plain; charset=utf-8", "WLAN gelöscht. Neustart ...");
    delay(500);
    ESP.restart();
  });
  web.onNotFound([]() {
    if (apMode) web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(apMode ? 302 : 404, "text/plain", apMode ? "" : "Not found");
  });
  web.begin();
}

bool connectStoredWiFi() {
  prefs.begin("winforce", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("Verbinde mit WLAN: "); Serial.println(ssid);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WLAN verbunden, IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  WiFi.disconnect(true);
  return false;
}

void startSetupAP() {
  apMode = true;
  setAllFansPercent(0.0f);
  WiFi.mode(WIFI_AP);
  uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06X", (uint32_t)(chip & 0xFFFFFF));
  String apName = String("VRC-WinForce-Setup-") + suffix;
  const char* apPass = "winforce123";
  WiFi.softAP(apName.c_str(), apPass);
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);
  Serial.println("--- VRC WinForce WLAN Setup ---");
  Serial.print("SSID: "); Serial.println(apName);
  Serial.print("Passwort: "); Serial.println(apPass);
  Serial.print("Browser: http://"); Serial.println(ip);
}

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword("winforce");

  ArduinoOTA.onStart([]() {
    setAllFansPercent(0.0f);
    webTestUntilMs = 0;
    Serial.println("ArduinoOTA Update gestartet - PWM 0%");
  });

  ArduinoOTA.onEnd([]() {
    setAllFansPercent(0.0f);
    Serial.println("ArduinoOTA Update abgeschlossen");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    setAllFansPercent(0.0f);
    Serial.print("ArduinoOTA Fehler: ");
    Serial.println((unsigned int)error);
  });

  ArduinoOTA.begin();
}

void setupPwm() {
  const uint8_t pwmCount = ENABLE_MULTI_PWM ? FAN_COUNT : 1;
  for (uint8_t i = 0; i < pwmCount; ++i) {
    if (!ledcAttach(PWM_PINS[i], PWM_FREQUENCY, PWM_RESOLUTION_BITS)) {
      Serial.print("FEHLER: LEDC konnte nicht initialisiert werden, GPIO ");
      Serial.println(PWM_PINS[i]);
    }
  }
  setAllFansPercent(0.0f);
}

void setupTachometer() {
  // 4-pin PC fan tach is normally open collector.
  // External 10k pull-up to 3.3V is required/recommended per tach line.
  // Tacho is telemetry only and has zero influence on PWM.
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    pinMode(TACH_PINS[i], INPUT);
  }

  attachInterrupt(digitalPinToInterrupt(TACH_PINS[0]), tachISR1, FALLING);
  attachInterrupt(digitalPinToInterrupt(TACH_PINS[1]), tachISR2, FALLING);
  attachInterrupt(digitalPinToInterrupt(TACH_PINS[2]), tachISR3, FALLING);

  lastTachSampleMs = millis();
  Serial.println("Tacho nur Anzeige:");
  Serial.println("  Fan 1 -> GPIO25");
  Serial.println("  Fan 2 -> GPIO26");
  Serial.println("  Fan 3 -> GPIO27");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("VRC WinForce ESP32 V1.3.3");
  Serial.print("Mehrkanal-PWM: "); Serial.println(ENABLE_MULTI_PWM ? "AN" : "AUS");

  setupPwm();
  setupTachometer();

  bool connected = connectStoredWiFi();
  if (!connected) startSetupAP();

  startWebRoutes();
  if (!apMode) {
    udpReady = (udp.begin(COMMAND_PORT) == 1);
    Serial.print("UDP Port ");
    Serial.print(COMMAND_PORT);
    Serial.print(": ");
    Serial.println(udpReady ? "AKTIV" : "FEHLER");

    setupOTA();
    if (udpReady) {
      announce();
      lastAnnounceMs = millis();
    }
  }
  lastCommandMs = millis();
}

void loop() {
  web.handleClient();

  if (apMode) {
    dns.processNextRequest();
    delay(2);
    return;
  }

  ArduinoOTA.handle();
  handleUdp();
  updateTachometer();

  const uint32_t now = millis();
  const bool webTestActive =
      webTestUntilMs != 0 &&
      (int32_t)(webTestUntilMs - now) > 0;

  if (!webTestActive && (uint32_t)(now - lastCommandMs) > COMMAND_TIMEOUT_MS) {
    bool anyRunning = false;
    for (uint8_t i = 0; i < FAN_COUNT; ++i) {
      if (fanPercent[i] != 0.0f) anyRunning = true;
    }
    if (anyRunning) {
      Serial.println("Failsafe: keine PC-Kommandos -> Fan 0%");
      setAllFansPercent(0.0f);
    }
  }

  if ((uint32_t)(now - lastAnnounceMs) >= ANNOUNCE_INTERVAL_MS) {
    lastAnnounceMs = now;
    announce();
  }

  if (WiFi.status() != WL_CONNECTED) {
    setAllFansPercent(0.0f);
  }
  delay(2);
}
