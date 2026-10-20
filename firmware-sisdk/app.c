/*
   Zigbee 4x4 keypad remote for Home Assistant (ZHA), sleepy end device.

   Every key press goes to the coordinator as a Level Control "move_to_level" command,
   level = key number (1..16):
     - endpoint 10 -> short press
     - endpoint 11 -> press held for >= 600 ms
   ZHA turns these into `zha_event`s, so all the logic lives in HA automations.

   Factory reset: hold key 1 for 5 s while powering on / resetting.
*/
#include "app/framework/include/af.h"
#include "network-steering.h"
#include "em_emu.h"
#include "sl_sleeptimer.h"
#include "board.h"
#include "keypad.h"
#include "battery.h"

// Must match the OTA "Current File Version" default in config/zcl/zcl_config.zap
#define FIRMWARE_VERSION            0x00000300
#define OTA_MANUFACTURER_ID         0x1049  // Silicon Labs
#define OTA_IMAGE_TYPE              0x0000
#define OTA_HARDWARE_VERSION        1

#define EP_SHORT                    10
#define EP_LONG                     11
#define COORDINATOR                 0x0000
#define COORDINATOR_ENDPOINT        1

#define RADIO_TX_POWER_DBM          10
#define BATTERY_REPORT_INTERVAL_MS  (30UL * 60UL * 1000UL)
#define BATTERY_CRITICAL_MV         3400
#define LOW_BATTERY_PERCENT         20
#define LED_PRESS_MS                80
#define LED_LOW_BATTERY_MS          1000
#define JOIN_BLINK_MS               1000
#define JOIN_RETRY_MS               15000
#define JOIN_ATTEMPTS               8
#define FACTORY_RESET_HOLD_MS       5000
#define REPORT_AFTER_JOIN_MS        2000
#define SHUTDOWN_AFTER_REPORT_MS    2000
// ZHA interviews a freshly joined device; a sleepy one has to keep polling meanwhile
#define STAY_AWAKE_AFTER_JOIN_MS    60000

static sl_zigbee_af_event_t led_event;
static sl_zigbee_af_event_t join_event;
static sl_zigbee_af_event_t join_blink_event;
static sl_zigbee_af_event_t battery_event;
static sl_zigbee_af_event_t shutdown_event;
static sl_zigbee_af_event_t awake_event;

static bool factory_reset_requested;
static uint8_t join_attempts_left;
static uint8_t battery_percent_now = 100;
static uint16_t battery_mv_now;
static uint32_t last_battery_report_ms;
static bool battery_reported;

static bool joined(void)
{
  return sl_zigbee_af_network_state() == SL_ZIGBEE_JOINED_NETWORK;
}

static void blink(uint8_t times)
{
  for (uint8_t i = 0; i < times; i++) {
    board_led(true);
    sl_sleeptimer_delay_millisecond(40);
    board_led(false);
    sl_sleeptimer_delay_millisecond(80);
  }
}

// ---------------------------------------------------------------------------
// Battery

static void measure_battery(void)
{
  battery_mv_now = battery_read_mv();
  battery_percent_now = battery_percent(battery_mv_now);
  sl_zigbee_app_debug_println("Battery: %d mV, %d%%", battery_mv_now, battery_percent_now);
}

// A single low reading must not switch the remote off: re-check a few times
static bool battery_critical(void)
{
  for (int i = 0; i < 3; i++) {
    measure_battery();
    if (battery_mv_now >= BATTERY_CRITICAL_MV) {
      return false;
    }
    halResetWatchdog();
    sl_sleeptimer_delay_millisecond(500);
  }
  return true;
}

