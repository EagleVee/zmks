/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 *
 * 2.4 GHz ESB keyboard transport — PTX-to-dongle HID link.
 *
 * When CONFIG_ZMK_SPLIT_ESB is enabled the radio is shared between two roles
 * via a simple time-multiplex hub:
 *
 *   PRX phase (default): radio listens on split pipes so peripheral halves
 *       (right, num, …) can deliver key events to the central.
 *
 *   PTX phase (brief, ~1-2 ms): triggered whenever a HID report must be sent
 *       to the dongle.  The hub switches to PTX, drains the pending-TX queue,
 *       then immediately returns to PRX.
 *
 * Pipe assignment (central side):
 *   pipe 0  — dongle HID link   (PTX address: base_addr_p0 + 2G4_ADDR_PREFIX)
 *   pipe 1  — split peripheral 0  (PRX address: base_addr_p1 + SPLIT_PIPE_PREFIX_0)
 *   pipe 2  — split peripheral 1  (PRX address: base_addr_p1 + SPLIT_PIPE_PREFIX_1)
 *
 * When CONFIG_ZMK_SPLIT_ESB is disabled the file behaves exactly as before:
 * always PTX, always sends directly.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <zmk/esb.h>
#include <zmk/hid.h>
#include <zmk/2g4.h>
#include <zmk/2g4_protocol.h>
#include <zmk/2g4_crypto.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
#include <zmk/split/esb/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/split/central.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Shared state ───────────────────────────────────────────────────────────── */

static struct zmk_esb_payload tx_payload;
static bool ready;
static struct k_work_delayable resend_work;
static struct k_work_delayable keepalive_work;
static struct k_work_delayable battery_report_work;

#define ZMK_2G4_BATTERY_REPORT_INTERVAL_MS 60000

#define ZMK_2G4_KEEPALIVE_MS 250
static uint8_t consec_fail_count;

/* Forward declaration — used as event_handler pointer inside hub work callbacks */
static void esb_event_handler(const struct zmk_esb_event *event);

/* ── Hub state (split-ESB mode only) ────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)

enum hub_state_t {
    HUB_PRX, /* radio in PRX — receiving from split peripherals */
    HUB_PTX, /* radio in PTX — sending HID reports to dongle    */
};

static enum hub_state_t hub_state = HUB_PRX;

/* Queue of encrypted payloads waiting to be sent to the dongle. */
K_MSGQ_DEFINE(ptx_msgq, sizeof(struct zmk_esb_payload), 8, 4);

static struct k_work ptx_window_work; /* PRX → PTX, drain one item */
static struct k_work prx_return_work; /* PTX → PRX (after TX result) */

/* ── Address configuration helpers ─────────────────────────────────────────── */

static void configure_all_addresses(void) {
    /*
     * Pipe 0 (dongle link): base_addr_p0 + 2G4_ADDR_PREFIX
     * Pipes 1..N (split):   base_addr_p1 + SPLIT_PIPE_PREFIX_{0,1,…}
     *
     * The driver uses base_addr_p0 for pipe 0 and base_addr_p1 for all other
     * pipes.  We need both base addresses set before we can call set_prefixes
     * for the full pipe array.
     */
    uint8_t base_p0[] = {
        CONFIG_ZMK_2G4_ADDR_BASE_0,
        CONFIG_ZMK_2G4_ADDR_BASE_1,
        CONFIG_ZMK_2G4_ADDR_BASE_2,
        CONFIG_ZMK_2G4_ADDR_BASE_3,
    };
    zmk_esb_set_base_address_0(base_p0);

    uint8_t base_p1[] = {
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_0,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_1,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_2,
        CONFIG_ZMK_SPLIT_ESB_ADDR_BASE_3,
    };
    zmk_esb_set_base_address_1(base_p1);

    /* Prefix array covers all configured pipes: [dongle, right, num, …] */
    static const uint8_t prefixes[] = {
        CONFIG_ZMK_2G4_ADDR_PREFIX,
        CONFIG_ZMK_SPLIT_ESB_PIPE_PREFIX_0,
#if CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS >= 2
        CONFIG_ZMK_SPLIT_ESB_PIPE_PREFIX_1,
#endif
    };
    zmk_esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));

    zmk_esb_set_rf_channel(CONFIG_ZMK_2G4_RF_CHANNEL);
    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);
}

