#include "zigbee_send.h"
#include <Arduino.h>
#include <Zigbee.h>
#include <ZigbeeAfLock.h>

// How long to wait for the stack to report the APS result (it retries internally)
static const uint32_t SEND_TIMEOUT_MS = 5000;
static const uint32_t REJOIN_TIMEOUT_MS = 10000;
// The library puts the OTA client cluster on its metadata endpoint
static const uint8_t OTA_CLIENT_ENDPOINT = 240;
static const uint16_t OTA_MANUFACTURER_ID = 0x1049;  // Silicon Labs
static const uint16_t OTA_IMAGE_TYPE = 0x0000;
static const uint16_t OTA_HARDWARE_VERSION = 1;
// A parent heard weaker than this is replaced by whichever router is nearest now
static const int8_t WEAK_PARENT_RSSI = -85;
static const uint32_t PARENT_SAMPLE_MAX_AGE_MS = 3000;
static const uint32_t WEAK_PARENT_REJOIN_INTERVAL_MS = 60000;
// Network steering joins at +3 dBm and the stack keeps that; +10 dBm is 5x the power
static const int8_t RADIO_TX_POWER_DBM = 10;

static StaticSemaphore_t sent_sem_buf;
static SemaphoreHandle_t sent_sem;
static volatile sl_status_t sent_status;
static uint8_t last_channel;
static volatile int8_t parent_rssi;
static volatile uint8_t parent_lqi;
static volatile uint32_t parent_sample_ms;
static bool parent_sampled;
static uint32_t last_weak_rejoin_ms;
static bool weak_rejoined;

// The stack calls this for every incoming APS message. An end device hears unicasts only through its
// parent, so their last hop RSSI is the quality of the parent link.
extern "C" void sl_zigbee_af_incoming_message_cb(sl_zigbee_incoming_message_type_t type,
                                                 sl_zigbee_aps_frame_t* aps_frame,
                                                 sl_zigbee_rx_packet_info_t* packet_info,
                                                 uint8_t message_length,
                                                 uint8_t* message)
{
  (void)aps_frame;
  (void)message_length;
  (void)message;
  if (type != SL_ZIGBEE_INCOMING_UNICAST || packet_info == nullptr) {
    return;
  }
  parent_rssi = packet_info->last_hop_rssi;
  parent_lqi = packet_info->last_hop_lqi;
  parent_sample_ms = millis();
  parent_sampled = true;
}

// Runs in the stack task once the frame is acked or given up on
static void on_sent(sl_zigbee_outgoing_message_type_t type,
                    uint16_t destination,
                    sl_zigbee_aps_frame_t* aps_frame,
                    uint16_t length,
                    uint8_t* message,
                    sl_status_t status)
{
  (void)type;
  (void)destination;
  (void)aps_frame;
  (void)length;
  (void)message;
  sent_status = status;
  xSemaphoreGive(sent_sem);
}

// ZigbeeSwitch::move_to_level() sends to the coordinator AND to bindings; ZHA binds the
// Level Control cluster to the coordinator, so every press arrived twice. Send it once, directly.
static bool transmit(uint8_t endpoint, uint8_t level)
{
  if (sent_sem == nullptr) {
    sent_sem = xSemaphoreCreateBinaryStatic(&sent_sem_buf);
  }
  sl_status_t status;
  {
    ZigbeeAfLock lock;
    xSemaphoreTake(sent_sem, 0);
    sl_zigbee_af_set_command_endpoints(endpoint, 1);
    sl_zigbee_af_fill_command_level_control_cluster_move_to_level(level, 0, 0x00, 0x00);
    status = sl_zigbee_af_send_command_unicast_with_cb(SL_ZIGBEE_OUTGOING_DIRECT, 0x0000, on_sent);
  }
  if (status != SL_STATUS_OK) {
    Serial.printf("Send failed to start: 0x%lx\n", (unsigned long)status);
    return false;
  }
  if (xSemaphoreTake(sent_sem, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdTRUE) {
    Serial.println("Send timed out");
    return false;
  }
  if (sent_status != SL_STATUS_OK) {
    Serial.printf("Send failed: 0x%lx\n", (unsigned long)sent_status);
    return false;
  }
  return true;
}

static void apply_tx_power()
{
  int8_t before;
  sl_status_t status;
  {
    ZigbeeAfLock lock;
    before = sl_zigbee_get_radio_power();
    if (before == RADIO_TX_POWER_DBM) {
      return;
    }
    status = sl_zigbee_set_radio_power(RADIO_TX_POWER_DBM);
  }
  Serial.printf("TX power %d -> %d dBm (0x%lx)\n", before, RADIO_TX_POWER_DBM, (unsigned long)status);
}

static bool wait_for_network(uint32_t timeout_ms)
{
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (Zigbee.isConnectedToNetwork()) {
      return true;
    }
    digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);
    delay(40);
    digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);
    delay(160);
  }
  return false;
}

