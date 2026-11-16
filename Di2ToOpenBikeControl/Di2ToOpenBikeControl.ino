#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include <new>
#include <vector>
#include <deque>
#include "Di2ChannelChanges.h"
#include "ZwiftRideProtocol.h"

static const uint32_t MOMENTARY_PRESS_MS = 80;

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

static const ChannelMap CHANNEL_MAP[4] = {
  { OBC_SHIFT_DOWN, OBC_POWER_UP, OBC_BACK },
  { OBC_SHIFT_UP, OBC_EMOTE, OBC_SELECT },
  { OBC_NAV_LEFT, OBC_STEER_LEFT, OBC_U_TURN },
  { OBC_NAV_RIGHT, OBC_STEER_RIGHT, OBC_NAV_UP },
};

static NimBLEUUID DI2_SERVICE_UUID("000018ef-5348-494d-414e-4f5f424c4500");
static NimBLEUUID DI2_BUTTON_UUID("00002ac2-5348-494d-414e-4f5f424c4500");
static const uint8_t DI2_UNMAPPED = 0xF0;
static const uint8_t DI2_MASK_SHORT = 0x10;
static const uint8_t DI2_MASK_LONG = 0x20;
static const uint8_t DI2_MASK_DBL = 0x40;

enum PressType : uint8_t { PRESS_NONE = 0, PRESS_SHORT, PRESS_LONG, PRESS_DOUBLE };
enum BleEventType : uint8_t { BLE_EVENT_DATA, BLE_EVENT_DISCONNECTED };

struct BleEvent {
  BleEventType type;
  uint8_t length;
  uint8_t data[5];
};

static const uint8_t MSG_BUTTON_STATE = 0x01;
static const uint8_t STATE_RELEASED = 0x00;
static const uint8_t STATE_PRESSED = 0x01;

static NimBLEClient* bleClient = nullptr;
static NimBLERemoteCharacteristic* bleButton = nullptr;
static QueueHandle_t bleEventQueue = nullptr;
static portMUX_TYPE discoveredDeviceMux = portMUX_INITIALIZER_UNLOCKED;
static NimBLEAdvertisedDevice* discoveredDevice = nullptr;
static bool di2Connected = false;
static Di2ChannelChanges di2ChannelChanges;
static PressType lastPress[4] = { PRESS_NONE, PRESS_NONE, PRESS_NONE, PRESS_NONE };
static uint8_t heldButton[4] = { OBC_NONE, OBC_NONE, OBC_NONE, OBC_NONE };

struct PendingRelease {
  uint8_t buttonId;
  uint32_t dueAt;
};

static std::vector<PendingRelease> pendingReleases;


// ---- Experimental Zwift Ride emulation (BLE peripheral) ----
static NimBLEUUID ZWIFT_SERVICE_UUID("00000001-19ca-4651-86e5-fa29dcdd09d1");
static NimBLEUUID ZWIFT_ADV_UUID("0000fc82-0000-1000-8000-00805f9b34fb");
static const char* ZWIFT_ASYNC_UUID = "00000002-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZWIFT_SYNC_RX_UUID = "00000003-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZWIFT_SYNC_TX_UUID = "00000004-19ca-4651-86e5-fa29dcdd09d1";
static const uint8_t ZWIFT_RIDE_ON[] = { 'R', 'i', 'd', 'e', 'O', 'n' };
static NimBLECharacteristic* zwiftAsync = nullptr;
static NimBLECharacteristic* zwiftSyncTx = nullptr;
static NimBLEServer* zwiftServer = nullptr;
static bool zwiftConnected = false;
static bool zwiftHandshake = false;
static bool zwiftSyncBusy = false;
static uint16_t zwiftConnHandle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t lastZwiftInput = 0;
static uint32_t zwiftSyncStartedAt = 0;
static uint32_t zwiftDownPressedAt = 0;
static bool zwiftDownReleasePending = false;
static ZwiftRide::Buttons zwiftButtons;
static std::deque<ZwiftRide::Bytes> zwiftReplies;

