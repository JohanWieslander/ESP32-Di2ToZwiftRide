#include "../Di2ToOpenBikeControl/ConfigPortal.h"

#include <cassert>
#include <algorithm>

using namespace ConfigPortal;

static ChannelMap mapping[4];

static void boot(uint32_t time = 0) {
  Fake::now = time;
  Fake::delayed = 0;
  WiFi.stations = 0;
  WiFi.disconnects = 0;
  WiFi.begins = 0;
  WiFi.stationDisconnects = 0;
  server.arguments.clear();
  std::copy(DEFAULT_CHANNEL_MAP, DEFAULT_CHANNEL_MAP + 4, mapping);
  begin(mapping);
}

static void wifiForm(const char* ssid, const char* password) {
  server.arguments.clear();
  server.arguments["ssid"] = ssid;
  server.arguments["password"] = password;
}

static void testWifiSavedAndConnectedOnBoot() {
  boot();
  assert(!wifiConfigured && WiFi.begins == 0);
  assert(WiFi.modeValue == WIFI_AP_STA);

  wifiForm("Home <net>", "secret123");
  server.request("/wifi", HTTP_POST);
  assert(server.status == 200);
  assert(WiFi.beginSsid == "Home <net>" && WiFi.beginPassword == "secret123");
  assert(server.body.find("Home &lt;net&gt;") != String::npos);
  assert(server.body.find("secret123") == String::npos);

  boot();
  assert(wifiConfigured && wifiSsid == "Home <net>");
  assert(WiFi.begins == 1 && WiFi.beginSsid == "Home <net>");
  assert(WiFi.beginPassword == "secret123");
}

static void testWifiValidation() {
  boot();
  unsigned writes = prefs.writes;
  const char* badPasswords[] = { "short", "" };
  wifiForm("", "");
  server.request("/wifi", HTTP_POST);
  assert(server.status == 400);
  wifiForm("net", badPasswords[0]);
  server.request("/wifi", HTTP_POST);
  assert(server.status == 400);
  wifiForm(std::string(33, 'a').c_str(), "");
  server.request("/wifi", HTTP_POST);
  assert(server.status == 400);
  server.arguments.clear();
  server.request("/wifi", HTTP_POST);
  assert(server.status == 400);
  assert(prefs.writes == writes);

  wifiForm("open", "");
  server.request("/wifi", HTTP_POST);
  assert(server.status == 200 && wifiConfigured);

  prefs.failOpen = true;
  wifiForm("other", "");
  server.request("/wifi", HTTP_POST);
  assert(server.status == 500 && wifiSsid == "open");
  prefs.failOpen = false;
  prefs.failWrite = true;
  server.request("/wifi", HTTP_POST);
  assert(server.status == 500 && wifiSsid == "open");
  prefs.failWrite = false;
}

static void testForgetWifiKeepsMapping() {
  boot();
  wifiForm("net", "password1");
  server.request("/wifi", HTTP_POST);
  server.arguments.clear();
  for (int ch = 0; ch < 4; ++ch)
    for (int p = 0; p < 3; ++p) server.arguments[key(ch, p)] = String(OBC_SELECT);
  server.request("/save", HTTP_POST);
  assert(server.status == 200);

  server.request("/wififorget", HTTP_POST);
  assert(server.status == 200 && !wifiConfigured);
  boot();
  assert(!wifiConfigured && WiFi.begins == 0);
  assert(mapping[0].shortPress == OBC_SELECT);

  server.request("/reset", HTTP_POST);
  wifiForm("net", "password1");
  server.request("/wifi", HTTP_POST);
  server.request("/reset", HTTP_POST);
  assert(server.status == 200);
  boot();
  assert(wifiConfigured);
  server.request("/wififorget", HTTP_POST);
}

static void testCloseKeepsStationOnlyWhenConfigured() {
  boot();
  Fake::now = 30000;
  handle();
  assert(!WiFi.open && WiFi.modeValue == WIFI_OFF);

  boot();
  wifiForm("net", "");
  server.request("/wifi", HTTP_POST);
  Fake::now = 30000;
  handle();
  assert(!WiFi.open && WiFi.modeValue == WIFI_STA);
  boot();
  server.request("/wififorget", HTTP_POST);
}

int main() {
  testWifiSavedAndConnectedOnBoot();
  testWifiValidation();
  testForgetWifiKeepsMapping();
  testCloseKeepsStationOnlyWhenConfigured();
}
