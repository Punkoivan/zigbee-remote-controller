#pragma once
#include <stdbool.h>

// Seeed Studio XIAO MG24 board bring-up: RF switch, LED, external SPI flash
void board_init(void);
void board_led(bool on);
// Unpower the RF switch before EM4
void board_rf_off(void);