static void report_battery(void)
{
  uint8_t voltage = (uint8_t)(battery_mv_now / 100);          // 100 mV units
  uint8_t percent = (uint8_t)(battery_percent_now * 2);       // 0.5 % units
  sl_zigbee_af_write_server_attribute(EP_SHORT, ZCL_POWER_CONFIG_CLUSTER_ID, ZCL_BATTERY_VOLTAGE_ATTRIBUTE_ID,
                                      &voltage, ZCL_INT8U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_server_attribute(EP_SHORT, ZCL_POWER_CONFIG_CLUSTER_ID,
                                      ZCL_BATTERY_PERCENTAGE_REMAINING_ATTRIBUTE_ID,
                                      &percent, ZCL_INT8U_ATTRIBUTE_TYPE);
  sl_zigbee_af_fill_external_buffer(ZCL_GLOBAL_COMMAND | ZCL_FRAME_CONTROL_SERVER_TO_CLIENT
                                    | ZCL_DISABLE_DEFAULT_RESPONSE_MASK,
                                    ZCL_POWER_CONFIG_CLUSTER_ID, ZCL_REPORT_ATTRIBUTES_COMMAND_ID, "vuuvuu",
                                    ZCL_BATTERY_VOLTAGE_ATTRIBUTE_ID, ZCL_INT8U_ATTRIBUTE_TYPE, voltage,
                                    ZCL_BATTERY_PERCENTAGE_REMAINING_ATTRIBUTE_ID, ZCL_INT8U_ATTRIBUTE_TYPE, percent);
  sl_zigbee_af_set_command_endpoints(EP_SHORT, COORDINATOR_ENDPOINT);
  sl_status_t status = sl_zigbee_af_send_command_unicast(SL_ZIGBEE_OUTGOING_DIRECT, COORDINATOR);
  sl_zigbee_app_debug_println("Battery report: 0x%02X", status);
  last_battery_report_ms = halCommonGetInt32uMillisecondTick();
  battery_reported = true;
}

// Deep discharge protection: warn with 3 long blinks, then EM4 until a row-1 key is pressed.
// Waking from EM4 resets the chip, so the boot path re-checks the voltage.
static void shutdown_now(void)
{
  sl_zigbee_app_debug_println("Battery critical, going to EM4");
  for (int i = 0; i < 3; i++) {
    board_led(true);
    sl_sleeptimer_delay_millisecond(500);
    board_led(false);
    sl_sleeptimer_delay_millisecond(300);
    halResetWatchdog();
  }
  board_rf_off();
  keypad_prepare_em4();
  EMU_EM4Init_TypeDef em4 = EMU_EM4INIT_DEFAULT;
  em4.pinRetentionMode = emuPinRetentionLatch;
  EMU_EM4Init(&em4);
  EMU_EnterEM4();
}

static void shutdown_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  shutdown_now();
}

static void battery_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  if (joined()) {
    measure_battery();
    report_battery();
    if (battery_mv_now < BATTERY_CRITICAL_MV && battery_critical()) {
      battery_percent_now = 0;
      report_battery();
      // Let the 0 % report go out before the radio stops
      sl_zigbee_af_event_set_delay_ms(&shutdown_event, SHUTDOWN_AFTER_REPORT_MS);
      return;
    }
  }
  sl_zigbee_af_event_set_delay_ms(&battery_event, BATTERY_REPORT_INTERVAL_MS);
}

// ---------------------------------------------------------------------------
// Network

static void awake_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  sl_zigbee_af_remove_from_current_app_tasks(SL_ZIGBEE_AF_FORCE_SHORT_POLL);
  sl_zigbee_app_debug_println("Back to long polling");
}

// ZHA takes the firmware version only from an OTA Query Next Image request
static void announce_firmware_version(void)
{
  sl_zigbee_af_fill_command_ota_bootload_cluster_query_next_image_request(0x01, OTA_MANUFACTURER_ID, OTA_IMAGE_TYPE,
                                                                        FIRMWARE_VERSION, OTA_HARDWARE_VERSION);
  sl_zigbee_af_set_command_endpoints(EP_SHORT, COORDINATOR_ENDPOINT);
  sl_status_t status = sl_zigbee_af_send_command_unicast(SL_ZIGBEE_OUTGOING_DIRECT, COORDINATOR);
  sl_zigbee_app_debug_println("Firmware version announce: 0x%02X", status);
}