/* Enqueue an already-encrypted payload for the next PTX window. */
static int hub_enqueue(const struct zmk_esb_payload *payload) {
    int ret = k_msgq_put(&ptx_msgq, payload, K_NO_WAIT);
    if (ret == -ENOMSG) {
        /* Queue full — drop the oldest item and retry. */
        struct zmk_esb_payload dummy;
        k_msgq_get(&ptx_msgq, &dummy, K_NO_WAIT);
        ret = k_msgq_put(&ptx_msgq, payload, K_NO_WAIT);
    }
    k_work_submit(&ptx_window_work);
    return ret;
}

/* ── PTX window work (PRX → PTX → send one item) ───────────────────────────── */

static void ptx_window_work_cb(struct k_work *work) {
    struct zmk_esb_payload payload;
    if (k_msgq_get(&ptx_msgq, &payload, K_NO_WAIT) != 0) {
        return; /* nothing to send */
    }

    if (hub_state == HUB_PTX) {
        /* Already in PTX (e.g. a resend after TX_FAILED). Just write. */
        zmk_esb_write_payload(&payload);
        return;
    }

    /* Switch PRX → PTX */
    zmk_esb_stop_rx();
    zmk_esb_disable();

    struct zmk_esb_config ptx_cfg = {
        .mode = ZMK_ESB_MODE_PTX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
        .retransmit_delay = CONFIG_ZMK_2G4_RETRANSMIT_DELAY,
        .retransmit_count = CONFIG_ZMK_2G4_RETRANSMIT_COUNT,
    };
    zmk_esb_init(&ptx_cfg);
    configure_all_addresses();

    hub_state = HUB_PTX;
    zmk_esb_write_payload(&payload);
}

/* ── PRX return work (PTX → PRX, called after TX_SUCCESS or giving up) ──────── */

static void prx_return_work_cb(struct k_work *work) {
    /* If more items queued, send the next one before switching back. */
    struct zmk_esb_payload payload;
    if (k_msgq_get(&ptx_msgq, &payload, K_NO_WAIT) == 0) {
        zmk_esb_write_payload(&payload);
        return; /* stay in PTX until this one completes */
    }

    /* Nothing more to send — return to PRX. */
    zmk_esb_disable();

    struct zmk_esb_config prx_cfg = {
        .mode = ZMK_ESB_MODE_PRX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
    };
    zmk_esb_init(&prx_cfg);
    configure_all_addresses();
    zmk_esb_start_rx();

    hub_state = HUB_PRX;
}

#else /* !CONFIG_ZMK_SPLIT_ESB — original single-address helper */

static void configure_dongle_addresses(void) {
    uint8_t base[] = {
        CONFIG_ZMK_2G4_ADDR_BASE_0,
        CONFIG_ZMK_2G4_ADDR_BASE_1,
        CONFIG_ZMK_2G4_ADDR_BASE_2,
        CONFIG_ZMK_2G4_ADDR_BASE_3,
    };
    zmk_esb_set_base_address_0(base);
    uint8_t prefix[] = {CONFIG_ZMK_2G4_ADDR_PREFIX};
    zmk_esb_set_prefixes(prefix, 1);
    zmk_esb_set_rf_channel(CONFIG_ZMK_2G4_RF_CHANNEL);
    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);
}

#endif /* CONFIG_ZMK_SPLIT_ESB */

/* ── ESB event handler ──────────────────────────────────────────────────────── */

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        consec_fail_count = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
        k_work_submit(&prx_return_work);
