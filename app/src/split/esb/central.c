/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 *
 * ESB split central transport.
 *
 * Runs on the central (left) half alongside the 2G4 dongle transport.  This
 * file registers the ZMK split-central transport slot; the actual radio
 * time-multiplexing (PRX ↔ PTX switching) is managed inside 2g4.c.
 *
 * When the 2G4 hub receives a packet on a split pipe it calls
 * zmk_esb_split_central_on_rx(), which deserialises the event and injects it
 * into the ZMK split transport framework (→ keymap processing).
 */

#include <zephyr/kernel.h>
#include <string.h>

#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>
#include <zmk/split/esb/central.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── State ─────────────────────────────────────────────────────────────────── */

static bool available; /* true while the 2G4 ESB hub is running */
static bool enabled;

/*
 * Per-source connectivity: bit N is set if we have received at least one
 * packet from peripheral index N since the hub was last started.
 */
static uint32_t connected_mask;

#define ALL_CONNECTED_MASK ((1u << CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS) - 1u)

static zmk_split_transport_central_status_changed_cb_t status_cb;

/* ── Forward declarations ───────────────────────────────────────────────────── */

static void notify_status(void);

/* ── RX injection (called from 2g4.c) ──────────────────────────────────────── */

void zmk_esb_split_central_on_rx(uint8_t pipe, const uint8_t *data, uint8_t len) {
    if (pipe < CONFIG_ZMK_SPLIT_ESB_PIPE_OFFSET) {
        /* Pipe 0 is the dongle link — should not appear here */
        return;
    }

    uint8_t source = pipe - CONFIG_ZMK_SPLIT_ESB_PIPE_OFFSET;
    if (source >= CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS) {
        LOG_WRN("ESB split central: unexpected pipe %u (source %u >= max %d)", pipe, source,
                CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS);
        return;
    }
    if (len < sizeof(struct zmk_split_transport_peripheral_event)) {
        LOG_WRN("ESB split central: short packet from source %u (%u bytes)", source, len);
        return;
    }

    bool was_connected = (connected_mask & BIT(source)) != 0;
    connected_mask |= BIT(source);
    if (!was_connected) {
        LOG_INF("ESB split: peripheral %u connected (pipe %u)", source, pipe);
        notify_status();
    }

    struct zmk_split_transport_peripheral_event ev;
    memcpy(&ev, data, sizeof(ev));

    zmk_split_transport_central_peripheral_event_handler(&esb_central, source, ev);
}

void zmk_esb_split_central_set_available(bool avail) {
    if (avail == available) {
        return;
    }
    available = avail;
    if (!avail) {
        connected_mask = 0;
        LOG_INF("ESB split: hub stopped, peripherals disconnected");
    } else {
        LOG_INF("ESB split: hub started, waiting for peripherals");
    }
    notify_status();
}

/* ── Transport API ──────────────────────────────────────────────────────────── */

static int esb_central_send_command(uint8_t source,
                                    struct zmk_split_transport_central_command cmd) {
    /* Central→peripheral commands (RGB sync, HID indicators, …) are not yet
     * implemented over ESB.  Return success so callers do not error out. */
    return 0;
}

static int esb_central_get_available_source_ids(uint8_t *sources) {
    for (uint8_t i = 0; i < CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS; i++) {
        sources[i] = i;
    }
    return CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS;
}

static int esb_central_set_enabled(bool en) {
    enabled = en;
    notify_status();
    return 0;
}

static struct zmk_split_transport_status esb_central_get_status(void) {
    enum zmk_split_transport_connections_status conn;
    if (connected_mask == ALL_CONNECTED_MASK) {
        conn = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
    } else if (connected_mask != 0) {
        conn = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED;
    } else {
        conn = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
    }

    return (struct zmk_split_transport_status){
        .available = available,
        .enabled = enabled,
        .connections = conn,
    };
}

static int
esb_central_set_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    status_cb = cb;
    return 0;
}

static const struct zmk_split_transport_central_api central_api = {
    .send_command = esb_central_send_command,
    .get_available_source_ids = esb_central_get_available_source_ids,
    .set_enabled = esb_central_set_enabled,
    .get_status = esb_central_get_status,
    .set_status_callback = esb_central_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(esb_central, &central_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

/* ── Notify helpers ─────────────────────────────────────────────────────────── */

static void notify_status_work_cb(struct k_work *work) {
    if (status_cb) {
        status_cb(&esb_central, esb_central_get_status());
    }
}
static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

static void notify_status(void) { k_work_submit(&notify_status_work); }