static void join_blink_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  if (joined()) {
    return;
  }
  blink(1);
  sl_zigbee_af_event_set_delay_ms(&join_blink_event, JOIN_BLINK_MS);
}

static void start_joining(void)
{
  if (join_attempts_left == 0) {
    join_attempts_left = JOIN_ATTEMPTS;
  }
  sl_zigbee_af_event_set_active(&join_event);
  sl_zigbee_af_event_set_active(&join_blink_event);
}

static void join_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  if (factory_reset_requested) {
    factory_reset_requested = false;
    if (sl_zigbee_af_network_state() != SL_ZIGBEE_NO_NETWORK) {
      sl_zigbee_app_debug_println("Factory reset: leaving the network");
      sl_zigbee_leave_network(SL_ZIGBEE_LEAVE_NWK_WITH_NO_OPTION);
      return;  // joining restarts on NETWORK_DOWN
    }
  }
  if (sl_zigbee_af_network_state() != SL_ZIGBEE_NO_NETWORK) {
    return;
  }
  sl_status_t status = sl_zigbee_af_network_steering_start();
  sl_zigbee_app_debug_println("Join start: 0x%02X (attempts left %d)", status, join_attempts_left);
}

void sl_zigbee_af_network_steering_complete_cb(sl_status_t status,
                                               uint8_t totalBeacons,
                                               uint8_t joinAttempts,
                                               uint8_t finalState)
{
  (void)joinAttempts;
  (void)finalState;
  sl_zigbee_app_debug_println("Join complete: 0x%02X, beacons %d", status, totalBeacons);
  if (status == SL_STATUS_OK) {
    return;
  }
  // Give up after a few rounds so a missing coordinator doesn't drain the battery; a key press retries
  if (join_attempts_left > 0 && --join_attempts_left > 0) {
    sl_zigbee_af_event_set_delay_ms(&join_event, JOIN_RETRY_MS);
  } else {
    sl_zigbee_af_event_set_inactive(&join_blink_event);
    board_led(false);
  }
}

void sl_zigbee_af_stack_status_cb(sl_status_t status)
{
  if (status == SL_STATUS_NETWORK_UP) {
    // Network steering joins at low power; the stack keeps whatever the join used
    sl_zigbee_set_radio_power(RADIO_TX_POWER_DBM);
    sl_zigbee_af_event_set_inactive(&join_blink_event);
    join_attempts_left = 0;
    sl_zigbee_app_debug_println("Network up: channel %d, PAN 0x%04X, node 0x%04X",
                                sl_zigbee_get_radio_channel(), sl_zigbee_get_pan_id(), sl_zigbee_get_node_id());
    sl_zigbee_af_add_to_current_app_tasks(SL_ZIGBEE_AF_FORCE_SHORT_POLL);
    sl_zigbee_af_event_set_delay_ms(&awake_event, STAY_AWAKE_AFTER_JOIN_MS);
    blink(3);
    announce_firmware_version();
    sl_zigbee_af_event_set_delay_ms(&battery_event, REPORT_AFTER_JOIN_MS);
  } else if (status == SL_STATUS_NETWORK_DOWN && sl_zigbee_af_network_state() == SL_ZIGBEE_NO_NETWORK) {
    sl_zigbee_app_debug_println("Network down, joining again");
    start_joining();
  }
}

// ---------------------------------------------------------------------------
// Keys

static void led_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  board_led(false);
}