#endif
        break;

    case ZMK_ESB_EVENT_TX_FAILED:
        zmk_esb_flush_tx();
        if (++consec_fail_count < CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES) {
            k_work_reschedule(&resend_work, K_MSEC(1));
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
            /* Stay in PTX; resend_work will re-enqueue and ptx_window_work
             * will send it while still in PTX (hub_state == HUB_PTX). */
#endif
        } else {
            LOG_WRN("2.4G: %d consecutive TX failures, dongle unreachable", consec_fail_count);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
            k_work_submit(&prx_return_work); /* Give up this window, back to PRX */
#endif
        }
        break;

    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            if (rx.length < 1) {
                continue;
            }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
            if (hub_state == HUB_PRX) {
                /* Packet arrived on a split pipe from a peripheral half. */
                if (rx.pipe >= CONFIG_ZMK_SPLIT_ESB_PIPE_OFFSET) {
                    zmk_esb_split_central_on_rx(rx.pipe, rx.data, rx.length);
                    continue;
                }
            }
            /* Fall through: ACK payload from dongle while in PTX mode. */
#endif
            switch (rx.data[0]) {
            case ZMK_2G4_ACK_LED_INDICATORS:
                break;
            case ZMK_2G4_ACK_HOST_CONNECTED:
                break;
            default:
                break;
            }
        }
        break;
    }
    }
}

/* ── Keepalive ──────────────────────────────────────────────────────────────── */

static void keepalive_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    struct zmk_esb_payload ka = {
        .pipe = 0,
        .noack = false,
    };
    ka.data[0] = ZMK_2G4_MSG_KEEP_ALIVE;

    int enc_len = zmk_2g4_crypto_encrypt(ka.data, 1, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        goto reschedule;
    }
    ka.length = enc_len;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
    hub_enqueue(&ka);
#else
    zmk_esb_write_payload(&ka);
#endif

reschedule:
    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
}

/* ── Battery report ─────────────────────────────────────────────────────────── */

static void battery_report_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    uint8_t levels[3] = {ZMK_2G4_BATTERY_UNKNOWN, ZMK_2G4_BATTERY_UNKNOWN,
                         ZMK_2G4_BATTERY_UNKNOWN};

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    levels[0] = zmk_battery_state_of_charge();
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    /* Peripheral battery levels (best-effort; 0xFF if not yet fetched) */
    uint8_t peri_level;
    for (int i = 0; i < 2; i++) {
        if (zmk_split_central_get_peripheral_battery_level(i, &peri_level) == 0) {
            levels[i + 1] = peri_level;
        }
    }
#endif

    struct zmk_esb_payload batt = {
        .pipe = 0,
        .noack = false,
        .length = 4,
    };
    batt.data[0] = ZMK_2G4_MSG_BATTERY_REPORT;
    batt.data[1] = levels[0];
    batt.data[2] = levels[1];
    batt.data[3] = levels[2];

    int enc_len = zmk_2g4_crypto_encrypt(batt.data, 4, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        goto reschedule;
    }
    batt.length = enc_len;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
    hub_enqueue(&batt);
#else
    zmk_esb_write_payload(&batt);
#endif

reschedule:
    k_work_reschedule(&battery_report_work, K_MSEC(ZMK_2G4_BATTERY_REPORT_INTERVAL_MS));
}

/* ── Report sending ─────────────────────────────────────────────────────────── */

static void resend_work_handler(struct k_work *work) {
    if (!ready) {
        return;
    }
    zmk_2g4_send_keyboard_report();
    zmk_2g4_send_consumer_report();
}

static int send_report(uint8_t report_type, const uint8_t *body, size_t len) {
    if (!ready) {
        return -ENODEV;
    }

    consec_fail_count = 0;

    tx_payload.pipe = 0;
    tx_payload.noack = false;
    tx_payload.length = len + 1;
    tx_payload.data[0] = report_type;
    memcpy(&tx_payload.data[1], body, MIN(len, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH - 1));

    int enc_len =
        zmk_2g4_crypto_encrypt(tx_payload.data, len + 1, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        return enc_len;
    }
    tx_payload.length = enc_len;

    /* Push keepalive deadline forward on every report */
    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
    return hub_enqueue(&tx_payload);
#else
    int ret = zmk_esb_write_payload(&tx_payload);
    if (ret == -ENOMEM) {
        zmk_esb_flush_tx();
        ret = zmk_esb_write_payload(&tx_payload);
    }
    return ret;
#endif
}