enum RideEventType : uint8_t { RIDE_CONNECT, RIDE_DISCONNECT, RIDE_WRITE, RIDE_SYNC_DONE };
struct RideEvent {
  RideEventType type;
  uint16_t connection;
  uint16_t length;
  uint8_t data[185];
};
static QueueHandle_t rideEventQueue = nullptr;

static void queueRideEvent(const RideEvent& event) {
  if (xQueueSend(rideEventQueue, &event, 0) != pdPASS) {
    if (event.type == RIDE_DISCONNECT) {
      xQueueReset(rideEventQueue);
      if (xQueueSend(rideEventQueue, &event, 0) != pdPASS) {
        Serial.println("[Ride] unable to queue disconnect after clearing event queue");
      } else {
        Serial.println("[Ride] event queue cleared to deliver disconnect");
      }
      return;
    }
    Serial.println("[Ride] event queue full; disconnecting central to reset session");
    for (uint16_t connection : zwiftServer->getPeerDevices()) zwiftServer->disconnect(connection);
  }
}

static void sendZwiftInput() {
  if (!zwiftConnected || !zwiftHandshake || zwiftAsync->getSubscribedCount() == 0) return;
  ZwiftRide::Bytes msg = ZwiftRide::inputReport(
      zwiftButtons.released(), zwiftButtons.leftSteering(), zwiftButtons.rightSteering());
  zwiftAsync->setValue(msg.data(), msg.size());
  zwiftAsync->notify();
  lastZwiftInput = millis();
}

// ---- OpenBikeControl BLE transport (for BikeControl and other OBC apps) ----
static NimBLEUUID OBC_BLE_SERVICE_UUID("d273f680-d548-419d-b9d1-fa0472345229");
static const char* OBC_BLE_BUTTON_UUID = "d273f681-d548-419d-b9d1-fa0472345229";
static const char* OBC_BLE_HAPTIC_UUID = "d273f682-d548-419d-b9d1-fa0472345229";
static const char* OBC_BLE_APPINFO_UUID = "d273f683-d548-419d-b9d1-fa0472345229";
static NimBLECharacteristic* obcBleButton = nullptr;
static bool obcBleConnected = false;

static void notifyObcBle(const uint8_t* msg, size_t len) {
  if (obcBleButton == nullptr || !obcBleConnected) return;
  obcBleButton->setValue(msg, len);
  obcBleButton->notify();
}

static void updateZwiftButton(uint8_t buttonId, uint8_t state) {
  ZwiftRide::Action action;
  switch (buttonId) {
    case OBC_SHIFT_UP: action = ZwiftRide::Action::ShiftUp; break;
    case OBC_SHIFT_DOWN: action = ZwiftRide::Action::ShiftDown; break;
    case OBC_DIFF_UP: action = ZwiftRide::Action::IntensityUp; break;
    case OBC_DIFF_DOWN: action = ZwiftRide::Action::IntensityDown; break;
    case OBC_NAV_UP: action = ZwiftRide::Action::Up; break;
    case OBC_NAV_DOWN: action = ZwiftRide::Action::Down; break;
    case OBC_U_TURN: action = ZwiftRide::Action::Down; break;
    case OBC_NAV_LEFT: action = ZwiftRide::Action::Left; break;
    case OBC_NAV_RIGHT: action = ZwiftRide::Action::Right; break;
    case OBC_STEER_LEFT: action = ZwiftRide::Action::SteerLeft; break;
    case OBC_STEER_RIGHT: action = ZwiftRide::Action::SteerRight; break;
    case OBC_SELECT: action = ZwiftRide::Action::Select; break;
    case OBC_BACK: action = ZwiftRide::Action::Back; break;
    case OBC_MENU: action = ZwiftRide::Action::Menu; break;
    case OBC_EMOTE: action = ZwiftRide::Action::Emote; break;
    case OBC_POWER_UP: action = ZwiftRide::Action::PowerUp; break;
    default:
      if (state == STATE_PRESSED) {
        Serial.printf("[Ride] OBC action 0x%02X has no Ride mapping (OBC output retained)\n", buttonId);
      }
      return;
  }
  if (action == ZwiftRide::Action::Down) {
    if (state == STATE_PRESSED) {
      zwiftDownPressedAt = millis();
      zwiftDownReleasePending = false;
    } else if (millis() - zwiftDownPressedAt < 2000) {
      // Ride's U-turn binding requires a hold, unlike the OBC momentary action.
      zwiftDownReleasePending = true;
      return;
    }
  }
  if (zwiftButtons.update(action, state == STATE_PRESSED)) sendZwiftInput();
}

class ZwiftServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    RideEvent event{};
    event.type = RIDE_CONNECT;
    event.connection = desc->conn_handle;
    queueRideEvent(event);
  }
  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    RideEvent event{};
    event.type = RIDE_DISCONNECT;
    event.connection = desc->conn_handle;
    queueRideEvent(event);
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc*) override {
    Serial.printf("[Ride] negotiated MTU %u\n", mtu);
  }
};

class ZwiftSyncRxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    std::string value = c->getValue();
    RideEvent event{};
    event.type = RIDE_WRITE;
    event.connection = desc->conn_handle;
    if (value.size() > sizeof(event.data)) {
      Serial.println("[Ride] oversized command; disconnecting central");
      zwiftServer->disconnect(desc->conn_handle);
      return;
    }
    event.length = value.size();
    memcpy(event.data, value.data(), event.length);
    queueRideEvent(event);
  }
};

class ZwiftSyncTxCB : public NimBLECharacteristicCallbacks {
  void onStatus(NimBLECharacteristic*, Status status, int code) override {
    if (status != SUCCESS_INDICATE && status != SUCCESS_NOTIFY) {
      Serial.printf("[Ride] response transmission failed: %d\n", code);
    }
    RideEvent event{};
    event.type = RIDE_SYNC_DONE;
    event.length = (status == SUCCESS_INDICATE || status == SUCCESS_NOTIFY) ? 0 : 1;
    queueRideEvent(event);
  }
};

class ZwiftInputCB : public NimBLECharacteristicCallbacks {
  void onStatus(NimBLECharacteristic*, Status status, int code) override {
    if (status != SUCCESS_NOTIFY) Serial.printf("[Ride] input transmission failed: %d\n", code);
  }
};

static ZwiftServerCB zwiftServerCB;
static ZwiftSyncRxCB zwiftSyncRxCB;
static ZwiftSyncTxCB zwiftSyncTxCB;
static ZwiftInputCB zwiftInputCB;

static void processRideCommand(const RideEvent& event) {
  if (event.length == 0) {
    Serial.println("[Ride] empty command");
    return;
  }
  Serial.printf("[Ride] RX opcode 0x%02X (%u bytes)\n", event.data[0], event.length);
  ZwiftRide::Bytes reply;
  if (event.length >= sizeof(ZWIFT_RIDE_ON) &&
      memcmp(event.data, ZWIFT_RIDE_ON, sizeof(ZWIFT_RIDE_ON)) == 0) {
    reply = {'R', 'i', 'd', 'e', 'O', 'n', 2, 4};
    zwiftHandshake = true;
    Serial.println("[Ride] unencrypted protocol 2.4 handshake");
  } else if (!zwiftHandshake) {
    Serial.println("[Ride] ignoring command before RideOn handshake");
    return;
  } else if (event.data[0] == 0x00) {
    uint32_t object;
    if (!ZwiftRide::getObjectId(event.data + 1, event.length - 1, object)) {
      Serial.println("[Ride] malformed GET command");
      return;
    }
    Serial.printf("[Ride] GET object 0x%04lX\n", (unsigned long)object);
    if (!ZwiftRide::getResponse(object, reply)) {
      Serial.println("[Ride] unsupported GET object; replying without object data");
      reply = {0x3C};
      ZwiftRide::number(reply, 1, object);
    }
  } else {
    Serial.printf("[Ride] opcode 0x%02X not implemented (haptics/game state not acted on)\n",
                  event.data[0]);
    return;
  }
  if (zwiftReplies.size() >= 8) {
    Serial.println("[Ride] response queue full; disconnecting central");
    zwiftServer->disconnect(zwiftConnHandle);
    return;
  }
  zwiftReplies.push_back(reply);
}

