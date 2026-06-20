/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Sprite-based bongo cat animation widget driven by WPM.
 *
 * On ZMK_2G4_DONGLE builds, typing activity is detected via the
 * zmk_dongle_link_state_changed event, which fires immediately on every
 * received 2G4 radio packet (connected=true) and 500 ms after the last
 * packet (connected=false).  This gives zero-delay response compared to
 * the WPM timer which only fires every 1 second.
 * WPM events are still used to escalate the speed tier (slow→mid→fast).
 *
 * On non-dongle builds, zmk_keycode_state_changed is used instead.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/widgets/bongo_cat.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>

#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
#include <zmk/events/dongle_link_state_changed.h>
#else
#include <zmk/events/keycode_state_changed.h>
#endif

#define SRC(array) (const void **)array, ARRAY_SIZE(array)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

LV_IMG_DECLARE(bongo_cat_none);
LV_IMG_DECLARE(bongo_cat_left1);
LV_IMG_DECLARE(bongo_cat_left2);
LV_IMG_DECLARE(bongo_cat_right1);
LV_IMG_DECLARE(bongo_cat_right2);
LV_IMG_DECLARE(bongo_cat_both1);
LV_IMG_DECLARE(bongo_cat_both1_open);
LV_IMG_DECLARE(bongo_cat_both2);

#define ANIMATION_SPEED_IDLE 10000
static const lv_img_dsc_t *idle_imgs[] = {
    &bongo_cat_both1_open,
    &bongo_cat_both1_open,
    &bongo_cat_both1_open,
    &bongo_cat_both1,
};

#define ANIMATION_SPEED_SLOW 2000
static const lv_img_dsc_t *slow_imgs[] = {
    &bongo_cat_left1,
    &bongo_cat_both1,
    &bongo_cat_both1,
    &bongo_cat_right1,
    &bongo_cat_both1,
    &bongo_cat_both1,
    &bongo_cat_left1,
    &bongo_cat_both1,
    &bongo_cat_both1,
};

#define ANIMATION_SPEED_MID 500
static const lv_img_dsc_t *mid_imgs[] = {
    &bongo_cat_left2,
    &bongo_cat_left1,
    &bongo_cat_none,
    &bongo_cat_right2,
    &bongo_cat_right1,
    &bongo_cat_none,
};

#define ANIMATION_SPEED_FAST 200
static const lv_img_dsc_t *fast_imgs[] = {
    &bongo_cat_both2,
    &bongo_cat_both1,
    &bongo_cat_none,
    &bongo_cat_none,
};

struct bongo_cat_state {
    uint8_t wpm;
    bool active; /* true = typing (dongle link up / key pressed) */
};

enum anim_state {
    anim_state_none,
    anim_state_idle,
    anim_state_slow,
    anim_state_mid,
    anim_state_fast,
} current_anim_state;

static void set_animation(lv_obj_t *animing, struct bongo_cat_state state) {
    enum anim_state next;

    if (!state.active) {
        next = anim_state_idle;
    } else if (state.wpm < 30) {
        next = anim_state_slow;
    } else if (state.wpm < 70) {
        next = anim_state_mid;
    } else {
        next = anim_state_fast;
    }

    if (next == current_anim_state) {
        return;
    }

    current_anim_state = next;

    switch (next) {
    case anim_state_idle:
        lv_animimg_set_src(animing, SRC(idle_imgs));
        lv_animimg_set_duration(animing, ANIMATION_SPEED_IDLE);
        break;
    case anim_state_slow:
        lv_animimg_set_src(animing, SRC(slow_imgs));
        lv_animimg_set_duration(animing, ANIMATION_SPEED_SLOW);
        break;
    case anim_state_mid:
        lv_animimg_set_src(animing, SRC(mid_imgs));
        lv_animimg_set_duration(animing, ANIMATION_SPEED_MID);
        break;
    case anim_state_fast:
        lv_animimg_set_src(animing, SRC(fast_imgs));
        lv_animimg_set_duration(animing, ANIMATION_SPEED_FAST);
        break;
    default:
        return;
    }
    lv_animimg_set_repeat_count(animing, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(animing);
}

static struct bongo_cat_state get_state(const zmk_event_t *eh) {
    bool active;

#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    const struct zmk_dongle_link_state_changed *link_ev = as_zmk_dongle_link_state_changed(eh);
    active = link_ev ? link_ev->connected : zmk_dongle_link_is_connected();
#else
    /* On non-dongle builds derive activity from WPM or keycode events */
    const struct zmk_keycode_state_changed *key_ev = as_zmk_keycode_state_changed(eh);
    uint8_t wpm = zmk_wpm_get_state();
    active = (wpm > 0) || (key_ev != NULL && key_ev->state);
#endif

    return (struct bongo_cat_state){.wpm = zmk_wpm_get_state(), .active = active};
}

static void update_cb(struct bongo_cat_state state) {
    struct zmk_widget_bongo_cat *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_animation(widget->obj, state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_bongo_cat, struct bongo_cat_state, update_cb, get_state)
ZMK_SUBSCRIPTION(widget_bongo_cat, zmk_wpm_state_changed);
#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
ZMK_SUBSCRIPTION(widget_bongo_cat, zmk_dongle_link_state_changed);
#else
ZMK_SUBSCRIPTION(widget_bongo_cat, zmk_keycode_state_changed);
#endif

int zmk_widget_bongo_cat_init(struct zmk_widget_bongo_cat *widget, lv_obj_t *parent) {
    widget->obj = lv_animimg_create(parent);
    lv_obj_center(widget->obj);

    sys_slist_append(&widgets, &widget->node);
    widget_bongo_cat_init();
    return 0;
}

lv_obj_t *zmk_widget_bongo_cat_obj(struct zmk_widget_bongo_cat *widget) {
    return widget->obj;
}