int zmk_2g4_send_keyboard_report(void) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    return send_report(ZMK_2G4_MSG_KEYBOARD_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

int zmk_2g4_send_consumer_report(void) {
    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return send_report(ZMK_2G4_MSG_CONSUMER_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_2g4_send_mouse_report(void) {
    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return send_report(ZMK_2G4_MSG_MOUSE_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}
#endif

/* ── Boot announcement ──────────────────────────────────────────────────────── */

static int send_boot_announcement(void) {
    uint32_t reset_cause = 0;
    int hr = hwinfo_get_reset_cause(&reset_cause);
    if (hr == 0) {
        (void)hwinfo_clear_reset_cause();
    } else {
        LOG_WRN("hwinfo_get_reset_cause failed: %d", hr);
        reset_cause = 0xFFFFFFFFu;
    }

    uint8_t body[8];
    sys_put_le32(reset_cause, body);
    sys_put_le32(zmk_2g4_crypto_tx_session_id(), body + 4);

    int ret = send_report(ZMK_2G4_MSG_BOOT, body, sizeof(body));
    LOG_INF("2.4G BOOT sent: reason=0x%08x session=0x%08x ret=%d", reset_cause,
            zmk_2g4_crypto_tx_session_id(), ret);
    return ret;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

bool zmk_2g4_is_ready(void) { return ready; }

int zmk_2g4_start(void) {
    if (ready) {
        return 0;
    }

    k_work_init_delayable(&resend_work, resend_work_handler);
    k_work_init_delayable(&keepalive_work, keepalive_handler);
    k_work_init_delayable(&battery_report_work, battery_report_handler);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
    k_work_init(&ptx_window_work, ptx_window_work_cb);
    k_work_init(&prx_return_work, prx_return_work_cb);
    k_msgq_purge(&ptx_msgq);
    hub_state = HUB_PRX;

    /* Start in PRX so peripheral halves can reach us immediately. */
    struct zmk_esb_config prx_cfg = {
        .mode = ZMK_ESB_MODE_PRX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
    };
    int ret = zmk_esb_init(&prx_cfg);
    if (ret) {
        LOG_ERR("ESB hub PRX init failed: %d", ret);
        return ret;
    }
    configure_all_addresses();
    zmk_esb_start_rx();

    zmk_esb_split_central_set_available(true);
    LOG_INF("2.4G hub started (PRX+PTX split mode)");

#else /* PTX-only (no split ESB) */

    struct zmk_esb_config ptx_cfg = {
        .mode = ZMK_ESB_MODE_PTX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
        .retransmit_delay = CONFIG_ZMK_2G4_RETRANSMIT_DELAY,
        .retransmit_count = CONFIG_ZMK_2G4_RETRANSMIT_COUNT,
    };
    int ret = zmk_esb_init(&ptx_cfg);
    if (ret) {
        LOG_ERR("ESB init failed: %d", ret);
        return ret;
    }
    configure_dongle_addresses();
    LOG_INF("2.4G transport started (PTX-only mode)");

#endif /* CONFIG_ZMK_SPLIT_ESB */

    ready = true;
    send_boot_announcement();
    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
    /* Send initial battery report shortly after start, then every minute */
    k_work_reschedule(&battery_report_work, K_MSEC(5000));

    return 0;
}

int zmk_2g4_stop(void) {
    if (!ready) {
        return 0;
    }

    ready = false;
    k_work_cancel_delayable(&resend_work);
    k_work_cancel_delayable(&keepalive_work);
    k_work_cancel_delayable(&battery_report_work);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB)
    k_work_cancel(&ptx_window_work);
    k_work_cancel(&prx_return_work);
    k_msgq_purge(&ptx_msgq);
    zmk_esb_split_central_set_available(false);
    hub_state = HUB_PRX;
#endif

    zmk_esb_flush_tx();
    zmk_esb_flush_rx();
    zmk_esb_disable();
    LOG_INF("2.4G transport stopped");
    return 0;
}