static void serviceZwiftRide() {
  if (zwiftDownReleasePending && millis() - zwiftDownPressedAt >= 2000) {
    zwiftDownReleasePending = false;
    zwiftButtons.update(ZwiftRide::Action::Down, false);
    sendZwiftInput();
  }
  RideEvent event{};
  while (xQueueReceive(rideEventQueue, &event, 0) == pdPASS) {
    if (event.type == RIDE_CONNECT) {
      zwiftConnHandle = event.connection;
      zwiftConnected = obcBleConnected = true;
      zwiftHandshake = zwiftSyncBusy = false;
      zwiftReplies.clear();
      Serial.println("[Ride] central connected");
    } else if (event.type == RIDE_DISCONNECT && event.connection == zwiftConnHandle) {
      zwiftConnected = obcBleConnected = false;
      zwiftHandshake = zwiftSyncBusy = false;
      zwiftConnHandle = BLE_HS_CONN_HANDLE_NONE;
      zwiftReplies.clear();
      Serial.println("[Ride] central disconnected; advertising again");
      NimBLEDevice::getAdvertising()->start();
    } else if (event.type == RIDE_SYNC_DONE) {
      zwiftSyncBusy = false;
      if (event.length != 0 && zwiftConnected) zwiftServer->disconnect(zwiftConnHandle);
    } else if (event.type == RIDE_WRITE && event.connection == zwiftConnHandle) {
      processRideCommand(event);
    }
  }
  if (!zwiftConnected) return;
  if (zwiftSyncBusy && millis() - zwiftSyncStartedAt >= 5000) {
    Serial.println("[Ride] response confirmation timed out; disconnecting central");
    zwiftServer->disconnect(zwiftConnHandle);
    zwiftSyncStartedAt = millis();
  }
  if (!zwiftSyncBusy && !zwiftReplies.empty() && zwiftSyncTx->getSubscribedCount() > 0) {
    const ZwiftRide::Bytes& reply = zwiftReplies.front();
    uint16_t mtu = zwiftServer->getPeerMTU(zwiftConnHandle);
    if (mtu < 3 || reply.size() > size_t(mtu - 3)) {
      Serial.printf("[Ride] response needs MTU >= %u; peer MTU %u; re-pair with MTU exchange\n",
                    (unsigned)(reply.size() + 3), mtu);
      zwiftServer->disconnect(zwiftConnHandle);
      zwiftReplies.clear();
      return;
    }
    zwiftSyncBusy = true;
    zwiftSyncStartedAt = millis();
    zwiftSyncTx->setValue(reply.data(), reply.size());
    zwiftSyncTx->indicate();
    zwiftReplies.pop_front();
  }
  if (zwiftHandshake && millis() - lastZwiftInput >= 1000) sendZwiftInput();
}

