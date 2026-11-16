#include "../Di2ToOpenBikeControl/ZwiftRideProtocol.h"

#include <assert.h>

int main() {
  using namespace ZwiftRide;
  Buttons buttons;
  assert(inputReport(buttons.released()) == Bytes({0x23, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
      0x1A, 0x04, 0x08, 0x00, 0x10, 0x00, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x00}));
  assert(buttons.update(Action::ShiftUp, true));
  assert(!buttons.update(Action::ShiftUp, true));
  assert(inputReport(buttons.released()) == Bytes({0x23, 0x08, 0xFF, 0xDF, 0xFF, 0xFF, 0x0F,
      0x1A, 0x04, 0x08, 0x00, 0x10, 0x00, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x00}));
  buttons.update(Action::IntensityUp, true);
  buttons.update(Action::ShiftUp, false);
  assert((buttons.released() & (1U << 12)) == 0);
  buttons.update(Action::IntensityUp, false);
  assert(buttons.released() == UINT32_MAX);
  buttons.update(Action::Select, true);
  buttons.update(Action::Emote, true);
  assert((buttons.released() & 0x90) == 0);
  buttons.reset();
  assert(buttons.released() == UINT32_MAX);

  const Action controls[] = {Action::Left, Action::Right, Action::Back,
                             Action::Select, Action::Emote, Action::PowerUp};
  const uint8_t keyBits[] = {0, 2, 5, 4, 7, 6};
  for (size_t i = 0; i < sizeof(keyBits); ++i) {
    assert(buttons.update(controls[i], true));
    assert(buttons.released() == (UINT32_MAX & ~(1U << keyBits[i])));
    assert(buttons.update(controls[i], false));
    assert(buttons.released() == UINT32_MAX);
  }
  buttons.update(Action::ShiftDown, true);
  buttons.update(Action::PowerUp, true);
  buttons.update(Action::PowerUp, false);
  assert(buttons.released() == (UINT32_MAX & ~(1U << 8)));
  buttons.reset();

  buttons.update(Action::SteerLeft, true);
  assert(buttons.leftSteering() == -100 && buttons.rightSteering() == 0);
  assert(buttons.released() == UINT32_MAX);
  buttons.update(Action::SteerRight, true);
  buttons.update(Action::ShiftUp, true);
  assert(inputReport(buttons.released(), buttons.leftSteering(), buttons.rightSteering()) ==
      Bytes({0x23, 0x08, 0xFF, 0xDF, 0xFF, 0xFF, 0x0F,
             0x1A, 0x05, 0x08, 0x00, 0x10, 0xC7, 0x01,
             0x1A, 0x05, 0x08, 0x01, 0x10, 0xC8, 0x01}));
  buttons.update(Action::SteerLeft, false);
  assert(buttons.leftSteering() == 0 && buttons.rightSteering() == 100);
  assert(buttons.released() == (UINT32_MAX & ~(1U << 12)));
  buttons.reset();
  assert(buttons.leftSteering() == 0 && buttons.rightSteering() == 0);

  uint32_t object = 0;
  Bytes get{0x08, 0x80, 0x08};
  assert(getObjectId(get.data(), get.size(), object) && object == 0x0400);
  Bytes reply;
  assert(getResponse(object, reply));
  assert(reply == Bytes({0x3C, 0x08, 0x80, 0x08, 0x12, 0x1A,
                         0x08, 0xFF, 0xFF, 0x03, 0x10, 0x03,
                         0x1A, 0x08, 0x08, 0x00, 0x10, 0xC7, 0x01, 0x18, 0xC8, 0x01,
                         0x1A, 0x08, 0x08, 0x01, 0x10, 0xC7, 0x01, 0x18, 0xC8, 0x01}));
  assert(getResponse(0x0303, reply));
  assert(reply == Bytes({0x3C, 0x08, 0x83, 0x06, 0x12, 0x08,
                         0x08, 0x00, 0x10, 0x64, 0x18, 0x00, 0x20, 0x00}));
  assert(getResponse(0, reply));
  assert(reply == Bytes({0x3C, 0x08, 0x00, 0x12, 0x22, 0x0A, 0x20,
                         0x08, 0x84, 0x04, 0x12, 0x04, 0x01, 0x00, 0x02, 0x01,
                         0x1A, 0x08, 'D', 'i', '2', ' ', 'R', 'i', 'd', 'e',
                         0x3A, 0x01, 'A', 0x42, 0x04, 0x08, 0x02, 0x10, 0x03,
                         0x48, 0x01, 0x50, 0x08}));
  assert(!getResponse(0xFFFF, reply));

  const Bytes invalid[] = {{}, {0x08}, {0x08, 0x80}, {0x00, 0x01},
                            {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0x10},
                            {0x12, 0x05, 0x01}, {0x08, 0x01, 0x15, 0x00}};
  for (const Bytes& data : invalid) assert(!getObjectId(data.data(), data.size(), object));
  Bytes extra{0x12, 0x02, 0x01, 0x02, 0x08, 0x80, 0x08};
  assert(getObjectId(extra.data(), extra.size(), object) && object == 0x0400);
}
