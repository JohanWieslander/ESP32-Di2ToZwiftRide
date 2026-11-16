#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

class Preferences {
 public:
  std::map<std::string, uint8_t> values;
  bool failOpen = false;
  bool failWrite = false;
  bool failClear = false;
  unsigned writes = 0;

  bool begin(const char*, bool) { return !failOpen; }
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
    values.clear();
    return true;
  }
};
