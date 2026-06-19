/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

// Device to Dongle
#define ZMK_2G4_MSG_KEYBOARD_REPORT 0x01
#define ZMK_2G4_MSG_CONSUMER_REPORT 0x02
#define ZMK_2G4_MSG_MOUSE_REPORT 0x03
#define ZMK_2G4_MSG_SYSTEM_REPORT 0x04
#define ZMK_2G4_MSG_BOOT 0x05
/* Battery report: [0x06][level_left][level_right][level_num]
 * Each level: 0-100 (%), 0xFF = unavailable */
#define ZMK_2G4_MSG_BATTERY_REPORT 0x06
#define ZMK_2G4_BATTERY_UNKNOWN 0xFF
#define ZMK_2G4_MSG_STUDIO_RPC_TX 0x10
#define ZMK_2G4_MSG_KEEP_ALIVE 0xFE

// Dongle to Device
#define ZMK_2G4_ACK_LED_INDICATORS 0x01
#define ZMK_2G4_ACK_RESOLUTION_MULTIPLIER 0x02
#define ZMK_2G4_ACK_HOST_CONNECTED 0x03
#define ZMK_2G4_ACK_STUDIO_RPC_RX 0x10