static void startZwiftRide() {
  rideEventQueue = xQueueCreate(32, sizeof(RideEvent));
  if (rideEventQueue == nullptr) {
    Serial.println("[Ride] could not allocate event queue; restarting");
    delay(1000);
    ESP.restart();
  }
  NimBLEServer* server = NimBLEDevice::createServer();
  zwiftServer = server;
  server->advertiseOnDisconnect(false);
  server->setCallbacks(&zwiftServerCB, false);
  NimBLEService* service = server->createService(ZWIFT_SERVICE_UUID);
  zwiftAsync = service->createCharacteristic(ZWIFT_ASYNC_UUID, NIMBLE_PROPERTY::NOTIFY);
  zwiftAsync->setCallbacks(&zwiftInputCB);
  NimBLECharacteristic* rx = service->createCharacteristic(
      ZWIFT_SYNC_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(&zwiftSyncRxCB);
  zwiftSyncTx = service->createCharacteristic(
      ZWIFT_SYNC_TX_UUID, NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::NOTIFY);
  zwiftSyncTx->setCallbacks(&zwiftSyncTxCB);
  NimBLECharacteristic* auxiliary = service->createCharacteristic(
      "00000006-19ca-4651-86e5-fa29dcdd09d1", NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE);
  auxiliary->setCallbacks(&zwiftSyncRxCB);
  service->start();
  NimBLEService* battery = server->createService("180f");
  NimBLECharacteristic* level = battery->createCharacteristic(
      "2a19", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t externalPowerLevel = 100;
  level->setValue(&externalPowerLevel, 1);
  battery->start();

  NimBLEService* obcService = server->createService(OBC_BLE_SERVICE_UUID);
  obcBleButton = obcService->createCharacteristic(
      OBC_BLE_BUTTON_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  obcService->createCharacteristic(OBC_BLE_HAPTIC_UUID,
                                   NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  obcService->createCharacteristic(OBC_BLE_APPINFO_UUID,
                                   NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  obcService->start();

  // 29-byte primary payload: flags + 128-bit user-input service data + Ride manufacturer data.
  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(0x06);
  advertisement.setServiceData(ZWIFT_SERVICE_UUID, std::string("\x02", 1));
  advertisement.setManufacturerData(std::string("\x4A\x09\x07\x00\x00", 5));
  NimBLEAdvertisementData scanResponse;
  scanResponse.setCompleteServices(ZWIFT_ADV_UUID);
  scanResponse.setName("Zwift Ride");
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(advertisement);
  adv->setScanResponseData(scanResponse);
  adv->setScanResponse(true);
  adv->start();
  Serial.println("[Ride] experimental Zwift Ride advertising; OBC GATT service retained");
}

static void sendButton(uint8_t buttonId, uint8_t state) {
  if (buttonId == OBC_NONE) return;
  uint8_t msg[3] = { MSG_BUTTON_STATE, buttonId, state };
  notifyObcBle(msg, sizeof(msg));
  updateZwiftButton(buttonId, state);
  Serial.printf("[OBC] button 0x%02X state 0x%02X\n", buttonId, state);
}

static void releasePendingButton(uint8_t buttonId) {
  bool wasPending = false;
  for (size_t i = 0; i < pendingReleases.size();) {
    if (pendingReleases[i].buttonId == buttonId) {
      pendingReleases.erase(pendingReleases.begin() + i);
      wasPending = true;
    } else {
      i++;
    }
  }
  if (wasPending) sendButton(buttonId, STATE_RELEASED);
}

static void fireMomentary(uint8_t buttonId) {
  if (buttonId == OBC_NONE) return;
  releasePendingButton(buttonId);
  sendButton(buttonId, STATE_PRESSED);
  pendingReleases.push_back({ buttonId, millis() + MOMENTARY_PRESS_MS });
}

static void servicePendingReleases() {
  uint32_t now = millis();
  for (size_t i = 0; i < pendingReleases.size();) {
    if ((int32_t)(now - pendingReleases[i].dueAt) >= 0) {
      sendButton(pendingReleases[i].buttonId, STATE_RELEASED);
      pendingReleases.erase(pendingReleases.begin() + i);
    } else {
      i++;
    }
  }
}

static void releaseAllButtons() {
  di2ChannelChanges.reset();
  for (const PendingRelease& pending : pendingReleases) {
    sendButton(pending.buttonId, STATE_RELEASED);
  }
  pendingReleases.clear();

  for (uint8_t i = 0; i < 4; i++) {
    if (heldButton[i] != OBC_NONE) {
      sendButton(heldButton[i], STATE_RELEASED);
      heldButton[i] = OBC_NONE;
    }
    lastPress[i] = PRESS_NONE;
  }
  zwiftDownReleasePending = false;
  zwiftButtons.reset();
  sendZwiftInput();
}

static PressType decodePress(uint8_t value) {
  if (value == DI2_UNMAPPED) return PRESS_NONE;
  if (value & DI2_MASK_DBL) return PRESS_DOUBLE;
  if (value & DI2_MASK_LONG) return PRESS_LONG;
  if (value & DI2_MASK_SHORT) return PRESS_SHORT;
  return PRESS_NONE;
}

static uint8_t mapButton(uint8_t channelIdx, PressType press) {
  const ChannelMap& map = CHANNEL_MAP[channelIdx];
  switch (press) {
    case PRESS_SHORT: return map.shortPress;
    case PRESS_LONG: return map.longPress;
    case PRESS_DOUBLE: return map.doublePress;
    default: return OBC_NONE;
  }
}

static void handleDi2Data(const uint8_t* data, size_t len) {
  if (len < 2) return;
  size_t channels = len - 1;
  if (channels > 4) channels = 4;

  Serial.print("[Di2] raw:");
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", data[i]);
  Serial.println();
  uint8_t changed = di2ChannelChanges.update(data, len);
  for (size_t idx = 0; idx < channels; idx++) {
    if (!(changed & (1U << idx))) continue;
    uint8_t raw = data[idx + 1];
    PressType press = decodePress(raw);
    PressType previous = lastPress[idx];
    Serial.printf("[Di2] ch%u raw 0x%02X\n", (unsigned)(idx + 1), raw);

    if (previous == PRESS_LONG && press != PRESS_LONG) {
      sendButton(heldButton[idx], STATE_RELEASED);
      heldButton[idx] = OBC_NONE;
    }

    if (press == PRESS_SHORT || press == PRESS_DOUBLE) {
      fireMomentary(mapButton(idx, press));
    } else if (press == PRESS_LONG && previous != PRESS_LONG) {
      uint8_t button = mapButton(idx, PRESS_LONG);
      heldButton[idx] = button;
      sendButton(button, STATE_PRESSED);
    }

    lastPress[idx] = press;
  }
}

static void di2NotifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  if (bleEventQueue == nullptr || data == nullptr) return;

  BleEvent event{};
  event.type = BLE_EVENT_DATA;
  event.length = (length > sizeof(event.data)) ? sizeof(event.data) : length;
  memcpy(event.data, data, event.length);
  if (xQueueSend(bleEventQueue, &event, 0) != pdPASS) {
    Serial.println("[BLE] input queue full; dropping Di2 state update");
  }
}

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    Serial.println("[BLE] connected to Di2");
  }

  void onDisconnect(NimBLEClient*) override {
    Serial.println("[BLE] Di2 disconnected");
    if (bleEventQueue == nullptr) return;

    BleEvent event{};
    event.type = BLE_EVENT_DISCONNECTED;
    if (xQueueSend(bleEventQueue, &event, 0) != pdPASS) {
      xQueueReset(bleEventQueue);
      if (xQueueSend(bleEventQueue, &event, 0) != pdPASS) {
        Serial.println("[BLE] unable to queue disconnect state");
      } else {
        Serial.println("[BLE] input queue cleared to deliver disconnect state");
      }
    }
  }
};

static ClientCB clientCB;

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) override {
    if (!device->isAdvertisingService(DI2_SERVICE_UUID)) return;

    Serial.printf("[BLE] found Di2: %s\n", device->getAddress().toString().c_str());
    NimBLEDevice::getScan()->stop();
    NimBLEAdvertisedDevice* copy = new (std::nothrow) NimBLEAdvertisedDevice(*device);
    if (copy == nullptr) {
      Serial.println("[BLE] could not store discovered Di2 device");
      return;
    }

    NimBLEAdvertisedDevice* previous = nullptr;
    portENTER_CRITICAL(&discoveredDeviceMux);
    previous = discoveredDevice;
    discoveredDevice = copy;
    portEXIT_CRITICAL(&discoveredDeviceMux);
    delete previous;
  }
};

