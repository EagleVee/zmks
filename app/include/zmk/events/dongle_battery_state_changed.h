/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/* Fired on the dongle whenever a battery-level report arrives from the
 * keyboard over the 2.4 GHz link.
 *
 * device indices:
 *   0 = left (central)
 *   1 = right (peripheral 0)
 *   2 = num   (peripheral 1)
 *
 * level: 0-100 (%)  |  0xFF = unavailable
 */
struct zmk_dongle_battery_state_changed {
    uint8_t device_index;
    uint8_t level;
};

ZMK_EVENT_DECLARE(zmk_dongle_battery_state_changed);
