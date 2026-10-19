/*
   Zigbee 4x4 keypad remote for Home Assistant (ZHA)

   Board: Seeed Studio XIAO MG24 (Sense), Silicon Labs Arduino core 5.x,
          Tools -> Protocol stack -> Zigbee

   Every key press is sent to the coordinator as a Level Control "move_to_level"
   command, where level = key number (1..16):
     - endpoint "short"  -> short press
     - endpoint "long"   -> press held for >= LONG_PRESS_MS
   ZHA turns these into `zha_event`s, so all the logic lives in HA automations.

   Wiring (keypad pins 1..8 -> XIAO D0..D7):
     rows    R1..R4 -> D0..D3  (inputs with pull-up, wake-up interrupts)
     columns C1..C4 -> D4..D7  (driven LOW while idle)
   Li-Po goes straight to the XIAO BAT pads; the board charges it from USB-C.

   Factory reset: hold key 1 (R1/C1) for 5 s while powering on / resetting.
*/
#include <Zigbee.h>
#include <ZigbeeSwitch.h>
#include <ZigbeePowerSource.h>
#include <ArduinoLowPower.h>
#include "zigbee_send.h"

static const uint8_t ROW_PINS[4] = { D0, D1, D2, D3 };
static const uint8_t COL_PINS[4] = { D4, D5, D6, D7 };

static const uint32_t DEBOUNCE_MS = 20;
static const uint32_t LONG_PRESS_MS = 600;
static const uint32_t BATTERY_REPORT_INTERVAL_MS = 30UL * 60UL * 1000UL;

// LED feedback: short flash per key press, long flash when the battery is low
static const uint32_t LED_PRESS_MS = 80;
static const uint32_t LED_LOW_BATTERY_MS = 1000;
static const uint8_t LOW_BATTERY_PERCENT = 20;

static const uint32_t FACTORY_RESET_HOLD_MS = 5000;
static const uint32_t REJOIN_RETRY_MS = 15000;
static const uint32_t FIRMWARE_VERSION = 0x00000204;

// XIAO MG24 RF switch between the chip and the antennas. The core leaves it unpowered, which starves
// even the onboard antenna. PB4: LOW = onboard ceramic antenna, HIGH = external U.FL antenna.
static const uint8_t RF_SWITCH_POWER_PIN = PB5;
static const uint8_t RF_SWITCH_SELECT_PIN = PB4;
static const bool USE_EXTERNAL_ANTENNA = false;

// Battery measurement on XIAO MG24: PD3 enables the divider, PD4 reads VBAT/2
// Typical Li-Po open-circuit discharge curve: {volts, percent}, descending
static const float BATTERY_CURVE[][2] = {
  { 4.20f, 100 }, { 4.10f, 90 }, { 4.02f, 80 }, { 3.95f, 70 }, { 3.87f, 60 }, { 3.84f, 50 },
  { 3.80f, 40 }, { 3.77f, 30 }, { 3.73f, 20 }, { 3.69f, 10 }, { 3.61f, 5 }, { 3.30f, 0 },
};
// Below this the remote stops the radio and sleeps in EM4 until a key in row 1 is pressed
static const float BATTERY_CRITICAL_V = 3.40f;

ZigbeeSwitch remote_short;
ZigbeeSwitch remote_long;
ZigbeePowerSource battery;

// The library hands out switch endpoints in begin() order
static const uint8_t EP_SHORT = 10;
static const uint8_t EP_LONG = 11;

static StaticSemaphore_t key_sem_buf;
static SemaphoreHandle_t key_sem;
static uint32_t last_battery_report_ms;
static bool battery_reported;
static uint8_t battery_percent = 100;

static void key_isr()
{
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(key_sem, &woken);
  portYIELD_FROM_ISR(woken);
}

// Idle state: all columns LOW, so any key pulls its row LOW and fires an interrupt
static void matrix_idle()
{
  for (uint8_t c = 0; c < 4; c++) {
    pinMode(COL_PINS[c], OUTPUT);
    digitalWrite(COL_PINS[c], LOW);
  }
}

