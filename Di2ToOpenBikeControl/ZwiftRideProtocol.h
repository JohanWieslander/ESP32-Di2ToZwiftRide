#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

// Wire format reference:
// https://github.com/rainyx/zwift-joypad-android (unencrypted Ride preset).
namespace ZwiftRide {
using Bytes = std::vector<uint8_t>;

inline void varint(Bytes& out, uint32_t value) {
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    out.push_back(byte | (value ? 0x80 : 0));
  } while (value);
}

inline void number(Bytes& out, uint8_t field, uint32_t value) {
  varint(out, field << 3);
  varint(out, value);
}

inline void bytes(Bytes& out, uint8_t field, const Bytes& value) {
  varint(out, (field << 3) | 2);
  varint(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}

inline bool readVarint(const uint8_t* data, size_t length, size_t& pos, uint32_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 35 && pos < length; shift += 7) {
    uint8_t byte = data[pos++];
    if (shift == 28 && (byte & 0xF0)) return false;
    value |= uint32_t(byte & 0x7F) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}

inline bool getObjectId(const uint8_t* data, size_t length, uint32_t& object) {
  size_t pos = 0;
  bool found = false;
  while (pos < length) {
    uint32_t tag;
    if (!readVarint(data, length, pos, tag) || (tag >> 3) == 0) return false;
    uint32_t value;
    switch (tag & 7) {
      case 0:
        if (!readVarint(data, length, pos, value)) return false;
        if ((tag >> 3) == 1) {
          object = value;
          found = true;
        }
        break;
      case 2:
        if (!readVarint(data, length, pos, value) || value > length - pos) return false;
        pos += value;
        break;
      case 1:
      case 5: {
        size_t size = (tag & 7) == 1 ? 8 : 4;
        if (length - pos < size) return false;
        pos += size;
        break;
      }
      default:
        return false;
    }
  }
  return found;
}

inline Bytes inputReport(uint32_t releasedButtons) {
  Bytes out{0x23};
  number(out, 1, releasedButtons);
  return out;
}

inline bool getResponse(uint32_t object, Bytes& out) {
  Bytes data;
  switch (object) {
    case 0x0000: {
      Bytes info;
      number(info, 1, 0x0204);
      bytes(info, 2, {0x01, 0x00, 0x02, 0x01});
      bytes(info, 3, {'D', 'i', '2', ' ', 'R', 'i', 'd', 'e'});
      bytes(info, 7, {'A'});
      Bytes capability;
      number(capability, 1, 2);
      number(capability, 2, 3);
      bytes(info, 8, capability);
      number(info, 9, 1);
      number(info, 10, 8);
      bytes(data, 1, info);
      break;
    }
    case 0x0400:
      number(data, 1, 0xFFFF);
      number(data, 2, 0);  // Digital controls only; no steering paddles.
      break;
    case 0x0303:
      number(data, 1, 0);
      number(data, 2, 100);
      number(data, 3, 0);
      number(data, 4, 0);
      break;
    case 0x0010:
    case 0x0011:
      number(data, 1, 0);
      break;
    default:
      return false;
  }
  out = {0x3C};
  number(out, 1, object);
  bytes(out, 2, data);
  return true;
}

enum class Action : uint8_t {
  ShiftUp, ShiftDown, IntensityUp, IntensityDown, Up, Down, Left, Right,
  Select, Back, Menu, Emote, PowerUp
};

class Buttons {
 public:
  void reset() { active_ = 0; }

  bool update(Action button, bool pressed) {
    // OBC actions that share a Ride key retain independent state.
    uint32_t mask = uint32_t(1) << static_cast<uint8_t>(button);
    bool changed = ((active_ & mask) != 0) != pressed;
    if (pressed) active_ |= mask;
    else active_ &= ~mask;
    return changed;
  }

  uint32_t released() const {
    uint32_t value = UINT32_MAX;
    for (uint8_t action = 0; action <= static_cast<uint8_t>(Action::PowerUp); ++action) {
      if (active_ & (uint32_t(1) << action)) value &= ~(uint32_t(1) << key(action));
    }
    return value;
  }

  static uint8_t key(uint8_t action) {
    // Shift +/-, intensity +/-, up/down/left/right, A/B/B/Z/Y.
    static const uint8_t keys[] = {12, 8, 12, 8, 1, 3, 0, 2, 4, 5, 5, 7, 6};
    return keys[action];
  }

 private:
  uint32_t active_ = 0;
};
}  // namespace ZwiftRide
