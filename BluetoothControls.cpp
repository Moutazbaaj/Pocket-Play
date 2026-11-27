#include "BluetoothControls.h"

#include <Arduino.h>
#include <Preferences.h>
#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_ROLE_CENTRAL
#include <array>
#include <esp_heap_caps.h>
#if __has_include(<esp32-hal-alloc-ble-mem.h>)
// Arduino must keep BLE memory reserved before setup() for direct NimBLE users.
#include <esp32-hal-alloc-ble-mem.h>
#elif __has_include(<esp32-hal-bt-mem.h>)
#include <esp32-hal-bt-mem.h>
#endif
#include "HidGamepad.h"

extern "C" {
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
void ble_store_config_init(void);
}

namespace {
using namespace BluetoothControls;
constexpr uint16_t kHid = 0x1812;
constexpr uint16_t kReportMap = 0x2A4B;
constexpr uint16_t kReport = 0x2A4D;
constexpr uint16_t kPnpId = 0x2A50;
constexpr uint16_t kReportReference = 0x2908;
constexpr uint16_t kClientConfig = 0x2902;
constexpr uint16_t kNoLink = BLE_HS_CONN_HANDLE_NONE;
constexpr uint32_t kScanMs = 6000;
constexpr uint32_t kPairMs = 25000;
constexpr uint32_t kSyncTimeoutMs = 10000;
constexpr char kEnabledKey[] = "bleCtl";
#if CONFIG_IDF_TARGET_ESP32S3
// The S3 controller requests a 32 KiB block after earlier allocations.
// Allow headroom for those allocations and the remaining host/controller startup.
constexpr size_t kMinInternalHeap = 64 * 1024;
constexpr size_t kMinInternalBlock = 40 * 1024;
#endif

struct Channel {
  uint16_t value = 0;
  uint16_t end = 0;
  uint16_t reportRef = 0;
  uint16_t cccd = 0;
  uint8_t id = 0;
  bool indication = false;
  bool subscribed = false;
};

enum class Command { None, Scan, Pair, Disconnect };
struct Request {
  Command action = Command::None;
  ble_addr_t address{};
  char name[25] = {};
};

Preferences *settings = nullptr;
bool radioRunning = false;
std::atomic<bool> radioWanted{false};
std::atomic<bool> saveController{false};
enum class HostSync { Pending, Ready, NoAddress };
std::atomic<HostSync> hostSync{HostSync::Pending};
bool awaitingStartup = false;
uint32_t startupAt = 0;
struct SavedController { ble_addr_t address; char name[25]; };
SavedController lastController{};
SavedController discoveredBond{};
portMUX_TYPE uiLock = portMUX_INITIALIZER_UNLOCKED;
Snapshot ui;
std::array<ble_addr_t, maxDevices> candidates;
Request pending;
ControllerButtons buttons;

// NimBLE owns these connection, discovery, and decoder fields on its host task.
ble_npl_event commandEvent;
ble_npl_callout pairTimeout;
uint8_t localAddressType = 0;
uint16_t link = kNoLink;
bool connecting = false;
bool closing = false;
bool failed = false;
bool securityReady = false;
bool securityRetried = false;
bool acceptingInput = false;
bool xbox = false;
char peerName[25];
uint16_t serviceStart, serviceEnd, reportMapHandle;
std::array<Channel, 12> channels;
size_t channelCount, channelIndex;
int openChannel;
std::array<uint8_t, 768> reportMap;
size_t mapLength;
HidGamepad decoder;

int gapEvent(ble_gap_event *event, void *arg);
void nextChannel();
void discoverHid();
void connectController(const Request &request);

void setStatus(State state, const char *text) {
  portENTER_CRITICAL(&uiLock);
  ui.state = state;
  ui.status = text;
  if (state == State::Connected) memcpy(ui.connected, peerName, sizeof(ui.connected));
  else ui.connected[0] = 0;
  ++ui.revision;
  portEXIT_CRITICAL(&uiLock);
}

bool activeLink(uint16_t handle) { return link == handle && !closing; }

void endLink() {
  acceptingInput = false;
  buttons.clear();
  ble_npl_callout_stop(&pairTimeout);
  closing = true;
  if (link != kNoLink) ble_gap_terminate(link, BLE_ERR_REM_USER_CONN_TERM);
  else if (connecting) ble_gap_conn_cancel();
  else closing = false;
}

void pairingError(const char *text) {
  failed = true;
  setStatus(State::Error, text);
  endLink();
}

void timedOut(ble_npl_event *) { pairingError("Pairing timed out"); }

bool isXboxName(const char *name) { return strncasecmp(name, "Xbox", 4) == 0; }

void addAdvertiser(const ble_gap_disc_desc &peer) {
  if (peer.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
      peer.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
      peer.event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return;
  ble_hs_adv_fields adv{};
  if (ble_hs_adv_parse_fields(&adv, peer.data, peer.length_data)) return;
  char name[25] = {};
  if (adv.name && adv.name_len) {
    const size_t count = std::min<size_t>(adv.name_len, sizeof(name) - 1);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t ch = adv.name[i];
      name[i] = ch >= 32 && ch <= 126 ? char(ch) : '?';
    }
  }
  bool hid = isXboxName(name) || (adv.appearance_is_present &&
                                  (adv.appearance == 0x03C3 || adv.appearance == 0x03C4));
  for (int i = 0; i < adv.num_uuids16; ++i) hid |= adv.uuids16[i].value == kHid;

  char fallback[25] = {};
  snprintf(fallback, sizeof(fallback), "HID %02X:%02X:%02X", peer.addr.val[2], peer.addr.val[1], peer.addr.val[0]);
  portENTER_CRITICAL(&uiLock);
  if (ui.state == State::Scanning) {
    int i = 0;
    while (i < ui.count && ble_addr_cmp(&candidates[i], &peer.addr)) ++i;
    // A name can arrive in a later scan response from an already listed controller.
    if (i < ui.count && name[0] && strcmp(ui.devices[i], name)) {
      memcpy(ui.devices[i], name, sizeof(name));
      ++ui.revision;
    } else if (i == ui.count && i < maxDevices && hid) {
      candidates[i] = peer.addr;
      memcpy(ui.devices[i], name[0] ? name : fallback, sizeof(name));
      ++ui.count;
      ++ui.revision;
    }
  }
  portEXIT_CRITICAL(&uiLock);
}

int configWritten(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *, void *) {
  if (!activeLink(handle)) return 0;
  channels[channelIndex].subscribed = error->status == 0;
  ++channelIndex;
  nextChannel();
  return 0;
}

int referenceRead(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *attribute, void *) {
  if (!activeLink(handle)) return 0;
  auto &channel = channels[channelIndex];
  uint8_t reference[2] = {};
  uint16_t length = 0;
  if (!error->status && attribute &&
      !ble_hs_mbuf_to_flat(attribute->om, reference, sizeof(reference), &length) &&
      length == sizeof(reference) && reference[1] == 1 && decoder.hasInputs(reference[0])) {
    channel.id = reference[0];
    const uint8_t enable[2] = {uint8_t(channel.indication ? 2 : 1), 0};
    if (!ble_gattc_write_flat(link, channel.cccd, enable, sizeof(enable), configWritten, nullptr)) return 0;
  }
  ++channelIndex;
  nextChannel();
  return 0;
}

int descriptorFound(uint16_t handle, const ble_gatt_error *error, uint16_t,
                    const ble_gatt_dsc *descriptor, void *) {
  if (!activeLink(handle)) return 0;
  auto &channel = channels[channelIndex];
  if (!error->status && descriptor) {
    const uint16_t id = ble_uuid_u16(&descriptor->uuid.u);
    if (id == kReportReference) channel.reportRef = descriptor->handle;
    if (id == kClientConfig) channel.cccd = descriptor->handle;
    return 0;
  }
  if (channel.reportRef && channel.cccd &&
      !ble_gattc_read(link, channel.reportRef, referenceRead, nullptr)) return 0;
  ++channelIndex;
  nextChannel();
  return 0;
}

void nextChannel() {
  while (channelIndex < channelCount) {
    const auto &channel = channels[channelIndex];
    if (channel.value < channel.end &&
        !ble_gattc_disc_all_dscs(link, channel.value, channel.end, descriptorFound, nullptr)) return;
    ++channelIndex;
  }
  for (size_t i = 0; i < channelCount; ++i) {
    if (!channels[i].subscribed) continue;
    ble_npl_callout_stop(&pairTimeout);
    ble_gap_conn_desc connection{};
    if (!ble_gap_conn_find(link, &connection) && connection.sec_state.bonded) {
      portENTER_CRITICAL(&uiLock);
      discoveredBond.address = connection.peer_id_addr;
      discoveredBond.address.type &= 1;
      memcpy(discoveredBond.name, peerName, sizeof(peerName));
      portEXIT_CRITICAL(&uiLock);
      saveController.store(true, std::memory_order_release);
    }
    acceptingInput = true;
    setStatus(State::Connected, "Controller connected");
    return;
  }
  pairingError("No usable input reports");
}

int mapRead(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *attribute, void *) {
  if (!activeLink(handle)) return 0;
  if (!error->status && attribute) {
    uint16_t copied = 0;
    if (mapLength > reportMap.size() || ble_hs_mbuf_to_flat(attribute->om, reportMap.data() + mapLength,
                                                         reportMap.size() - mapLength, &copied)) {
      pairingError("Controller report map too large");
      return BLE_HS_EAPP;
    }
    mapLength += copied;
  } else if (error->status == BLE_HS_EDONE &&
             decoder.load(reportMap.data(), mapLength,
                          xbox ? HidGamepad::Mapping::XboxBle : HidGamepad::Mapping::Generic)) {
    channelIndex = 0;
    nextChannel();
  } else {
    pairingError("Unsupported gamepad layout");
  }
  return 0;
}

int characteristicFound(uint16_t handle, const ble_gatt_error *error, const ble_gatt_chr *chr, void *) {
  if (!activeLink(handle)) return 0;
  if (!error->status && chr) {
    if (openChannel >= 0) {
      channels[openChannel].end = chr->def_handle - 1;
      openChannel = -1;
    }
    const uint16_t id = ble_uuid_u16(&chr->uuid.u);
    if (id == kReportMap) reportMapHandle = chr->val_handle;
    if (id == kReport && channelCount < channels.size() &&
        (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
      openChannel = int(channelCount++);
      channels[openChannel] = {chr->val_handle, serviceEnd, 0, 0, 0,
                               !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY), false};
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || !reportMapHandle || !channelCount ||
      ble_gattc_read_long(link, reportMapHandle, 0, mapRead, nullptr)) {
    pairingError("Could not read HID reports");
  }
  return 0;
}

int serviceFound(uint16_t handle, const ble_gatt_error *error, const ble_gatt_svc *service, void *) {
  if (!activeLink(handle)) return 0;
  if (!error->status && service) {
    if (!serviceStart) {
      serviceStart = service->start_handle;
      serviceEnd = service->end_handle;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || !serviceStart ||
      ble_gattc_disc_all_chrs(link, serviceStart, serviceEnd, characteristicFound, nullptr)) {
    pairingError("No Bluetooth HID service");
  }
  return 0;
}

void discoverHid() {
  static const ble_uuid16_t hidUuid = BLE_UUID16_INIT(kHid);
  if (ble_gattc_disc_svc_by_uuid(link, &hidUuid.u, serviceFound, nullptr)) {
    pairingError("Could not discover controller");
  }
}

int pnpRead(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *attribute, void *) {
  if (!activeLink(handle)) return 0;
  if (!error->status && attribute) {
    uint8_t pnp[7];
    uint16_t length = 0;
    if (!ble_hs_mbuf_to_flat(attribute->om, pnp, sizeof(pnp), &length) && length == sizeof(pnp)) {
      xbox |= (pnp[0] == 2 && pnp[1] == 0x5E && pnp[2] == 0x04) ||
              (pnp[0] == 1 && pnp[1] == 0x06 && pnp[2] == 0);
    }
  } else {
    discoverHid();
  }
  return 0;
}

void encrypted() {
  if (securityReady) return;
  securityReady = true;
  static const ble_uuid16_t pnpUuid = BLE_UUID16_INIT(kPnpId);
  if (ble_gattc_read_by_uuid(link, 1, UINT16_MAX, &pnpUuid.u, pnpRead, nullptr)) discoverHid();
}

void inputReport(const ble_gap_event &event) {
  if (!acceptingInput || !activeLink(event.notify_rx.conn_handle)) return;
  for (size_t i = 0; i < channelCount; ++i) {
    const auto &channel = channels[i];
    if (channel.value != event.notify_rx.attr_handle || !channel.subscribed) continue;
    uint8_t data[64];
    uint16_t length = 0;
    if (!ble_hs_mbuf_to_flat(event.notify_rx.om, data, sizeof(data), &length)) {
      buttons.publish(decoder.report(data, length, channel.id));
    }
    return;
  }
}

int gapEvent(ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: addAdvertiser(event->disc); break;
    case BLE_GAP_EVENT_DISC_COMPLETE: {
      const auto now = status();
      if (now.state == State::Scanning) setStatus(State::Ready, now.count ? "Select a controller" : "No BLE controllers found");
      break;
    }
    case BLE_GAP_EVENT_CONNECT:
      connecting = false;
      if (event->connect.status) {
        const bool cancelled = closing && !failed;
        closing = false;
        if (cancelled) setStatus(State::Ready, "Pairing cancelled");
        else if (!failed) pairingError("Could not connect");
      } else {
        link = event->connect.conn_handle;
        if (closing) {
          endLink();
        } else {
          ble_gap_conn_desc current{};
          if (!ble_gap_conn_find(link, &current) && current.sec_state.encrypted) {
            encrypted();
          } else {
            const int result = ble_gap_security_initiate(link);
            if (result && result != BLE_HS_EALREADY) pairingError("Could not start pairing");
          }
        }
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (activeLink(event->enc_change.conn_handle)) {
        const int result = event->enc_change.status;
        if (!result) {
          encrypted();
        } else if (result == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) && !securityRetried) {
          securityRetried = true;
          if (ble_gap_security_initiate(link)) pairingError("Bluetooth pairing failed");
        } else {
          pairingError("Bluetooth pairing failed");
        }
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle == link) {
        link = kNoLink;
        closing = acceptingInput = false;
        buttons.clear();
        ble_npl_callout_stop(&pairTimeout);
        if (!failed) setStatus(State::Ready, "Controller disconnected");
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: inputReport(*event); break;
    default: break;
  }
  return 0;
}

void runCommand(ble_npl_event *) {
  portENTER_CRITICAL(&uiLock);
  const Request request = pending;
  pending.action = Command::None;
  portEXIT_CRITICAL(&uiLock);
  if (request.action == Command::Scan && link == kNoLink && !connecting) {
    portENTER_CRITICAL(&uiLock);
    ui.count = 0;
    portEXIT_CRITICAL(&uiLock);
    setStatus(State::Scanning, "Put the controller in pairing mode");
    ble_gap_disc_params params{};
    params.passive = 0;
    params.filter_duplicates = 0;
    if (ble_gap_disc(localAddressType, kScanMs, &params, gapEvent, nullptr)) {
      setStatus(State::Error, "Could not scan for controllers");
    }
  } else if (request.action == Command::Pair && link == kNoLink && !connecting) {
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    connectController(request);
  } else if (request.action == Command::Disconnect) {
    if (ble_gap_disc_active()) {
      ble_gap_disc_cancel();
      setStatus(State::Ready, "Scan cancelled");
    } else if (link != kNoLink || connecting) {
      setStatus(State::Disconnecting, "Disconnecting controller");
      endLink();
    }
  }
}

void connectController(const Request &request) {
  buttons.clear();
  failed = closing = securityReady = securityRetried = acceptingInput = false;
  memcpy(peerName, request.name, sizeof(peerName));
  xbox = isXboxName(peerName);
  serviceStart = serviceEnd = reportMapHandle = 0;
  channelCount = mapLength = 0;
  openChannel = -1;
  setStatus(State::Pairing, "Connecting and reading controls");
  connecting = ble_gap_connect(localAddressType, &request.address, 8000, nullptr, gapEvent, nullptr) == 0;
  if (connecting) ble_npl_callout_reset(&pairTimeout, ble_npl_time_ms_to_ticks32(kPairMs));
  else pairingError("Could not connect");
}

void hostReset(int) {
  buttons.clear();
  link = kNoLink;
  connecting = closing = acceptingInput = false;
  ble_npl_callout_stop(&pairTimeout);
  hostSync.store(HostSync::Pending);
  setStatus(State::Starting, "Restarting Bluetooth");
}

void hostSynced() {
  if (ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0, &localAddressType)) {
    hostSync.store(HostSync::NoAddress);
    return;
  }
  Request reconnect;
  portENTER_CRITICAL(&uiLock);
  reconnect.address = lastController.address;
  memcpy(reconnect.name, lastController.name, sizeof(reconnect.name));
  portEXIT_CRITICAL(&uiLock);
  if (reconnect.name[0]) connectController(reconnect);
  else setStatus(State::Ready, "Scan to pair a BLE controller");
  hostSync.store(HostSync::Ready);
}

void hostTask(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void startError(const char *message, int error = 0) {
  radioWanted.store(false);
  setStatus(State::Error, message);
  if (error) Serial.printf("Bluetooth: %s (%d)\n", message, error);
  else Serial.printf("Bluetooth: %s\n", message);
}

void startRadio() {
  if (radioRunning) return;
  // Leave Bluetooth off on the next boot if startup aborts or resets the chip.
  if (settings && settings->getBool(kEnabledKey, false) && !settings->putBool(kEnabledKey, false)) {
    startError("Could not save Bluetooth recovery");
    return;
  }
  hostSync.store(HostSync::Pending);
  startupAt = millis();
  setStatus(State::Starting, "Starting Bluetooth");
  constexpr uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const size_t freeInternal = heap_caps_get_free_size(internalCaps);
  const size_t largestInternalBlock = heap_caps_get_largest_free_block(internalCaps);
  Serial.printf("Bluetooth: internal heap %u bytes free, largest block %u bytes\n",
                unsigned(freeInternal), unsigned(largestInternalBlock));
#if CONFIG_IDF_TARGET_ESP32S3
  if (freeInternal < kMinInternalHeap || largestInternalBlock < kMinInternalBlock) {
    startError("Low internal RAM. Restart to retry");
    return;
  }
#endif
  const int initResult = nimble_port_init();
  if (initResult != ESP_OK) {
    startError(initResult == ESP_ERR_NO_MEM ? "Bluetooth needs more internal RAM" : "Could not start Bluetooth",
               initResult);
    return;
  }
  link = kNoLink;
  connecting = closing = failed = false;
  ble_hs_cfg.reset_cb = hostReset;
  ble_hs_cfg.sync_cb = hostSynced;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_store_config_init();
  const int timerResult = ble_npl_callout_init(&pairTimeout, nimble_port_get_dflt_eventq(), timedOut, nullptr);
  if (timerResult) {
    nimble_port_deinit();
    startError("Could not start pairing timer", timerResult);
    return;
  }
  ble_npl_event_init(&commandEvent, runCommand, nullptr);
  const int taskResult = esp_nimble_enable(reinterpret_cast<void *>(hostTask));
  if (taskResult != ESP_OK) {
    ble_npl_callout_deinit(&pairTimeout);
    ble_npl_event_deinit(&commandEvent);
    nimble_port_deinit();
    startError("Could not start Bluetooth task", taskResult);
    return;
  }
  radioRunning = true;
  awaitingStartup = true;
}

bool stopRadio() {
  awaitingStartup = false;
  buttons.clear();
  if (radioRunning) {
    if (nimble_port_stop() != 0) {
      setStatus(State::Error, "Could not stop Bluetooth");
      return false;
    }
    ble_npl_callout_stop(&pairTimeout);
    ble_npl_callout_deinit(&pairTimeout);
    ble_npl_event_deinit(&commandEvent);
    nimble_port_deinit();
    radioRunning = false;
  }
  portENTER_CRITICAL(&uiLock);
  ui.count = 0;
  pending.action = Command::None;
  portEXIT_CRITICAL(&uiLock);
  buttons.clear();
  setStatus(State::Off, "Bluetooth is off");
  return true;
}

void serviceStartup() {
  if (!awaitingStartup) return;
  const auto synced = hostSync.load();
  if (synced == HostSync::Ready) {
    // Let an automatic reconnect finish before enabling it on the next boot.
    portENTER_CRITICAL(&uiLock);
    const bool reconnecting = ui.state == State::Pairing || ui.state == State::Starting;
    portEXIT_CRITICAL(&uiLock);
    if (reconnecting) return;
    awaitingStartup = false;
    if (settings && !settings->putBool(kEnabledKey, true)) {
      Serial.println("Bluetooth: could not remember enabled setting");
    }
    Serial.println("Bluetooth: startup complete");
  } else if (synced == HostSync::NoAddress || millis() - startupAt >= kSyncTimeoutMs) {
    radioWanted.store(false);
    if (stopRadio()) {
      startError(synced == HostSync::NoAddress ? "Bluetooth address unavailable" : "Bluetooth startup timed out");
    }
  }
}

bool send(Command action, int index = -1) {
  if (!radioRunning) return false;
  bool queued = false;
  portENTER_CRITICAL(&uiLock);
  const bool ready = ui.state == State::Ready || ui.state == State::Error;
  const bool allowed = (action == Command::Scan && ready) ||
                       (action == Command::Pair && ready && index >= 0 && index < ui.count) ||
                       (action == Command::Disconnect && (ui.state == State::Scanning ||
                        ui.state == State::Pairing || ui.state == State::Connected));
  if (pending.action == Command::None && allowed) {
    pending.action = action;
    if (action == Command::Pair) {
      pending.address = candidates[index];
      memcpy(pending.name, ui.devices[index], sizeof(pending.name));
    }
    queued = true;
  }
  portEXIT_CRITICAL(&uiLock);
  if (queued) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &commandEvent);
  return queued;
}
}

void BluetoothControls::begin(Preferences &preferences, bool disableAtBoot) {
  settings = &preferences;
  if (settings->getBytesLength("blePeer") == sizeof(lastController)) {
    settings->getBytes("blePeer", &lastController, sizeof(lastController));
    lastController.name[sizeof(lastController.name) - 1] = 0;
    if (lastController.address.type > BLE_ADDR_RANDOM) lastController = {};
  }
  const bool wasEnabled = settings->getBool(kEnabledKey, false);
  if (disableAtBoot) {
    if (wasEnabled) settings->putBool(kEnabledKey, false);
    Serial.println("Bluetooth: skipped at boot (Start+Select)");
    return;
  }
  radioWanted.store(wasEnabled);
  resume();
#if CONFIG_SPIRAM_USE_MALLOC && CONFIG_IDF_TARGET_ESP32S3
  if (radioRunning && psramFound()) {
    // Malloc and realloc normally put short ROM paths and other small objects
    // in SRAM. Keep that heap available for FreeRTOS and DMA on Bluetooth boots.
    // Set this once, before the launcher mounts the card or starts other tasks.
    heap_caps_malloc_extmem_enable(0);
    Serial.printf("Bluetooth: PSRAM preferred, %u bytes internal heap after radio start\n",
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  }
#endif
}

void BluetoothControls::enableOnRestart() {
  if (!settings || !settings->putBool(kEnabledKey, true)) {
    startError("Could not save Bluetooth setting");
    return;
  }
  setStatus(State::RestartRequired, "Restart to start Bluetooth");
}

void BluetoothControls::disable() {
  if (settings && settings->getBool(kEnabledKey, false) && !settings->putBool(kEnabledKey, false)) {
    const auto state = radioWanted.load() ? State::Error : State::RestartRequired;
    setStatus(state, "Could not save Bluetooth setting");
    return;
  }
  radioWanted.store(false);
  stopRadio();
}

void BluetoothControls::suspend() {
  if (radioRunning) stopRadio();
}
void BluetoothControls::resume() { if (radioWanted.load()) startRadio(); }

void BluetoothControls::service() {
  serviceStartup();
  if (!settings || !saveController.exchange(false, std::memory_order_acq_rel)) return;
  portENTER_CRITICAL(&uiLock);
  const auto peer = discoveredBond;
  const bool changed = ble_addr_cmp(&peer.address, &lastController.address) != 0 ||
                       strcmp(peer.name, lastController.name) != 0;
  lastController = peer;
  portEXIT_CRITICAL(&uiLock);
  if (changed) settings->putBytes("blePeer", &peer, sizeof(peer));
}
bool BluetoothControls::scan() { return send(Command::Scan); }
bool BluetoothControls::pair(int index) { return send(Command::Pair, index); }
bool BluetoothControls::disconnect() { return send(Command::Disconnect); }

BluetoothControls::Snapshot BluetoothControls::status() {
  portENTER_CRITICAL(&uiLock);
  auto copy = ui;
  portEXIT_CRITICAL(&uiLock);
  copy.enabled = radioWanted.load();
  return copy;
}

ControllerInput BluetoothControls::takeInput() { return buttons.take(); }

#else
void BluetoothControls::begin(Preferences &, bool) {}
void BluetoothControls::enableOnRestart() {}
void BluetoothControls::disable() {}
void BluetoothControls::suspend() {}
void BluetoothControls::resume() {}
void BluetoothControls::service() {}
bool BluetoothControls::scan() { return false; }
bool BluetoothControls::pair(int) { return false; }
bool BluetoothControls::disconnect() { return false; }
BluetoothControls::Snapshot BluetoothControls::status() {
  Snapshot unavailable;
  unavailable.status = "BLE unavailable in this build";
  return unavailable;
}
ControllerInput BluetoothControls::takeInput() { return {}; }
#endif
