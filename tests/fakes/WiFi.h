#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  String(int value) : std::string(std::to_string(value)) {}
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
};

inline String operator+(const String& value, int number) {
  return std::string(value) + std::to_string(number);
}

#define F(value) value

namespace Fake {
static uint32_t now = 0;
static uint32_t delayed = 0;
}

inline uint32_t millis() { return Fake::now; }
inline void delay(uint32_t duration) {
  Fake::now += duration;
  Fake::delayed += duration;
}

static struct {
  template <typename... Args>
  void printf(const char*, Args...) {}
  void println(const char*) {}
} Serial;

enum { WIFI_OFF, WIFI_AP, WIFI_STA, WIFI_AP_STA };

struct FakeAddress {
  String toString() const { return "192.168.4.1"; }
};

static struct {
  int modeValue = WIFI_OFF;
  unsigned stations = 0;
  bool open = false;
  unsigned disconnects = 0;
  std::string beginSsid, beginPassword;
  unsigned begins = 0, stationDisconnects = 0;

  void setHostname(const char*) {}
  void begin(const char* ssid, const char* password) {
    beginSsid = ssid; beginPassword = password; ++begins;
  }
  void disconnect() { ++stationDisconnects; }
  bool mode(int value) { modeValue = value; return true; }
  bool softAP(const char*) { open = true; return true; }
  FakeAddress softAPIP() const { return {}; }
  unsigned softAPgetStationNum() const { return stations; }
  bool softAPdisconnect(bool) { open = false; ++disconnects; return true; }
} WiFi;
