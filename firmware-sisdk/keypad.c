#include "keypad.h"
#include "app/framework/include/af.h"
#include "sl_gpio.h"
#include "sl_udelay.h"
#include "sl_sleeptimer.h"
#include "sl_hal_gpio.h"
#include "em_gpio.h"

#define DEBOUNCE_MS     20
#define HOLD_POLL_MS    20
#define LONG_PRESS_MS   600
// Port C pins can't wake the chip from EM2 (only ports A/B can), so while idle a low-frequency
// timer wakes it briefly to look at the rows
#define IDLE_SCAN_MS    50
#define ROWS_MASK       0x0Fu  // PC0..PC3

static const sl_gpio_t ROWS[4] = {
  { SL_GPIO_PORT_C, 0 }, { SL_GPIO_PORT_C, 1 }, { SL_GPIO_PORT_C, 2 }, { SL_GPIO_PORT_C, 3 },
};
static const sl_gpio_t COLS[4] = {
  { SL_GPIO_PORT_C, 4 }, { SL_GPIO_PORT_C, 5 }, { SL_GPIO_PORT_C, 6 }, { SL_GPIO_PORT_C, 7 },
};

typedef enum {
  KEYPAD_IDLE,          // idle timer watches the rows
  KEYPAD_DEBOUNCE,      // edge seen, settle before scanning
  KEYPAD_HELD,          // key down, deciding short vs long
  KEYPAD_WAIT_RELEASE,  // already reported, wait so a held key doesn't repeat
} keypad_state_t;

static keypad_handler_t handler;
static sl_zigbee_af_event_t keypad_event;
// ISR events can only be activated (no delay), so the idle timer goes through this one
static sl_zigbee_af_event_t wake_event;
static keypad_state_t state;
static uint8_t held_key;
static uint32_t pressed_at_ms;
static sl_sleeptimer_timer_handle_t idle_timer;

// Idle: all columns LOW, so any key pulls its row LOW
static void columns_idle(void)
{
  for (int c = 0; c < 4; c++) {
    sl_gpio_set_pin_mode(&COLS[c], SL_GPIO_MODE_PUSH_PULL, false);
  }
}

static bool any_row_low(void)
{
  for (int r = 0; r < 4; r++) {
    bool level;
    sl_gpio_get_pin_input(&ROWS[r], &level);
    if (!level) {
      return true;
    }
  }
  return false;
}

uint8_t keypad_scan(void)
{
  uint8_t key = 0;
  for (int c = 0; c < 4 && key == 0; c++) {
    // Only the scanned column is driven; the rest float so they don't short through other keys
    for (int i = 0; i < 4; i++) {
      sl_gpio_set_pin_mode(&COLS[i], i == c ? SL_GPIO_MODE_PUSH_PULL : SL_GPIO_MODE_INPUT, false);
    }
    sl_udelay_wait(10);
    for (int r = 0; r < 4; r++) {
      bool level;
      sl_gpio_get_pin_input(&ROWS[r], &level);
      if (!level) {
        key = (uint8_t)(r * 4 + c + 1);
        break;
      }
    }
  }
  columns_idle();
  return key;
}

// Runs in interrupt context every IDLE_SCAN_MS; must stay tiny
static void idle_scan(sl_sleeptimer_timer_handle_t *timer, void *data)
{
  (void)data;
  if ((sl_hal_gpio_get_port_input(SL_GPIO_PORT_C) & ROWS_MASK) == ROWS_MASK) {
    return;
  }
  sl_sleeptimer_stop_timer(timer);
  sl_zigbee_af_event_set_active(&wake_event);
}

static void wake_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  state = KEYPAD_DEBOUNCE;
  sl_zigbee_af_event_set_delay_ms(&keypad_event, DEBOUNCE_MS);
}

static void idle_start(void)
{
  state = KEYPAD_IDLE;
  sl_sleeptimer_start_periodic_timer_ms(&idle_timer, IDLE_SCAN_MS, idle_scan, NULL, 0,
                                        SL_SLEEPTIMER_NO_HIGH_PRECISION_HF_CLOCKS_REQUIRED_FLAG);
}

static void keypad_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;
  switch (state) {
    case KEYPAD_DEBOUNCE:
      held_key = keypad_scan();
      if (held_key == 0) {
        idle_start();
        return;
      }
      pressed_at_ms = halCommonGetInt32uMillisecondTick();
      state = KEYPAD_HELD;
      sl_zigbee_af_event_set_delay_ms(&keypad_event, HOLD_POLL_MS);
      return;

    case KEYPAD_HELD:
      if (keypad_scan() != held_key) {
        handler(held_key, false);
        state = KEYPAD_WAIT_RELEASE;
      } else if (halCommonGetInt32uMillisecondTick() - pressed_at_ms >= LONG_PRESS_MS) {
        handler(held_key, true);
        state = KEYPAD_WAIT_RELEASE;
      }
      sl_zigbee_af_event_set_delay_ms(&keypad_event, HOLD_POLL_MS);
      return;

    case KEYPAD_WAIT_RELEASE:
      if (any_row_low()) {
        sl_zigbee_af_event_set_delay_ms(&keypad_event, HOLD_POLL_MS);
        return;
      }
      idle_start();
      return;

    case KEYPAD_IDLE:
    default:
      return;
  }
}

void keypad_init(keypad_handler_t key_handler)
{
  handler = key_handler;
  sl_zigbee_af_event_init(&keypad_event, keypad_event_handler);
  sl_zigbee_af_isr_event_init(&wake_event, wake_event_handler);
  columns_idle();
  for (int r = 0; r < 4; r++) {
    sl_gpio_set_pin_mode(&ROWS[r], SL_GPIO_MODE_INPUT_PULL, true);
  }
  idle_start();
}

void keypad_prepare_em4(void)
{
  columns_idle();
  sl_sleeptimer_stop_timer(&idle_timer);
  // EM4 keeps pin states only with retention; the pull-up on row 1 must survive
  GPIO_EM4EnablePinWakeup(GPIO_IEN_EM4WUIEN6, 0);
}
