/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/*
 * Called by 2g4.c when a packet arrives on a split pipe (pipe >= PIPE_OFFSET)
 * while the radio is in PRX mode.  central.c maps pipe → source index and
 * injects the event into the ZMK split transport framework.
 */
void zmk_esb_split_central_on_rx(uint8_t pipe, const uint8_t *data, uint8_t len);

/*
 * Called by 2g4.c to inform central.c that the ESB hub has started/stopped.
 * central.c uses this to report transport availability to ZMK.
 */
void zmk_esb_split_central_set_available(bool available);