static ScanCB scanCB;

static NimBLEAdvertisedDevice* takeDiscoveredDevice() {
  NimBLEAdvertisedDevice* device = nullptr;
  portENTER_CRITICAL(&discoveredDeviceMux);
  device = discoveredDevice;
  discoveredDevice = nullptr;
  portEXIT_CRITICAL(&discoveredDeviceMux);
  return device;
}

static bool hasDiscoveredDevice() {
  bool found;
  portENTER_CRITICAL(&discoveredDeviceMux);
  found = discoveredDevice != nullptr;
  portEXIT_CRITICAL(&discoveredDeviceMux);
  return found;
}

static bool connectToDi2(NimBLEAdvertisedDevice* device) {
  if (device == nullptr) return false;
  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&clientCB, false);
    bleClient->setConnectTimeout(10);
    bleClient->setConnectionParams(6, 12, 0, 200);
  }

  Serial.println("[BLE] connecting...");
  if (!bleClient->connect(device)) {
    Serial.println("[BLE] connect failed");
    return false;
  }
  if (!bleClient->secureConnection()) {
    Serial.println("[BLE] secure/bond failed");
    bleClient->disconnect();
    return false;
  }

  NimBLERemoteService* service = bleClient->getService(DI2_SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("[BLE] Di2 service not found");
    bleClient->disconnect();
    return false;
  }

  bleButton = service->getCharacteristic(DI2_BUTTON_UUID);
  if (bleButton == nullptr) {
    Serial.println("[BLE] button characteristic not found");
    bleClient->disconnect();
    return false;
  }
  if (!bleButton->canIndicate()) {
    Serial.println("[BLE] button characteristic does not support indications");
    bleClient->disconnect();
    return false;
  }
  if (!bleButton->subscribe(false, di2NotifyCB)) {
    Serial.println("[BLE] indication subscription failed");
    bleClient->disconnect();
    return false;
  }

  di2Connected = true;
  Serial.println("[BLE] Di2 bonded and subscribed; ready for D-Fly channel 1-4 input");
  return true;
}

