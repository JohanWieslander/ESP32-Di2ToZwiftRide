#include "../Di2ToZwiftRide/ConfigPortal.h"

#include <cassert>
#include <algorithm>

using namespace ConfigPortal;

static ChannelMap mapping[4];

static void boot(uint32_t time = 0) {
  Fake::now = time;
  Fake::delayed = 0;
  WiFi.stations = 0;
  WiFi.disconnects = 0;
  server.arguments.clear();
  std::copy(DEFAULT_CHANNEL_MAP, DEFAULT_CHANNEL_MAP + 4, mapping);
  begin(mapping);
}

static void fillForm() {
  for (int ch = 0; ch < 4; ++ch) {
    server.arguments[key(ch, 0)] = String(OBC_SELECT);
    server.arguments[key(ch, 1)] = String(OBC_STEER_RIGHT);
    server.arguments[key(ch, 2)] = String(OBC_NONE);
  }
}

static void assertDefaults() {
  for (int ch = 0; ch < 4; ++ch) {
    assert(mapping[ch].shortPress == DEFAULT_CHANNEL_MAP[ch].shortPress);
    assert(mapping[ch].longPress == DEFAULT_CHANNEL_MAP[ch].longPress);
    assert(mapping[ch].doublePress == DEFAULT_CHANNEL_MAP[ch].doublePress);
  }
}

static void testSaveAndReload() {
  boot();
  server.request("/", HTTP_GET);
  assert(server.status == 200);
  assert(server.body.find("name=c3d") != String::npos);
  assert(server.body.find("formaction=/saveclose") != String::npos);
  fillForm();
  server.request("/save", HTTP_POST);
  assert(server.status == 200);
  assert(WiFi.open && server.running && dns.running);
  boot();
  for (int ch = 0; ch < 4; ++ch) {
    assert(mapping[ch].shortPress == OBC_SELECT);
    assert(mapping[ch].longPress == OBC_STEER_RIGHT);
    assert(mapping[ch].doublePress == OBC_NONE);
  }
  server.request("/reset", HTTP_POST);
  assert(server.status == 200);
  assertDefaults();
  boot();
  assertDefaults();
}

static void testInvalidFormIsRejected() {
  // None is valid, but malformed strings must not silently become None.
  const char* invalid[] = {
    "", "garbage", "1junk", "-1", " 1", "3", "256", "999999999999999999999"
  };
  for (const char* value : invalid) {
    boot();
    fillForm();
    server.arguments["c3d"] = value;
    unsigned writes = prefs.writes;
    server.request("/saveclose", HTTP_POST);
    assert(server.status == 400);
    assertDefaults();
    assert(prefs.writes == writes);
    Fake::now = 200;
    handle();
    assert(WiFi.open);
  }
  boot();
  fillForm();
  server.arguments.erase("c3d");
  server.request("/save", HTTP_POST);
  assert(server.status == 400);
  assertDefaults();
}

static void testInvalidStoredActionUsesDefault() {
  prefs.values["c0s"] = 255;
  prefs.values["c1l"] = OBC_NONE;
  boot();
  assert(mapping[0].shortPress == DEFAULT_CHANNEL_MAP[0].shortPress);
  assert(mapping[1].longPress == OBC_NONE);
  prefs.values.clear();
}

static void testIdleTimeout() {
  boot();
  Fake::now = 29999;
  handle();
  assert(WiFi.open);
  Fake::now = 30000;
  handle();
  assert(!WiFi.open && WiFi.modeValue == WIFI_OFF);
  assert(!server.running && !dns.running);
  unsigned handled = server.handled;
  unsigned processed = dns.processed;
  handle();
  assert(WiFi.disconnects == 1);
  assert(server.handled == handled && dns.processed == processed);

  boot(UINT32_MAX - 10000);
  Fake::now += 29999;
  handle();
  assert(WiFi.open);
  ++Fake::now;
  handle();
  assert(!WiFi.open);
}

static void testConnectionKeepsPortalOpen() {
  boot();
  Fake::now = 29999;
  WiFi.stations = 1;
  handle();
  WiFi.stations = 0;
  Fake::now = 60000;
  handle();
  assert(WiFi.open && server.running && dns.running);

  boot();
  Fake::now = 30000;
  WiFi.stations = 1;
  handle();
  assert(WiFi.open);
}

static void testSaveAndClose() {
  boot();
  fillForm();
  server.request("/saveclose", HTTP_POST);
  assert(server.status == 200);
  assert(server.body.find("portal is closing") != String::npos);
  assert(mapping[0].shortPress == OBC_SELECT);
  assert(prefs.values.at("c0s") == OBC_SELECT);
  assert(WiFi.open);
  handle();
  assert(Fake::delayed == 0);
  Fake::now = 199;
  handle();
  assert(WiFi.open);
  Fake::now = 200;
  handle();
  assert(!WiFi.open && !server.running && !dns.running);

  boot();
  assert(mapping[0].shortPress == OBC_SELECT);
  handle();
  assert(WiFi.open);
  server.request("/reset", HTTP_POST);
}

static void testFailedSaveDoesNotCloseOrApply() {
  boot();
  fillForm();
  prefs.failOpen = true;
  server.request("/saveclose", HTTP_POST);
  assert(server.status == 500);
  assertDefaults();
  prefs.failOpen = false;
  prefs.failWrite = true;
  server.request("/saveclose", HTTP_POST);
  assert(server.status == 500);
  assertDefaults();
  handle();
  assert(WiFi.open);
  prefs.failWrite = false;

  server.request("/save", HTTP_POST);
  assert(server.status == 200);
  prefs.failClear = true;
  server.request("/reset", HTTP_POST);
  assert(server.status == 500);
  assert(mapping[0].shortPress == OBC_SELECT);
  prefs.failClear = false;
  server.request("/reset", HTTP_POST);
}

int main() {
  testSaveAndReload();
  testInvalidFormIsRejected();
  testInvalidStoredActionUsesDefault();
  testIdleTimeout();
  testConnectionKeepsPortalOpen();
  testSaveAndClose();
  testFailedSaveDoesNotCloseOrApply();
  boot();
  server.request("/generate_204", HTTP_GET);
  assert(server.status == 302);
  assert(server.headers.at("Location") == "http://192.168.4.1/");
}
