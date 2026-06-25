/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

bool zmk_split_bt_peripheral_is_connected(void);

bool zmk_split_bt_peripheral_is_bonded(void);

/* True once the BLE split transport has given up trying to (re)connect, i.e. the
 * give-up timer has elapsed and BLE reports itself unavailable.  Used by the ESB
 * transport to schedule its own availability only after BLE has actually
 * concluded, rather than at raw disconnect time. */
bool zmk_split_bt_peripheral_gave_up(void);