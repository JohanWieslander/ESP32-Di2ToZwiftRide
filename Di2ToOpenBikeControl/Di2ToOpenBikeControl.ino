#include <WiFi.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include <new>
#include <vector>

static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static const char* OBC_DEVICE_NAME = "Di2 Bridge";
static const char* OBC_HOSTNAME = "di2bridge";
static const uint16_t OBC_TCP_PORT = 8080;
static const char* OBC_MANUFACTURER = "DIY";
static const char* OBC_MODEL = "ESP32-Di2-Bridge";
static const uint8_t OBC_MAX_CLIENTS = 4;
static const uint32_t MOMENTARY_PRESS_MS = 80;
static const uint32_t STATUS_HEARTBEAT_MS = 30000;

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
  OBC_EMOTE = 0x20,
  OBC_SCREENSHOT = 0x24,
  OBC_DIFF_UP = 0x30,
  OBC_DIFF_DOWN = 0x31,
  OBC_LAP = 0x35,
  OBC_NONE = 0x00,
};

struct ChannelMap {
  uint8_t shortPress;
  uint8_t longPress;
  uint8_t doublePress;
};

static const ChannelMap CHANNEL_MAP[4] = {
  { OBC_SHIFT_UP, OBC_DIFF_UP, OBC_SELECT },
  { OBC_SHIFT_DOWN, OBC_DIFF_DOWN, OBC_BACK },
  { OBC_NAV_UP, OBC_MENU, OBC_EMOTE },
  { OBC_NAV_DOWN, OBC_SCREENSHOT, OBC_LAP },
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

WiFiServer tcpServer(OBC_TCP_PORT);
WiFiClient clients[OBC_MAX_CLIENTS];
static NimBLEClient* bleClient = nullptr;
static NimBLERemoteCharacteristic* bleButton = nullptr;
static QueueHandle_t bleEventQueue = nullptr;
static portMUX_TYPE discoveredDeviceMux = portMUX_INITIALIZER_UNLOCKED;
static NimBLEAdvertisedDevice* discoveredDevice = nullptr;
static bool di2Connected = false;
static PressType lastPress[4] = { PRESS_NONE, PRESS_NONE, PRESS_NONE, PRESS_NONE };
static uint8_t heldButton[4] = { OBC_NONE, OBC_NONE, OBC_NONE, OBC_NONE };

struct PendingRelease {
  uint8_t buttonId;
  uint32_t dueAt;
};

static std::vector<PendingRelease> pendingReleases;
static uint32_t lastHeartbeat = 0;

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
  uint8_t connected = di2Connected ? 0x01 : 0x00;
  uint8_t msg[3] = { MSG_DEVICE_STATUS, 0xFF, connected };
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

  for (size_t idx = 0; idx < channels; idx++) {
    uint8_t raw = data[idx + 1];
    if (raw == DI2_UNMAPPED) continue;

    PressType press = decodePress(raw);
    PressType previous = lastPress[idx];
    if (press == previous) continue;

    if (previous == PRESS_LONG && press != PRESS_LONG) {
      sendButton(heldButton[idx], STATE_RELEASED);
      heldButton[idx] = OBC_NONE;
    }

    if (press == PRESS_SHORT || press == PRESS_DOUBLE) {
      fireMomentary(mapButton(idx, press));
    } else if (press == PRESS_LONG) {
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
  Serial.println("[BLE] subscribed to Di2 button indications");
  sendDeviceStatus();
  return true;
}

static void processBleEvents() {
  BleEvent event{};
  while (xQueueReceive(bleEventQueue, &event, 0) == pdPASS) {
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

static void startWifiAndMdns() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OBC_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\n[WiFi] IP %s\n", WiFi.localIP().toString().c_str());

  tcpServer.begin();
  tcpServer.setNoDelay(true);
  if (!MDNS.begin(OBC_HOSTNAME)) {
    Serial.println("[mDNS] start failed; TCP server is available only by IP");
    return;
  }
  MDNS.addService("openbikecontrol", "tcp", OBC_TCP_PORT);
  MDNS.addServiceTxt("openbikecontrol", "tcp", "version", "1");
  MDNS.addServiceTxt("openbikecontrol", "tcp", "id", WiFi.macAddress());
  MDNS.addServiceTxt("openbikecontrol", "tcp", "name", OBC_DEVICE_NAME);
  MDNS.addServiceTxt("openbikecontrol", "tcp", "service-uuids", "d273f680-d548-419d-b9d1-fa0472345229");
  MDNS.addServiceTxt("openbikecontrol", "tcp", "manufacturer", OBC_MANUFACTURER);
  MDNS.addServiceTxt("openbikecontrol", "tcp", "model", OBC_MODEL);
  Serial.printf("[mDNS] _openbikecontrol._tcp on port %u\n", OBC_TCP_PORT);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nDi2 -> OpenBikeControl bridge starting");

  bleEventQueue = xQueueCreate(16, sizeof(BleEvent));
  if (bleEventQueue == nullptr) {
    Serial.println("[BLE] could not create event queue; restarting");
    delay(1000);
    ESP.restart();
  }

  startWifiAndMdns();

  NimBLEDevice::init(OBC_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  scan->start(0, nullptr, false);
  Serial.println("[BLE] scanning for Di2...");
}

void loop() {
  processBleEvents();
  manageTcpClients();
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

  uint32_t now = millis();
  if (now - lastHeartbeat >= STATUS_HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendDeviceStatus();
  }
  delay(5);
}