// Returns key number 1..16, or 0 if nothing is pressed
static uint8_t matrix_scan()
{
  uint8_t key = 0;
  for (uint8_t c = 0; c < 4 && key == 0; c++) {
    for (uint8_t i = 0; i < 4; i++) {
      // Only the scanned column is driven; the rest float so they don't short through other keys
      pinMode(COL_PINS[i], i == c ? OUTPUT : INPUT);
    }
    digitalWrite(COL_PINS[c], LOW);
    delayMicroseconds(10);
    for (uint8_t r = 0; r < 4; r++) {
      if (digitalRead(ROW_PINS[r]) == LOW) {
        key = r * 4 + c + 1;
        break;
      }
    }
  }
  matrix_idle();
  return key;
}

static bool any_key_down()
{
  for (uint8_t r = 0; r < 4; r++) {
    if (digitalRead(ROW_PINS[r]) == LOW) {
      return true;
    }
  }
  return false;
}

// Load (radio TX) and ADC glitches only ever pull the reading down, so the highest
// of a few averaged bursts is the best estimate of the real battery voltage
static float read_battery_voltage()
{
  pinMode(PD3, OUTPUT);
  digitalWrite(PD3, HIGH);
  delay(10);
  float best = 0;
  for (uint8_t burst = 0; burst < 3; burst++) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 8; i++) {
      sum += analogRead(PD4);
    }
    float v = (sum / 8.0f) * (2.0f * 3.3f / 4095.0f);
    if (v > best) {
      best = v;
    }
    delay(5);
  }
  digitalWrite(PD3, LOW);
  return best;
}

static uint8_t voltage_to_percent(float v)
{
  const size_t n = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
  if (v >= BATTERY_CURVE[0][0]) {
    return 100;
  }
  for (size_t i = 1; i < n; i++) {
    if (v >= BATTERY_CURVE[i][0]) {
      float v_hi = BATTERY_CURVE[i - 1][0], p_hi = BATTERY_CURVE[i - 1][1];
      float v_lo = BATTERY_CURVE[i][0], p_lo = BATTERY_CURVE[i][1];
      return (uint8_t)(p_lo + (v - v_lo) * (p_hi - p_lo) / (v_hi - v_lo) + 0.5f);
    }
  }
  return 0;
}

// Returns the measured voltage and updates battery_percent
static float measure_battery()
{
  float v = read_battery_voltage();
  battery_percent = voltage_to_percent(v);
  Serial.printf("Battery: %.2f V, %u%%\n", v, battery_percent);
  return v;
}

// Deep discharge protection: warn with 3 long blinks, then EM4 until a row-1 key (D0) is pressed.
// Waking from EM4 resets the chip, so setup() re-checks the voltage.
static void shutdown_low_battery()
{
  Serial.println("Battery critical, going to deep sleep");
  digitalWrite(RF_SWITCH_POWER_PIN, LOW);
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);
    delay(500);
    digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);
    delay(300);
  }
  matrix_idle();
  LowPower.attachInterruptWakeup(ROW_PINS[0], nullptr, FALLING);
  LowPower.deepSleep();
}

// A single low reading must not switch the remote off: re-check a few times
static bool battery_critical()
{
  for (uint8_t i = 0; i < 3; i++) {
    if (measure_battery() >= BATTERY_CRITICAL_V) {
      return false;
    }
    delay(500);
  }
  return true;
}

static void report_battery()
{
  // Periodic reports also notice a lost parent and move to a nearer router
  zigbee_ensure_connected();
  battery.set_battery_percent(battery_percent);
  battery.send_attribute_report();
  last_battery_report_ms = millis();
  battery_reported = true;
}

