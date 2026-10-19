#pragma once
#include <stdint.h>

// Rejoin through the nearest router if the remote has no network/parent. Returns true when connected.
bool zigbee_ensure_connected();

// Send Level Control move_to_level(level) from `endpoint` straight to the coordinator, once.
// If the frame isn't delivered (e.g. the parent is gone), rejoin and retry once.
bool send_key(uint8_t endpoint, uint8_t level);

// Rejoin through a nearer router if the parent link has become weak (e.g. the remote was carried to another
// room while the old parent still answers). Call shortly after a send, once the coordinator's reply came in.
void zigbee_check_parent();

// Tell the coordinator our firmware version: an OTA Query Next Image request is what ZHA reads it from
void announce_firmware_version(uint32_t version);
