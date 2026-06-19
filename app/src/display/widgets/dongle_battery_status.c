/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Dongle battery-status widget.
 *
 * Displays battery levels for all three keyboard halves received over the
 * 2.4 GHz link.  Example label text:  "L:85 R:72 N:91"
 * Unknown level (0xFF) is shown as "--".
 *
 * Updated at most once per 2G4 battery report interval (typically 60 s).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <stdio.h>

#include <zmk/display.h>
#include <zmk/display/widgets/dongle_battery_status.h>
#include <zmk/events/dongle_battery_state_changed.h>
#include <zmk/event_manager.h>

#define BATT_UNKNOWN 0xFF
#define NUM_DEVICES  3

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* Accumulated battery levels for L / R / N */
static uint8_t battery_levels[NUM_DEVICES] = {BATT_UNKNOWN, BATT_UNKNOWN, BATT_UNKNOWN};

static void refresh_widgets(void) {
    char text[20];
    const char *const names[NUM_DEVICES] = {"L", "R", "N"};

    /* Build "L:85 R:72 N:91" (or "--" for unknown) */
    int pos = 0;
    for (int i = 0; i < NUM_DEVICES; i++) {
        if (i > 0) {
            text[pos++] = '\n';
        }
        if (battery_levels[i] == BATT_UNKNOWN) {
            pos += snprintf(text + pos, sizeof(text) - pos, "%s:--", names[i]);
        } else {
            pos += snprintf(text + pos, sizeof(text) - pos, "%s:%u%%", names[i],
                            battery_levels[i]);
        }
    }
    text[pos] = '\0';

    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_label_set_text(widget->obj, text);
    }
}

static int battery_event_handler(const zmk_event_t *eh) {
    const struct zmk_dongle_battery_state_changed *ev = as_zmk_dongle_battery_state_changed(eh);
    if (!ev || ev->device_index >= NUM_DEVICES) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    battery_levels[ev->device_index] = ev->level;
    refresh_widgets();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_dongle_battery_status, battery_event_handler);
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_dongle_battery_state_changed);

int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget,
                                          lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);
    lv_label_set_text(widget->obj, "L:--\nR:--\nN:--");

    sys_slist_append(&widgets, &widget->node);
    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget) {
    return widget->obj;
}
