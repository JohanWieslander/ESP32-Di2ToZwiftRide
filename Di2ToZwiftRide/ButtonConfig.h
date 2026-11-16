#pragma once
#include <stdint.h>
#include <stddef.h>

enum ObcButton : uint8_t {
  OBC_SHIFT_UP = 0x01,
  OBC_SHIFT_DOWN = 0x02,
  OBC_NAV_UP = 0x10,
  OBC_NAV_DOWN = 0x11,
  OBC_NAV_LEFT = 0x12,
  OBC_NAV_RIGHT = 0x13,
  OBC_SELECT = 0x14,
  OBC_BACK = 0x15,
  OBC_MENU = 0x16,
  OBC_STEER_LEFT = 0x18,
  OBC_STEER_RIGHT = 0x19,
  OBC_EMOTE = 0x20,
  OBC_SCREENSHOT = 0x24,
  OBC_DIFF_UP = 0x30,
  OBC_DIFF_DOWN = 0x31,
  OBC_LAP = 0x35,
  OBC_U_TURN = 0x37,
  OBC_POWER_UP = 0x50,
  OBC_NONE = 0x00,
};

struct ChannelMap {
  uint8_t shortPress;
  uint8_t longPress;
  uint8_t doublePress;
};

struct ButtonOption {
  uint8_t id;
  const char* label;
};

static const ButtonOption BUTTON_OPTIONS[] = {
  { OBC_NONE, "Nothing" },
  { OBC_SHIFT_DOWN, "Shift down" },
  { OBC_SHIFT_UP, "Shift up" },
  { OBC_NAV_LEFT, "D-pad left / junction left" },
  { OBC_NAV_RIGHT, "D-pad right / junction right" },
  { OBC_NAV_UP, "D-pad up / action bar" },
  { OBC_NAV_DOWN, "D-pad down" },
  { OBC_SELECT, "A - Select" },
  { OBC_BACK, "B - Back" },
  { OBC_MENU, "Menu" },
  { OBC_POWER_UP, "Y - Use power-up" },
  { OBC_EMOTE, "Z - Ride On (hold for bomb)" },
  { OBC_STEER_LEFT, "Steer left (hold)" },
  { OBC_STEER_RIGHT, "Steer right (hold)" },
  { OBC_U_TURN, "U-turn" },
  { OBC_DIFF_UP, "Camera / difficulty up" },
  { OBC_DIFF_DOWN, "Camera / difficulty down" },
};
static const size_t BUTTON_OPTION_COUNT = sizeof(BUTTON_OPTIONS) / sizeof(BUTTON_OPTIONS[0]);

static const ChannelMap DEFAULT_CHANNEL_MAP[4] = {
  { OBC_SHIFT_DOWN, OBC_POWER_UP, OBC_BACK },
  { OBC_SHIFT_UP, OBC_EMOTE, OBC_SELECT },
  { OBC_NAV_LEFT, OBC_STEER_LEFT, OBC_U_TURN },
  { OBC_NAV_RIGHT, OBC_STEER_RIGHT, OBC_NAV_UP },
};

static inline bool isValidButton(int id) {
  for (size_t i = 0; i < BUTTON_OPTION_COUNT; i++)
    if (BUTTON_OPTIONS[i].id == id) return true;
  return false;
}
