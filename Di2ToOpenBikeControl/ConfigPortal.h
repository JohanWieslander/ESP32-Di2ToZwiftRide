#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "ButtonConfig.h"

// Closes 30 s after boot if nobody has connected.
// Open (passwordless) Wi-Fi AP with a captive portal for the CH1-4 mapping.
namespace ConfigPortal {

static const char* AP_SSID = "Di2-Zwift-Bridge";
static const char* PREF_NS = "chmap";
static const char* PRESS_KEYS[3] = { "s", "l", "d" };
static const char* PRESS_NAMES[3] = { "Single press", "Long press", "Double press" };

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;
static ChannelMap* liveMap = nullptr;
static const uint32_t IDLE_TIMEOUT_MS = 30000;
static uint32_t startedAt = 0;
static bool active = false;
static bool clientSeen = false;
static bool closeRequested = false;
static uint32_t closeRequestedAt = 0;

static uint8_t* slot(ChannelMap& m, int press) {
  return press == 0 ? &m.shortPress : press == 1 ? &m.longPress : &m.doublePress;
}

static String key(int ch, int press) { return String("c") + ch + PRESS_KEYS[press]; }

static void load() {
  if (!prefs.begin(PREF_NS, true)) {
    Serial.println("[Portal] no saved mapping available; using defaults");
    return;
  }
  for (int ch = 0; ch < 4; ch++)
    for (int p = 0; p < 3; p++) {
      uint8_t v = prefs.getUChar(key(ch, p).c_str(), *slot(liveMap[ch], p));
      if (isValidButton(v)) *slot(liveMap[ch], p) = v;
    }
  prefs.end();
}

static void sendPage(const String& notice) {
  String h = F("<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Di2 bridge</title><style>"
               "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#111;color:#eee}"
               "fieldset{border:1px solid #444;border-radius:8px;margin:12px 0}"
               "legend{font-weight:600}label{display:block;margin:8px 0 2px;font-size:.9em;color:#bbb}"
               "select,button{width:100%;padding:10px;font-size:1em;border-radius:6px;border:0}"
               "button{background:#fc6719;color:#fff;margin-top:8px}.s{background:#444}"
               ".ok{background:#1d5c2e;padding:8px;border-radius:6px}</style></head><body>"
               "<h2>Di2 &rarr; Zwift Ride</h2>");
  if (notice.length()) h += "<p class=ok>" + notice + "</p>";
  h += F("<form method=post action=/save>");
  for (int ch = 0; ch < 4; ch++) {
    h += "<fieldset><legend>CH" + String(ch + 1) + "</legend>";
    for (int p = 0; p < 3; p++) {
      h += String("<label>") + PRESS_NAMES[p] + "</label><select name=" + key(ch, p) + ">";
      uint8_t cur = *slot(liveMap[ch], p);
      for (size_t i = 0; i < BUTTON_OPTION_COUNT; i++) {
        h += "<option value=" + String(BUTTON_OPTIONS[i].id) + (BUTTON_OPTIONS[i].id == cur ? " selected>" : ">") +
             BUTTON_OPTIONS[i].label + "</option>";
      }
      h += "</select>";
    }
    h += "</fieldset>";
  }
  h += F("<button>Save</button><button formaction=/saveclose class=s>Save and close portal</button></form>"
         "<form method=post action=/reset><button class=s>Restore defaults</button></form></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", h);
}

static bool formError(int status, const char* message) {
  Serial.printf("[Portal] %s\n", message);
  server.send(status, "text/plain", message);
  return false;
}

static bool applyForm() {
  ChannelMap candidate[4];
  for (int ch = 0; ch < 4; ch++)
    for (int p = 0; p < 3; p++) {
      String name = key(ch, p);
      if (!server.hasArg(name)) return formError(400, "Missing button action.");
      String value = server.arg(name);
      if (value.length() == 0) return formError(400, "Invalid button action.");
      int v = 0;
      for (size_t i = 0; i < value.length(); i++) {
        if (value[i] < '0' || value[i] > '9') return formError(400, "Invalid button action.");
        v = v * 10 + value[i] - '0';
        if (v > 255) return formError(400, "Invalid button action.");
      }
      if (!isValidButton(v)) return formError(400, "Unsupported button action.");
      *slot(candidate[ch], p) = static_cast<uint8_t>(v);
    }
  if (!prefs.begin(PREF_NS, false)) return formError(500, "Could not open settings storage.");
  for (int ch = 0; ch < 4; ch++)
    for (int p = 0; p < 3; p++) {
      if (prefs.putUChar(key(ch, p).c_str(), *slot(candidate[ch], p)) != 1) {
        prefs.end();
        return formError(500, "Could not save all settings. Retry saving before rebooting.");
      }
    }
  prefs.end();
  for (int ch = 0; ch < 4; ch++) liveMap[ch] = candidate[ch];
  return true;
}

static void handleSave() {
  if (!applyForm()) return;
  sendPage("Saved. Changes apply immediately.");
}

static void handleSaveClose() {
  if (!applyForm()) return;
  server.send(200, "text/html",
              F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                "<body style='font-family:system-ui;background:#111;color:#eee;padding:16px'>"
                "<h2>Saved</h2><p>The portal is closing. Reboot the bridge to reopen it.</p></body>"));
  closeRequested = true;
  closeRequestedAt = millis();
}

static void handleReset() {
  if (!prefs.begin(PREF_NS, false)) {
    formError(500, "Could not open settings storage.");
    return;
  }
  bool cleared = prefs.clear();
  prefs.end();
  if (!cleared) {
    formError(500, "Could not restore defaults.");
    return;
  }
  for (int ch = 0; ch < 4; ch++) liveMap[ch] = DEFAULT_CHANNEL_MAP[ch];
  sendPage("Defaults restored.");
}

// Any unknown URL (OS connectivity probes) redirects to the portal.
static void handleRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

static void begin(ChannelMap* map) {
  liveMap = map;
  for (int ch = 0; ch < 4; ch++) liveMap[ch] = DEFAULT_CHANNEL_MAP[ch];
  load();
  startedAt = millis();
  active = true;
  clientSeen = false;
  closeRequested = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);  // no password
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, []() { sendPage(""); });
  server.on("/save", HTTP_POST, handleSave);
  server.on("/saveclose", HTTP_POST, handleSaveClose);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleRedirect);
  server.begin();
  Serial.printf("[Portal] open AP '%s' at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void close() {
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  active = false;
  Serial.println("[Portal] closed");
}

static void handle() {
  if (!active) return;
  if (closeRequested) {
    if (millis() - closeRequestedAt >= 200) close();
    return;
  }
  if (!clientSeen) {
    if (WiFi.softAPgetStationNum() > 0) clientSeen = true;
    else if (millis() - startedAt >= IDLE_TIMEOUT_MS) { close(); return; }
  }
  dns.processNextRequest();
  server.handleClient();
}

}  // namespace ConfigPortal
