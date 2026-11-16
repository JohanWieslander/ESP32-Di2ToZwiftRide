#pragma once

#include <stddef.h>
#include <stdint.h>

class Di2ChannelChanges {
 public:
  uint8_t update(const uint8_t* data, size_t length) {
    if (length < 2) return 0;
    size_t count = length - 1;
    if (count > 4) count = 4;

    uint8_t changed = 0;
    for (size_t i = 0; i < count; ++i) {
      uint8_t value = data[i + 1];
      if (initialized_ && i < previousCount_ && value != 0xF0 &&
          value != previous_[i]) {
        changed |= 1U << i;
      }
      previous_[i] = value;
    }
    previousCount_ = count;
    initialized_ = true;
    return changed;
  }

  void reset() {
    initialized_ = false;
    previousCount_ = 0;
  }

 private:
  uint8_t previous_[4] = {};
  size_t previousCount_ = 0;
  bool initialized_ = false;
};
