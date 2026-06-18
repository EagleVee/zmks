/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 *
 * ESB split peripheral transport.
 *
 * Runs on each split peripheral half (right, num, …).  Transmits key-position
 * and sensor events to the central (left) via ESB PTX on a dedicated pipe.
 *
 * Address scheme
 * ──────────────
 * The central (PRX) listens on pipe (PIPE_OFFSET + PERIPHERAL_INDEX) whose
 * address is built from base_addr_p1 + prefix[PIPE_OFFSET + PERIPHERAL_INDEX].
 * On the peripheral we replicate that same 5-byte address on our local pipe 0
 * (base_addr_p0 + prefix[0]) so packets land on the correct pipe at the
 * central.
 *
 * Retry behaviour
 * ───────────────
 * The central is briefly in PTX mode when forwarding HID reports to the dongle.
 * During that window (~1-2 ms) our packets receive no ACK.  The ESB retransmit
 * counter (default 10 × 600 µs = 6 ms) covers that window transparently.
 * On TX_FAILED we schedule a short software retry via a delayable work item.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include <zmk/esb.h>
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <=
                 CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH,
             "zmk_split_transport_peripheral_event does not fit in ESB payload");

/* ── State ─────────────────────────────────────────────────────────────────── */

static bool enabled;
static bool connected; /* true after a successful TX to the central */

static struct zmk_esb_payload pending_payload;
static bool pending; /* a serialised event is waiting to be ACKed */

static void retry_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(retry_work, retry_work_cb);
static zmk_split_transport_peripheral_status_changed_cb_t status_cb;

/* ── Helpers ────────────────────────────────────────────────────────────────── */

static void notify_status(void);

static void configure_addresses(void) {
    /* base_addr_p0 must equal the central's base_addr_p1 (the split base) */
    uint8_t base[] = {
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_0,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_1,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_2,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_3,
    };
    zmk_esb_set_base_address_0(base);

    /* prefix[0] = the central's prefix for our pipe index */
    static const uint8_t pipe_prefixes[] = {
        CONFIG_ZMK_SPLIT_ESB_PIPE_PREFIX_0,
        CONFIG_ZMK_SPLIT_ESB_PIPE_PREFIX_1,
    };
    uint8_t prefix[1] = {pipe_prefixes[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_INDEX]};
    zmk_esb_set_prefixes(prefix, 1);

    zmk_esb_set_rf_channel(CONFIG_ZMK_SPLIT_ESB_RF_CHANNEL);
    zmk_esb_set_tx_power(CONFIG_ZMK_SPLIT_ESB_TX_POWER);
}

/* ── ESB event handler ──────────────────────────────────────────────────────── */

static void esb_event_handler(const struct zmk_esb_event *evt) {
    switch (evt->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        pending = false;
        if (!connected) {
            connected = true;
            notify_status();
        }
        break;

    case ZMK_ESB_EVENT_TX_FAILED:
        zmk_esb_flush_tx();
        if (connected) {
            connected = false;
            notify_status();
        }
        if (pending && enabled) {
            /* Software retry: give the central time to finish its PTX window */
            k_work_reschedule(&retry_work, K_MSEC(10));
        }
        break;

    case ZMK_ESB_EVENT_RX_RECEIVED: {
        /* ACK payloads from central — reserved for future central→peripheral
         * commands (RGB sync, HID indicators, …). Drain and discard for now. */
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
        }
        break;
    }
    }
}

static void retry_work_cb(struct k_work *work) {
    if (pending && enabled) {
        int ret = zmk_esb_write_payload(&pending_payload);
        if (ret < 0) {
            LOG_WRN("ESB split peripheral: retry write failed %d", ret);
        }
    }
}

/* ── Transport API ──────────────────────────────────────────────────────────── */

static int esb_peripheral_report_event(const struct zmk_split_transport_peripheral_event *ev) {
    if (!enabled) {
        return -ENODEV;
    }

    pending_payload.pipe = 0;
    pending_payload.noack = false;
    pending_payload.length = sizeof(*ev);
    memcpy(pending_payload.data, ev, sizeof(*ev));

    pending = true;
    int ret = zmk_esb_write_payload(&pending_payload);
    if (ret == -ENOMEM) {
        zmk_esb_flush_tx();
        ret = zmk_esb_write_payload(&pending_payload);
    }
    return ret;
}

static int esb_peripheral_set_enabled(bool en) {
    if (en == enabled) {
        return 0;
    }
    enabled = en;

    if (en) {
        struct zmk_esb_config cfg = {
            .mode = ZMK_ESB_MODE_PTX,
            .bitrate = ZMK_ESB_BITRATE_2MBPS,
            .crc = ZMK_ESB_CRC_16BIT,
            .tx_mode = ZMK_ESB_TXMODE_AUTO,
            .event_handler = esb_event_handler,
            .selective_auto_ack = false,
            .retransmit_delay = CONFIG_ZMK_SPLIT_ESB_RETRANSMIT_DELAY,
            .retransmit_count = CONFIG_ZMK_SPLIT_ESB_RETRANSMIT_COUNT,
        };
        int ret = zmk_esb_init(&cfg);
        if (ret) {
            LOG_ERR("ESB split peripheral: init failed %d", ret);
            enabled = false;
            return ret;
        }
        configure_addresses();
        LOG_INF("ESB split peripheral %d started", CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_INDEX);
    } else {
        k_work_cancel_delayable(&retry_work);
        zmk_esb_flush_tx();
        zmk_esb_disable();
        connected = false;
        pending = false;
        LOG_INF("ESB split peripheral %d stopped", CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_INDEX);
    }

    notify_status();
    return 0;
}

static struct zmk_split_transport_status esb_peripheral_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = enabled,
        .connections = connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                 : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static int
esb_peripheral_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    status_cb = cb;
    return 0;
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = esb_peripheral_report_event,
    .set_enabled = esb_peripheral_set_enabled,
    .get_status = esb_peripheral_get_status,
    .set_status_callback = esb_peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &peripheral_api,
                                        CONFIG_ZMK_SPLIT_ESB_PRIORITY);

/* ── Notify helpers ─────────────────────────────────────────────────────────── */

static void notify_status_work_cb(struct k_work *work) {
    if (status_cb) {
        status_cb(&esb_peripheral, esb_peripheral_get_status());
    }
}
static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

static void notify_status(void) { k_work_submit(&notify_status_work); }

/* Transport selector calls set_enabled(true) when this transport is chosen.
 * No SYS_INIT needed — retry_work is statically initialised above. */
