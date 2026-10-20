#include "board.h"
#include "sl_gpio.h"
#include "sl_udelay.h"

// RF switch between the chip and the antennas: PB5 powers it, PB4 picks the antenna
// (LOW = onboard ceramic, HIGH = U.FL). Unpowered, even the onboard antenna is starved.
static const sl_gpio_t RF_SWITCH_POWER = { SL_GPIO_PORT_B, 5 };
static const sl_gpio_t RF_SWITCH_SELECT = { SL_GPIO_PORT_B, 4 };
#define USE_EXTERNAL_ANTENNA false

static const sl_gpio_t LED = { SL_GPIO_PORT_A, 7 };  // active low

// Onboard SPI flash, unused here. Left in standby it draws more than the sleeping chip.
static const sl_gpio_t FLASH_CS = { SL_GPIO_PORT_A, 6 };
static const sl_gpio_t FLASH_SCK = { SL_GPIO_PORT_A, 3 };
static const sl_gpio_t FLASH_MOSI = { SL_GPIO_PORT_A, 5 };
#define FLASH_CMD_DEEP_POWER_DOWN 0xB9

static void flash_deep_power_down(void)
{
  sl_gpio_set_pin_mode(&FLASH_CS, SL_GPIO_MODE_PUSH_PULL, true);
  sl_gpio_set_pin_mode(&FLASH_SCK, SL_GPIO_MODE_PUSH_PULL, false);
  sl_gpio_set_pin_mode(&FLASH_MOSI, SL_GPIO_MODE_PUSH_PULL, false);
  sl_udelay_wait(50);

  sl_gpio_clear_pin(&FLASH_CS);
  for (int bit = 7; bit >= 0; bit--) {
    if (FLASH_CMD_DEEP_POWER_DOWN & (1u << bit)) {
      sl_gpio_set_pin(&FLASH_MOSI);
    } else {
      sl_gpio_clear_pin(&FLASH_MOSI);
    }
    sl_gpio_set_pin(&FLASH_SCK);
    sl_udelay_wait(1);
    sl_gpio_clear_pin(&FLASH_SCK);
    sl_udelay_wait(1);
  }
  sl_gpio_set_pin(&FLASH_CS);

  // CS stays high so the flash keeps sleeping; the bus pins don't need to drive anything
  sl_gpio_set_pin_mode(&FLASH_SCK, SL_GPIO_MODE_DISABLED, false);
  sl_gpio_set_pin_mode(&FLASH_MOSI, SL_GPIO_MODE_DISABLED, false);
}

void board_init(void)
{
  sl_gpio_set_pin_mode(&RF_SWITCH_POWER, SL_GPIO_MODE_PUSH_PULL, true);
  sl_gpio_set_pin_mode(&RF_SWITCH_SELECT, SL_GPIO_MODE_PUSH_PULL, USE_EXTERNAL_ANTENNA);
  sl_gpio_set_pin_mode(&LED, SL_GPIO_MODE_PUSH_PULL, true);
  flash_deep_power_down();
}

void board_led(bool on)
{
  if (on) {
    sl_gpio_clear_pin(&LED);
  } else {
    sl_gpio_set_pin(&LED);
  }
}

void board_rf_off(void)
{
  sl_gpio_clear_pin(&RF_SWITCH_POWER);
}