static void blink(uint8_t times)
{
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);
    delay(40);
    digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);
    if (i + 1 < times) {
      delay(80);
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Zigbee keypad remote");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);

  pinMode(RF_SWITCH_POWER_PIN, OUTPUT);
  digitalWrite(RF_SWITCH_POWER_PIN, HIGH);
  pinMode(RF_SWITCH_SELECT_PIN, OUTPUT);
  digitalWrite(RF_SWITCH_SELECT_PIN, USE_EXTERNAL_ANTENNA ? HIGH : LOW);

  for (uint8_t r = 0; r < 4; r++) {
    pinMode(ROW_PINS[r], INPUT_PULLUP);
  }
  matrix_idle();

  if (battery_critical()) {
    shutdown_low_battery();
  }

  // A row-1 key wakes the remote from low-battery EM4 and may still be held here: not a reset request.
  // Key 1 must be held for FACTORY_RESET_HOLD_MS (LED on) so a stray touch can't wipe the pairing.
  if (!LowPower.wokeUpFromDeepSleep() && matrix_scan() == 1) {
    digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);
    uint32_t start = millis();
    while (matrix_scan() == 1 && millis() - start < FACTORY_RESET_HOLD_MS) {
      delay(50);
    }
    digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);
    if (millis() - start >= FACTORY_RESET_HOLD_MS) {
      Serial.println("Factory resetting... release key 1");
      blink(5);
      while (any_key_down()) {
        delay(100);
      }
      Zigbee.factoryReset();
    }
  }

  key_sem = xSemaphoreCreateBinaryStatic(&key_sem_buf);
  for (uint8_t r = 0; r < 4; r++) {
    attachInterrupt(digitalPinToInterrupt(ROW_PINS[r]), key_isr, FALLING);
  }

  Zigbee.setVendorName("punka");
  Zigbee.setProductName("Keypad Remote");
  Zigbee.setFirmwareVersion(FIRMWARE_VERSION);
  Zigbee.setDeviceType(ZIGBEE_DEVICE_TYPE_END_DEVICE);
  Zigbee.setPowerSource(ZIGBEE_POWER_SOURCE_TYPE_BATTERY);
  Zigbee.begin();
  remote_short.begin();
  remote_long.begin();
  battery.begin();

  if (!Zigbee.isPaired()) {
    Serial.println("Waiting to join a Zigbee network (enable pairing in ZHA)...");
  }
  while (!Zigbee.isPaired()) {
    blink(1);
    delay(1000);
  }
  // Paired but no parent (e.g. powered up out of range of the old one): look for the nearest router
  uint32_t last_rejoin = millis();
  while (!Zigbee.isConnectedToNetwork()) {
    blink(1);
    delay(200);
    if (millis() - last_rejoin >= REJOIN_RETRY_MS) {
      zigbee_ensure_connected();
      last_rejoin = millis();
    }
  }
  Serial.printf("Connected; channel %u, PAN 0x%04X\n", Zigbee.getChannel(), Zigbee.getPanId());
  zigbee_ensure_connected();  // remembers the channel for fast rejoins
  announce_firmware_version(FIRMWARE_VERSION);
  blink(3);
  measure_battery();
  report_battery();
}

void loop()
{
  // Block until a key interrupt; the stack lets the chip sleep in the meantime.
  // The timeout keeps battery reports going even if nobody touches the remote.
  if (xSemaphoreTake(key_sem, pdMS_TO_TICKS(BATTERY_REPORT_INTERVAL_MS)) != pdTRUE) {
    measure_battery();
    report_battery();
    if (battery_critical()) {
      delay(2000);  // let the 0 % report go out before the radio stops
      shutdown_low_battery();
    }
    return;
  }

  delay(DEBOUNCE_MS);
  uint8_t key = matrix_scan();
  if (key == 0) {
    xSemaphoreTake(key_sem, 0);
    return;
  }

  uint32_t pressed_at = millis();
  bool long_press = false;
  while (matrix_scan() == key) {
    if (millis() - pressed_at >= LONG_PRESS_MS) {
      long_press = true;
      break;
    }
    delay(20);
  }

  Serial.printf("Key %u %s\n", key, long_press ? "long" : "short");
  // Measure before transmitting so the radio load doesn't skew the reading
  float v = measure_battery();
  digitalWrite(LED_BUILTIN, LED_BUILTIN_ACTIVE);
  if (!send_key(long_press ? EP_LONG : EP_SHORT, key)) {
    Serial.println("Key press not delivered");
  }
  delay(battery_percent < LOW_BATTERY_PERCENT ? LED_LOW_BATTERY_MS : LED_PRESS_MS);
  digitalWrite(LED_BUILTIN, LED_BUILTIN_INACTIVE);

  // Wait for release so a held key doesn't repeat
  while (any_key_down()) {
    delay(20);
  }
  delay(DEBOUNCE_MS);
  zigbee_check_parent();
  // Drop interrupts caused by bouncing and by the scan itself
  xSemaphoreTake(key_sem, 0);

  if (v < BATTERY_CRITICAL_V && battery_critical()) {
    report_battery();
    delay(2000);
    shutdown_low_battery();
  }
  if (!battery_reported || millis() - last_battery_report_ms >= BATTERY_REPORT_INTERVAL_MS) {
    report_battery();
  }
}