// Secure rejoin through whichever router is closest now: first on the known channel, then on all
static bool rejoin()
{
  const uint32_t masks[] = { last_channel ? (1UL << last_channel) : 0, SL_ZIGBEE_ALL_802_15_4_CHANNELS_MASK };
  for (uint32_t mask : masks) {
    if (mask == 0) {
      continue;
    }
    Serial.printf("Rejoining, channel mask 0x%08lx\n", (unsigned long)mask);
    sl_status_t status;
    {
      ZigbeeAfLock lock;
      status = sl_zigbee_find_and_rejoin_network(true, mask, SL_ZIGBEE_REJOIN_DUE_TO_APP_EVENT_1,
                                                 SL_ZIGBEE_DEVICE_TYPE_UNCHANGED);
    }
    if (status == SL_STATUS_OK && wait_for_network(REJOIN_TIMEOUT_MS)) {
      Serial.println("Rejoined");
      apply_tx_power();
      return true;
    }
  }
  Serial.println("Rejoin failed");
  return false;
}

bool zigbee_ensure_connected()
{
  uint8_t channel = Zigbee.getChannel();  // the stored network's channel, also while the parent is gone
  if (channel >= 11 && channel <= 26) {
    last_channel = channel;
  }
  if (Zigbee.isConnectedToNetwork() || rejoin()) {
    apply_tx_power();
    return true;
  }
  return false;
}

bool send_key(uint8_t endpoint, uint8_t level)
{
  // The old parent may be out of range after the remote was carried to another room
  if (zigbee_ensure_connected() && transmit(endpoint, level)) {
    return true;
  }
  return rejoin() && transmit(endpoint, level);
}

void zigbee_check_parent()
{
  if (!parent_sampled || millis() - parent_sample_ms > PARENT_SAMPLE_MAX_AGE_MS) {
    return;
  }
  sl_802154_short_addr_t parent;
  {
    ZigbeeAfLock lock;
    parent = sl_zigbee_get_parent_node_id();
  }
  Serial.printf("Parent 0x%04X, RSSI %d dBm, LQI %u\n", parent, parent_rssi, parent_lqi);
  if (parent_rssi >= WEAK_PARENT_RSSI) {
    return;
  }
  if (weak_rejoined && millis() - last_weak_rejoin_ms < WEAK_PARENT_REJOIN_INTERVAL_MS) {
    return;
  }
  weak_rejoined = true;
  last_weak_rejoin_ms = millis();
  parent_sampled = false;
  Serial.println("Parent link weak, looking for a nearer router");
  rejoin();
}

void announce_firmware_version(uint32_t version)
{
  ZigbeeAfLock lock;
  sl_zigbee_af_set_command_endpoints(OTA_CLIENT_ENDPOINT, 1);
  // Field control bit 0: hardware version present
  sl_zigbee_af_fill_command_ota_bootload_cluster_query_next_image_request(0x01, OTA_MANUFACTURER_ID, OTA_IMAGE_TYPE,
                                                                        version, OTA_HARDWARE_VERSION);
  sl_status_t status = sl_zigbee_af_send_command_unicast(SL_ZIGBEE_OUTGOING_DIRECT, 0x0000);
  if (status != SL_STATUS_OK) {
    Serial.printf("Firmware version announce failed: 0x%lx\n", (unsigned long)status);
  }
}
