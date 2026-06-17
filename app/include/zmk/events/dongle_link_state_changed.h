/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_dongle_link_state_changed {
    bool connected;
};

ZMK_EVENT_DECLARE(zmk_dongle_link_state_changed);

bool zmk_dongle_link_is_connected(void);
