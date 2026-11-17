#include <WiFi.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include <new>
#include <vector>
#include "Di2ChannelChanges.h"
#include "ButtonConfig.h"
#include "ConfigPortal.h"

// Di2 D-Fly buttons -> OpenBikeControl over TCP, advertised with mDNS.

static const char* OBC_DEVICE_NAME = "Di2 Bridge";
static const uint16_t OBC_TCP_PORT = 8080;
static const char* OBC_MANUFACTURER = "DIY";
static const char* OBC_MODEL = "ESP32-Di2-Bridge";
static const uint8_t OBC_MAX_CLIENTS = 4;
static const uint32_t MOMENTARY_PRESS_MS = 80;
static const uint32_t STATUS_HEARTBEAT_MS = 30000;

static ChannelMap CHANNEL_MAP[4] = {
  DEFAULT_CHANNEL_MAP[0], DEFAULT_CHANNEL_MAP[1], DEFAULT_CHANNEL_MAP[2], DEFAULT_CHANNEL_MAP[3]
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
static const uint8_t MSG_DEVICE_STATUS = 0x02;
static const uint8_t STATE_RELEASED = 0x00;
static const uint8_t STATE_PRESSED = 0x01;

static WiFiServer tcpServer(OBC_TCP_PORT);
static WiFiClient clients[OBC_MAX_CLIENTS];
static bool networkStarted = false;
static uint32_t lastHeartbeat = 0;

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

static void broadcast(const uint8_t* data, size_t len) {
  for (uint8_t i = 0; i < OBC_MAX_CLIENTS; i++) {
    if (clients[i] && clients[i].connected()) {
      clients[i].write(data, len);
    }
  }
}

static void sendButton(uint8_t buttonId, uint8_t state) {
  if (buttonId == OBC_NONE) return;
  uint8_t msg[3] = { MSG_BUTTON_STATE, buttonId, state };
  broadcast(msg, sizeof(msg));
  Serial.printf("[OBC] button 0x%02X state 0x%02X\n", buttonId, state);
}

static void sendDeviceStatus() {
  uint8_t msg[3] = { MSG_DEVICE_STATUS, 0xFF, (uint8_t)(di2Connected ? 0x01 : 0x00) };
  broadcast(msg, sizeof(msg));
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
}

static void manageTcpClients() {
  WiFiClient incoming = tcpServer.available();
  if (incoming) {
    bool placed = false;
    for (uint8_t i = 0; i < OBC_MAX_CLIENTS; i++) {
      if (!clients[i] || !clients[i].connected()) {
        clients[i].stop();
        clients[i] = incoming;
        clients[i].setNoDelay(true);
        placed = true;
        Serial.printf("[OBC] client connected (slot %u)\n", i);
        uint8_t msg[3] = { MSG_DEVICE_STATUS, 0xFF, (uint8_t)(di2Connected ? 0x01 : 0x00) };
        clients[i].write(msg, sizeof(msg));
        break;
      }
    }
    if (!placed) {
      Serial.println("[OBC] no free client slot, rejecting");
      incoming.stop();
    }
  }

  for (uint8_t i = 0; i < OBC_MAX_CLIENTS; i++) {
    if (clients[i] && clients[i].connected()) {
      while (clients[i].available()) clients[i].read();
    }
  }
}

// Starts the TCP server and mDNS advertisement once Wi-Fi has an address.
static void serviceNetwork() {
  if (!networkStarted) {
    if (WiFi.status() != WL_CONNECTED) return;
    Serial.printf("[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
    tcpServer.begin();
    tcpServer.setNoDelay(true);
    if (!MDNS.begin(ConfigPortal::HOSTNAME)) {
      Serial.println("[mDNS] start failed; TCP server is available only by IP");
    } else {
      MDNS.addService("openbikecontrol", "tcp", OBC_TCP_PORT);
      MDNS.addServiceTxt("openbikecontrol", "tcp", "version", "1");
      MDNS.addServiceTxt("openbikecontrol", "tcp", "id", WiFi.macAddress());
      MDNS.addServiceTxt("openbikecontrol", "tcp", "name", OBC_DEVICE_NAME);
      MDNS.addServiceTxt("openbikecontrol", "tcp", "service-uuids", "d273f680-d548-419d-b9d1-fa0472345229");
      MDNS.addServiceTxt("openbikecontrol", "tcp", "manufacturer", OBC_MANUFACTURER);
      MDNS.addServiceTxt("openbikecontrol", "tcp", "model", OBC_MODEL);
      Serial.printf("[mDNS] _openbikecontrol._tcp on port %u\n", OBC_TCP_PORT);
    }
    networkStarted = true;
  }

  manageTcpClients();

  uint32_t now = millis();
  if (now - lastHeartbeat >= STATUS_HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendDeviceStatus();
  }
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
  sendDeviceStatus();
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
      sendDeviceStatus();
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

  NimBLEDevice::init("Di2 Bridge");
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  ConfigPortal::begin(CHANNEL_MAP);
  scan->start(0, nullptr, false);
  Serial.println("[BLE] scanning for Di2...");
}

void loop() {
  ConfigPortal::handle();
  serviceNetwork();
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
