/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Onboard Nice Nano LED (gpio0 15, GPIO_ACTIVE_HIGH) system status indicator.
 *
 * CENTRAL (left half):
 *   USB plugged   → solid ON until USB removed.
 *   USB removed   → LED OFF.
 *   BLE scanning  → 4 Hz blink while advertising / not connected.
 *   Mode trigger  → fired once on each transport selection:
 *       BLE endpoint   : 1 quick blink (200 ms on / 200 ms off)
 *       2G4 dongle mode: 2 quick blinks (200 ms on / 200 ms off × 2)
 *
 * PERIPHERAL (right / num):
 *   Boot blink → 5 Hz for CONFIG_ZMK_STATUS_LED_PERIPHERAL_BOOT_BLINK_MS ms,
 *                then LED OFF.  This gives a ~30 s "pairing window" indicator.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk_status_led, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>

/* ── LED hardware ──────────────────────────────────────────────────────── */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(blue_led), okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
#else
#error "status_led: blue_led node not found in DTS"
#endif

static inline void led_on(void)  { gpio_pin_set_dt(&led, 1); }
static inline void led_off(void) { gpio_pin_set_dt(&led, 0); }

/* ── Blink work ────────────────────────────────────────────────────────── */

struct blink_ctx {
    struct k_work_delayable work;
    uint32_t period_ms;   /* half-period of the square wave                   */
    int32_t  remaining;   /* pulses left; -1 = continuous; 0 = stop            */
    uint8_t  state;       /* current LED state within the pattern              */
};

static struct blink_ctx blink;

static void blink_work_cb(struct k_work *work) {
    struct blink_ctx *ctx = CONTAINER_OF(work, struct blink_ctx, work.work);

    if (ctx->remaining == 0) {
        led_off();
        return;
    }

    ctx->state ^= 1;
    if (ctx->state) {
        led_on();
    } else {
        led_off();
        if (ctx->remaining > 0) {
            ctx->remaining--;
        }
    }

    if (ctx->remaining != 0) {
        k_work_reschedule(&ctx->work, K_MSEC(ctx->period_ms));
    } else {
        led_off();
    }
}

/* Start or restart a blink pattern.
 *   period_ms : half-period (LED on time == LED off time)
 *   pulses    : number of on-pulses; -1 = continuous until stop_blink()
 */
static void start_blink(uint32_t period_ms, int32_t pulses) {
    k_work_cancel_delayable(&blink.work);
    blink.period_ms = period_ms;
    blink.remaining = (pulses < 0) ? -1 : pulses * 2; /* × 2: on + off per pulse */
    blink.state     = 0;
    led_off();
    k_work_reschedule(&blink.work, K_MSEC(period_ms));
}

static void stop_blink(void) {
    k_work_cancel_delayable(&blink.work);
    blink.remaining = 0;
    led_off();
}

/* ── CENTRAL behaviour ─────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_2G4)
#include <zmk/2g4.h>
#endif

#include <zmk/events/endpoint_changed.h>

static bool usb_powered = false;

static void central_update_led(void) {
    if (usb_powered) {
        stop_blink();
        led_on();
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (!zmk_ble_active_profile_is_connected()) {
        /* Scanning / advertising: fast blink (4 Hz) */
        start_blink(125, -1);
        return;
    }
#endif

    led_off();
}

static void fire_mode_blink(void) {
    if (usb_powered) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_2G4)
    if (zmk_2g4_is_ready()) {
        start_blink(200, 2); /* dongle mode: 2 quick blinks */
        return;
    }
#endif

    start_blink(200, 1); /* BLE mode: 1 quick blink */
}

#if IS_ENABLED(CONFIG_ZMK_USB)
static int central_usb_handler(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (!ev) return ZMK_EV_EVENT_BUBBLE;

    bool was_powered = usb_powered;
    usb_powered = (ev->conn_state == USB_DC_CONNECTED || ev->conn_state == USB_DC_CONFIGURED ||
                   ev->conn_state == USB_DC_RESUME);
    if (usb_powered != was_powered) {
        central_update_led();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led_usb, central_usb_handler);
ZMK_SUBSCRIPTION(status_led_usb, zmk_usb_conn_state_changed);
#endif /* CONFIG_ZMK_USB */

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int central_ble_handler(const zmk_event_t *eh) {
    central_update_led();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led_ble, central_ble_handler);
ZMK_SUBSCRIPTION(status_led_ble, zmk_ble_active_profile_changed);
#endif

static int central_endpoint_handler(const zmk_event_t *eh) {
    fire_mode_blink();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led_endpoint, central_endpoint_handler);
ZMK_SUBSCRIPTION(status_led_endpoint, zmk_endpoint_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

/* ── PERIPHERAL behaviour (right / num) ───────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct k_work_delayable boot_blink_stop_work;

static void boot_blink_stop_cb(struct k_work *work) {
    LOG_DBG("peripheral boot blink timeout");
    stop_blink();
}

#endif /* peripheral */

/* ── Common init ───────────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct k_work_delayable initial_mode_blink_work;

static void initial_mode_blink_cb(struct k_work *w) {
    fire_mode_blink();
}
#endif

static int status_led_init(void) {
    if (!device_is_ready(led.port)) {
        LOG_ERR("status_led GPIO port not ready");
        return -ENODEV;
    }
    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("status_led configure failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&blink.work, blink_work_cb);

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Blink at 5 Hz for 30 s to signal "waiting to pair/connect" */
    k_work_init_delayable(&boot_blink_stop_work, boot_blink_stop_cb);
    start_blink(100, -1); /* 100 ms half-period = 5 Hz */
    k_work_schedule(&boot_blink_stop_work,
                    K_MSEC(CONFIG_ZMK_STATUS_LED_PERIPHERAL_BOOT_BLINK_MS));
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_USB)
    usb_powered = zmk_usb_is_powered();
#endif
    central_update_led();
    /* Fire the mode blink after a short delay so the 2G4 radio has time to
     * start and zmk_2g4_is_ready() returns the correct value. */
    k_work_init_delayable(&initial_mode_blink_work, initial_mode_blink_cb);
    k_work_schedule(&initial_mode_blink_work, K_MSEC(3000));
#endif

    return 0;
}

/* Run after the ZMK application subsystems have initialised */
SYS_INIT(status_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY + 1);
