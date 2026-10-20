#pragma once
#include <stdint.h>

// Li-Po on the XIAO BAT pads: PD3 enables the divider, PD4 reads VBAT/2
void battery_init(void);
// Blocks ~25 ms. Call before transmitting: radio load only ever pulls the reading down.
uint16_t battery_read_mv(void);
uint8_t battery_percent(uint16_t mv);
