#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <iterator>
#include <string>
#include "WiFi.h"

class Preferences {
 public:
  std::map<std::string, uint8_t> values;
  bool failOpen = false;
  bool failWrite = false;
  bool failClear = false;
  unsigned writes = 0;

  std::map<std::string, std::string> strings;
  std::string ns;

  bool begin(const char* name, bool) { ns = name; return !failOpen; }
  bool isKey(const char* name) const { return strings.count(ns + "/" + name) != 0; }
  String getString(const char* name, const String& fallback) const {
    auto entry = strings.find(ns + "/" + name);
    return entry == strings.end() ? fallback : String(entry->second);
  }
  size_t putString(const char* name, const String& value) {
    ++writes;
    if (failWrite) return 0;
    strings[ns + "/" + name] = value;
    return value.length();
  }
  void end() {}
  uint8_t getUChar(const char* name, uint8_t fallback) const {
    auto entry = values.find(name);
    return entry == values.end() ? fallback : entry->second;
  }
  size_t putUChar(const char* name, uint8_t value) {
    ++writes;
    if (failWrite) return 0;
    values[name] = value;
    return 1;
  }
  bool clear() {
    if (failClear) return false;
    if (ns == "wifi") {
      for (auto it = strings.begin(); it != strings.end();)
        it = it->first.compare(0, 5, "wifi/") == 0 ? strings.erase(it) : std::next(it);
    } else {
      values.clear();
    }
    return true;
  }
};
