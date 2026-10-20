#pragma once
#include <stdbool.h>
#include <stdint.h>

// 4x4 matrix keypad: rows R1..R4 on PC0..PC3 (inputs with pull-up, polled while idle),
// columns C1..C4 on PC4..PC7 (held LOW while idle). Key number = (row - 1) * 4 + column, 1..16.

typedef void (*keypad_handler_t)(uint8_t key, bool long_press);

void keypad_init(keypad_handler_t handler);
// Blocking scan, 0 if nothing is pressed. Used at boot for the factory reset check.
uint8_t keypad_scan(void);
// EM4 wake-up on row 1 (PC0 = EM4WU6)
void keypad_prepare_em4(void);
