#include "../Di2ToZwiftRide/Di2ChannelChanges.h"

#include <assert.h>

int main() {
  Di2ChannelChanges changes;
  const uint8_t initial[] = {0, 0x10, 0x00, 0xF0, 0x00};
  const uint8_t same[] = {1, 0x10, 0x00, 0xF0, 0x00};
  const uint8_t otherChanged[] = {2, 0x10, 0x10, 0xF0, 0x00};
  const uint8_t rawChanged[] = {3, 0x11, 0x10, 0xF0, 0x00};
  const uint8_t released[] = {4, 0x01, 0x00, 0xF0, 0x00};
  const uint8_t unmapped[] = {5, 0xF0, 0x00, 0xF0, 0x00};
  const uint8_t mappedAgain[] = {6, 0x10, 0x00, 0xF0, 0x00};
  assert(changes.update(initial, sizeof(initial)) == 0);
  assert(changes.update(same, sizeof(same)) == 0);
  assert(changes.update(otherChanged, sizeof(otherChanged)) == 0x02);
  assert(changes.update(rawChanged, sizeof(rawChanged)) == 0x01);
  assert(changes.update(released, sizeof(released)) == 0x03);
  assert(changes.update(unmapped, sizeof(unmapped)) == 0x01);
  assert(changes.update(mappedAgain, sizeof(mappedAgain)) == 0x01);

  const uint8_t held[] = {7, 0x20, 0x00, 0xF0, 0x00};
  assert(changes.update(held, sizeof(held)) == 0x01);
  assert(changes.update(unmapped, sizeof(unmapped)) == 0x01);
  assert(changes.update(unmapped, sizeof(unmapped)) == 0);

  changes.reset();
  assert(changes.update(mappedAgain, sizeof(mappedAgain)) == 0);
  const uint8_t shortPacket[] = {7, 0x10};
  const uint8_t longerPacket[] = {8, 0x10, 0x10};
  assert(changes.update(shortPacket, sizeof(shortPacket)) == 0);
  assert(changes.update(longerPacket, sizeof(longerPacket)) == 0);
  assert(changes.update(otherChanged, sizeof(otherChanged)) == 0);
}
