/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/widgets/dongle_link_status.h>
#include <zmk/events/dongle_link_state_changed.h>
#include <zmk/event_manager.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct dongle_link_status_state {
    bool connected;
};

static struct dongle_link_status_state get_state(const zmk_event_t *eh) {
    const struct zmk_dongle_link_state_changed *ev = as_zmk_dongle_link_state_changed(eh);
    return (struct dongle_link_status_state){
        .connected = ev ? ev->connected : zmk_dongle_link_is_connected(),
    };
}

static void set_status_symbol(lv_obj_t *label, struct dongle_link_status_state state) {
    lv_label_set_text(label, state.connected ? LV_SYMBOL_WIFI " " LV_SYMBOL_OK
                                             : LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE);
}

static void dongle_link_status_update_cb(struct dongle_link_status_state state) {
    struct zmk_widget_dongle_link_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_status_symbol(widget->obj, state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_link_status, struct dongle_link_status_state,
                            dongle_link_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_dongle_link_status, zmk_dongle_link_state_changed);

int zmk_widget_dongle_link_status_init(struct zmk_widget_dongle_link_status *widget,
                                       lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);

    sys_slist_append(&widgets, &widget->node);

    widget_dongle_link_status_init();
    return 0;
}

lv_obj_t *zmk_widget_dongle_link_status_obj(struct zmk_widget_dongle_link_status *widget) {
    return widget->obj;
}