static void processBleEvents() {
  BleEvent event{};
  // Block briefly so button events are handled as soon as they arrive.
  TickType_t wait = pdMS_TO_TICKS(2);
  while (xQueueReceive(bleEventQueue, &event, wait) == pdPASS) {
    wait = 0;
    if (event.type == BLE_EVENT_DATA) {
      handleDi2Data(event.data, event.length);
    } else if (event.type == BLE_EVENT_DISCONNECTED) {
      bleButton = nullptr;
      di2Connected = false;
      releaseAllButtons();
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) delay(10);
  delay(200);
  Serial.println("\nDi2 -> OpenBikeControl bridge starting");

  bleEventQueue = xQueueCreate(16, sizeof(BleEvent));
  if (bleEventQueue == nullptr) {
    Serial.println("[BLE] could not create event queue; restarting");
    delay(1000);
    ESP.restart();
  }

  NimBLEDevice::init("Zwift Ride");
  int mtuResult = NimBLEDevice::setMTU(185);
  if (mtuResult != 0) Serial.printf("[Ride] could not set preferred MTU: %d\n", mtuResult);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  startZwiftRide();
  scan->start(0, nullptr, false);
  Serial.println("[BLE] scanning for Di2...");
}

void loop() {
  serviceZwiftRide();
  processBleEvents();
  servicePendingReleases();

  NimBLEAdvertisedDevice* device = takeDiscoveredDevice();
  if (device != nullptr) {
    if (!di2Connected) connectToDi2(device);
    delete device;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!di2Connected && !hasDiscoveredDevice() && !scan->isScanning()) {
    scan->start(0, nullptr, false);
  }

  delay(1);
}
