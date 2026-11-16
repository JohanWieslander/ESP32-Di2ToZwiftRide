#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include <new>
#include <vector>

// Zwift cannot use mDNS/TCP, so by default the bridge emulates a Zwift Click
// over BLE. Build with -DENABLE_WIFI_OBC=1 to use the Wi-Fi OpenBikeControl endpoint instead.
#ifndef ENABLE_WIFI_OBC
#define ENABLE_WIFI_OBC 0
#endif

static const char* OBC_DEVICE_NAME = "Di2 Bridge";
static const char* OBC_HOSTNAME = "di2bridge";
static const char* WIFI_PREFERENCES_NAMESPACE = "di2bridge";
static const char* WIFI_SSID_KEY = "ssid";
static const char* WIFI_PASSWORD_KEY = "password";
static const uint16_t OBC_TCP_PORT = 8080;
static const char* OBC_MANUFACTURER = "DIY";
static const char* OBC_MODEL = "ESP32-Di2-Bridge";
static const uint8_t OBC_MAX_CLIENTS = 4;
static const uint32_t MOMENTARY_PRESS_MS = 80;
static const uint32_t STATUS_HEARTBEAT_MS = 30000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 120000;
static const uint32_t WIFI_DISCONNECT_RESET_MS = 10000;

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
static DNSServer dnsServer;
static WebServer portalServer(80);
static Preferences wifiPreferences;
static bool networkReady = false;
static bool portalRunning = false;
static uint32_t wifiDisconnectedAt = 0;
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


// ---- Zwift Click emulation (BLE peripheral) ----
static NimBLEUUID ZWIFT_SERVICE_UUID("0000fc82-0000-1000-8000-00805f9b34fb");
static const char* ZWIFT_ASYNC_UUID = "00000002-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZWIFT_SYNC_RX_UUID = "00000003-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZWIFT_SYNC_TX_UUID = "00000004-19ca-4651-86e5-fa29dcdd09d1";
static const uint8_t ZWIFT_RIDE_ON[] = { 'R', 'i', 'd', 'e', 'O', 'n' };
static const uint8_t ZWIFT_MSG_CLICK_STATE = 0x37;
static const uint8_t ZWIFT_PRESSED = 0x00;
static const uint8_t ZWIFT_RELEASED = 0x01;
static NimBLECharacteristic* zwiftAsync = nullptr;
static NimBLECharacteristic* zwiftSyncTx = nullptr;
static bool zwiftConnected = false;
static bool zwiftPlus = false;
static bool zwiftMinus = false;

static void sendZwiftClick() {
  if (zwiftAsync == nullptr || !zwiftConnected) return;
  uint8_t msg[5] = { ZWIFT_MSG_CLICK_STATE, 0x08, (uint8_t)(zwiftPlus ? ZWIFT_PRESSED : ZWIFT_RELEASED),
                     0x10, (uint8_t)(zwiftMinus ? ZWIFT_PRESSED : ZWIFT_RELEASED) };
  zwiftAsync->setValue(msg, sizeof(msg));
  zwiftAsync->notify();
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
  bool pressed = state == STATE_PRESSED;
  if (buttonId == OBC_SHIFT_UP) zwiftPlus = pressed;
  else if (buttonId == OBC_SHIFT_DOWN) zwiftMinus = pressed;
  else return;
  sendZwiftClick();
}

class ZwiftServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    zwiftConnected = true;
    obcBleConnected = true;
    Serial.println("[BLE] central connected");
    NimBLEDevice::getAdvertising()->start();
  }
  void onDisconnect(NimBLEServer*) override {
    zwiftConnected = false;
    obcBleConnected = false;
    zwiftPlus = zwiftMinus = false;
    Serial.println("[BLE] central disconnected; advertising again");
    NimBLEDevice::getAdvertising()->start();
  }
};

class ZwiftSyncRxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string value = c->getValue();
    if (value.size() >= sizeof(ZWIFT_RIDE_ON) &&
        memcmp(value.data(), ZWIFT_RIDE_ON, sizeof(ZWIFT_RIDE_ON)) == 0) {
      const uint8_t reply[] = { 'R', 'i', 'd', 'e', 'O', 'n', 0x01, 0x03 };
      zwiftSyncTx->setValue(reply, sizeof(reply));
      zwiftSyncTx->indicate();
      Serial.println("[Zwift] RideOn handshake");
    }
  }
};

static ZwiftServerCB zwiftServerCB;
static ZwiftSyncRxCB zwiftSyncRxCB;

static void startZwiftClick() {
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&zwiftServerCB);
  NimBLEService* service = server->createService(ZWIFT_SERVICE_UUID);
  zwiftAsync = service->createCharacteristic(ZWIFT_ASYNC_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* rx = service->createCharacteristic(
      ZWIFT_SYNC_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(&zwiftSyncRxCB);
  zwiftSyncTx = service->createCharacteristic(ZWIFT_SYNC_TX_UUID, NIMBLE_PROPERTY::INDICATE);
  service->start();

  NimBLEService* obcService = server->createService(OBC_BLE_SERVICE_UUID);
  obcBleButton = obcService->createCharacteristic(
      OBC_BLE_BUTTON_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  obcService->createCharacteristic(OBC_BLE_HAPTIC_UUID,
                                   NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  obcService->createCharacteristic(OBC_BLE_APPINFO_UUID,
                                   NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  obcService->start();

  // Zwift manufacturer id 0x094A (little endian) + Click device type 0x09
  std::string mfg("\x4A\x09\x09", 3);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(ZWIFT_SERVICE_UUID);
  adv->addServiceUUID(OBC_BLE_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setManufacturerData(mfg);
  adv->setName("Zwift Click");
  adv->start();
  Serial.println("[BLE] advertising Zwift Click + OpenBikeControl services");
}

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
  notifyObcBle(msg, sizeof(msg));
  updateZwiftButton(buttonId, state);
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
    PressType press = decodePress(raw);  // 0xF0 (idle) decodes to PRESS_NONE and re-arms the channel
    Serial.printf("[Di2] ch%u raw 0x%02X\n", (unsigned)(idx + 1), raw);
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

static void handleWifiPortalRoot() {
  static const char page[] =
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Di2 Bridge Wi-Fi</title></head><body><h1>Di2 Bridge Wi-Fi setup</h1>"
      "<form method='post' action='/save'>"
      "<label>Wi-Fi name (SSID)<br><input name='ssid' maxlength='32' required></label><br><br>"
      "<label>Wi-Fi password<br><input name='password' type='password' maxlength='64'></label><br><br>"
      "<button type='submit'>Save and connect</button></form></body></html>";
  portalServer.send(200, "text/html", page);
}

static void redirectToWifiPortal() {
  portalServer.sendHeader("Location", "/", true);
  portalServer.send(302, "text/plain", "");
}

static void handleWifiPortalSave() {
  if (!portalServer.hasArg("ssid") || !portalServer.hasArg("password")) {
    portalServer.send(400, "text/plain", "SSID and password fields are required.");
    return;
  }

  String ssid = portalServer.arg("ssid");
  String password = portalServer.arg("password");
  if (ssid.length() == 0 || ssid.length() > 32 ||
      (password.length() != 0 && (password.length() < 8 || password.length() > 64))) {
    portalServer.send(400, "text/plain", "SSID or password length is invalid.");
    return;
  }
  if (!wifiPreferences.begin(WIFI_PREFERENCES_NAMESPACE, false)) {
    portalServer.send(500, "text/plain", "Could not open Wi-Fi settings storage.");
    return;
  }

  wifiPreferences.putString(WIFI_SSID_KEY, ssid);
  wifiPreferences.putString(WIFI_PASSWORD_KEY, password);
  bool credentialsStored = wifiPreferences.getString(WIFI_SSID_KEY, "") == ssid &&
                          wifiPreferences.getString(WIFI_PASSWORD_KEY, "") == password;
  wifiPreferences.end();
  if (!credentialsStored) {
    portalServer.send(500, "text/plain", "Could not save Wi-Fi settings.");
    return;
  }
  portalServer.send(200, "text/html",
                    "<!doctype html><html><body><p>Saved. The bridge is restarting and will connect to Wi-Fi.</p></body></html>");
  delay(500);
  ESP.restart();
}

static bool startWifiSetupPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(OBC_HOSTNAME);

  char setupSsid[32];
  uint64_t chipId = ESP.getEfuseMac();
  snprintf(setupSsid, sizeof(setupSsid), "Di2-Bridge-%04X", static_cast<unsigned int>(chipId & 0xFFFF));
  if (!WiFi.softAP(setupSsid, nullptr, 1, false, 1)) {
    Serial.println("[WiFi] could not start setup access point");
    return false;
  }

  IPAddress portalIp = WiFi.softAPIP();
  if (!dnsServer.start(53, "*", portalIp)) {
    Serial.println("[WiFi] captive DNS failed; open http://192.168.4.1 manually");
  }
  portalServer.on("/", HTTP_GET, handleWifiPortalRoot);
  portalServer.on("/save", HTTP_POST, handleWifiPortalSave);
  portalServer.onNotFound(redirectToWifiPortal);
  portalServer.begin();
  portalRunning = true;
  Serial.printf("[WiFi] setup network: %s\n", setupSsid);
  Serial.printf("[WiFi] open http://%s to configure network access\n", portalIp.toString().c_str());
  return true;
}

static bool startWifiAndMdns() {
  if (!wifiPreferences.begin(WIFI_PREFERENCES_NAMESPACE, false)) {
    Serial.println("[WiFi] could not open settings storage; starting setup portal");
    if (!startWifiSetupPortal()) {
      delay(1000);
      ESP.restart();
    }
    return false;
  }
  String ssid = wifiPreferences.isKey(WIFI_SSID_KEY)
                    ? wifiPreferences.getString(WIFI_SSID_KEY, "")
                    : "";
  String password = wifiPreferences.isKey(WIFI_PASSWORD_KEY)
                        ? wifiPreferences.getString(WIFI_PASSWORD_KEY, "")
                        : "";
  wifiPreferences.end();

  if (ssid.isEmpty()) {
    Serial.println("[WiFi] no saved network; starting setup portal");
    if (!startWifiSetupPortal()) {
      delay(1000);
      ESP.restart();
    }
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OBC_HOSTNAME);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("[WiFi] connecting to %s", ssid.c_str());
  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] connection timed out; keeping saved network and starting setup portal");
    if (!startWifiSetupPortal()) {
      delay(1000);
      ESP.restart();
    }
    return false;
  }

  Serial.printf("\n[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  if (!MDNS.begin(OBC_HOSTNAME)) {
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
  networkReady = true;
  return true;
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

#if ENABLE_WIFI_OBC
  if (!startWifiAndMdns()) return;
#else
  networkReady = true;
#endif

  NimBLEDevice::init(OBC_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  startZwiftClick();
  scan->start(0, nullptr, false);
  Serial.println("[BLE] scanning for Di2...");
}

void loop() {
  if (!networkReady) {
    if (portalRunning) {
      dnsServer.processNextRequest();
      portalServer.handleClient();
    }
    delay(2);
    return;
  }

#if ENABLE_WIFI_OBC
  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedAt = 0;
  } else if (wifiDisconnectedAt == 0) {
    wifiDisconnectedAt = millis();
  } else if (millis() - wifiDisconnectedAt >= WIFI_DISCONNECT_RESET_MS) {
    Serial.println("[WiFi] connection lost; restarting");
    delay(500);
    ESP.restart();
  }
  manageTcpClients();
#endif

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

  uint32_t now = millis();
  if (now - lastHeartbeat >= STATUS_HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendDeviceStatus();
  }
  delay(1);
}