static void key_sent(sl_zigbee_outgoing_message_type_t type,
                     uint16_t destination,
                     sl_zigbee_aps_frame_t *aps_frame,
                     uint16_t length,
                     uint8_t *message,
                     sl_status_t status)
{
  (void)type;
  (void)destination;
  (void)length;
  (void)message;
  sl_zigbee_app_debug_println("Key from ep %d delivered: 0x%02X", aps_frame->sourceEndpoint, status);
  if (status != SL_STATUS_OK) {
    // Parent unreachable (e.g. the remote was carried to another room): find a nearer one
    sl_zigbee_af_start_move_cb();
  }
}

static void on_key(uint8_t key, bool long_press)
{
  sl_zigbee_app_debug_println("Key %d %s", key, long_press ? "long" : "short");
  if (!joined()) {
    start_joining();
    return;
  }

  // Measure before transmitting so the radio load doesn't skew the reading
  measure_battery();
  board_led(true);
  sl_zigbee_af_event_set_delay_ms(&led_event,
                                  battery_percent_now < LOW_BATTERY_PERCENT ? LED_LOW_BATTERY_MS : LED_PRESS_MS);

  sl_zigbee_af_fill_command_level_control_cluster_move_to_level(key, 0, 0, 0);
  sl_zigbee_af_set_command_endpoints(long_press ? EP_LONG : EP_SHORT, COORDINATOR_ENDPOINT);
  sl_status_t status = sl_zigbee_af_send_command_unicast_with_cb(SL_ZIGBEE_OUTGOING_DIRECT, COORDINATOR, key_sent);
  if (status != SL_STATUS_OK) {
    sl_zigbee_app_debug_println("Key send failed to start: 0x%02X", status);
  }

  if (battery_mv_now < BATTERY_CRITICAL_MV
      || !battery_reported
      || halCommonGetInt32uMillisecondTick() - last_battery_report_ms >= BATTERY_REPORT_INTERVAL_MS) {
    sl_zigbee_af_event_set_active(&battery_event);
  }
}

// ---------------------------------------------------------------------------
// Startup

// Key 1 must be held for FACTORY_RESET_HOLD_MS (LED on) so a stray touch can't wipe the pairing
static bool factory_reset_held(void)
{
  if (keypad_scan() != 1) {
    return false;
  }
  board_led(true);
  uint32_t start = halCommonGetInt32uMillisecondTick();
  while (keypad_scan() == 1) {
    halResetWatchdog();
    if (halCommonGetInt32uMillisecondTick() - start >= FACTORY_RESET_HOLD_MS) {
      board_led(false);
      blink(5);
      return true;
    }
    sl_sleeptimer_delay_millisecond(50);
  }
  board_led(false);
  return false;
}

void sl_zigbee_af_main_init_cb(void)
{
  board_init();
  battery_init();
  sl_zigbee_app_debug_println("Zigbee keypad remote 0x%08X", FIRMWARE_VERSION);

  sl_zigbee_af_event_init(&led_event, led_event_handler);
  sl_zigbee_af_event_init(&join_event, join_event_handler);
  sl_zigbee_af_event_init(&join_blink_event, join_blink_event_handler);
  sl_zigbee_af_event_init(&battery_event, battery_event_handler);
  sl_zigbee_af_event_init(&shutdown_event, shutdown_event_handler);
  sl_zigbee_af_event_init(&awake_event, awake_event_handler);

  if (battery_critical()) {
    shutdown_now();
  }

  keypad_init(on_key);

  // A row-1 key wakes the remote from low-battery EM4 and may still be held here: not a reset request
  bool woke_from_em4 = halGetExtendedResetInfo() == RESET_EXTERNAL_EM4PIN;
  factory_reset_requested = !woke_from_em4 && factory_reset_held();

  if (factory_reset_requested || sl_zigbee_af_network_state() == SL_ZIGBEE_NO_NETWORK) {
    start_joining();
  }
}

#ifndef SL_CATALOG_ZIGBEE_EZSP_PRESENT
void sl_zigbee_af_radio_needs_calibrating_cb(void)
{
  sl_mac_calibrate_current_channel();
}
#endif
