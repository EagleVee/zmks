/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Bongo cat animation widget: draws a drumming cat on an LVGL canvas and
 * animates paw taps driven by the WPM subsystem.
 *
 * Three frames:
 *   0 — idle  (both arms resting)
 *   1 — left  paw down (tapping left drum)
 *   2 — right paw down (tapping right drum)
 *
 * Animation toggles between frames 1 and 2 on every WPM update while the
 * user is typing; reverts to frame 0 after a short inactivity period.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/widgets/bongo_cat.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/wpm.h>

/* ── Canvas dimensions ─────────────────────────────────────────────────── */

#define CAT_W 64
#define CAT_H 32

/*
 * Indexed 1-bit buffer layout (LVGL):
 *   bytes 0-7   : 2-entry colour palette (4 bytes each, ARGB)
 *   bytes 8+    : pixel data, MSB-first, rows padded to byte boundary
 *
 * Row stride = ceil(64/8) = 8 bytes → total pixel data = 8 * 32 = 256 bytes
 * Total buffer size = 8 + 256 = 264 bytes
 */
#define CAT_BUF_SIZE (8 + (CAT_W / 8) * CAT_H)

/* ── Widget state ──────────────────────────────────────────────────────── */

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct bongo_cat_state {
    uint8_t wpm;
};

/* ── Drawing helpers ───────────────────────────────────────────────────── */

/* Palette index helpers for indexed-1bit canvas.
 * In LV_COLOR_DEPTH_1 mode lv_color_t.full is 0 (black) or 1 (white). */
static inline lv_color_t px_on(void)  { lv_color_t c; c.full = 1; return c; }
static inline lv_color_t px_off(void) { lv_color_t c; c.full = 0; return c; }

static void set_px(lv_obj_t *canvas, int x, int y, lv_color_t c) {
    if (x < 0 || x >= CAT_W || y < 0 || y >= CAT_H) return;
    lv_canvas_set_px_color(canvas, x, y, c);
}

static void fill_rect(lv_obj_t *canvas, int x, int y, int w, int h) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            set_px(canvas, x + dx, y + dy, px_on());
}

/* Bresenham line */
static void draw_line(lv_obj_t *canvas, int x0, int y0, int x1, int y1) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        set_px(canvas, x0, y0, px_on());
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Filled circle (integer arithmetic only) */
static void fill_circle(lv_obj_t *canvas, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                set_px(canvas, cx + dx, cy + dy, px_on());
}

/*
 * Draw one bongo-cat frame onto the canvas.
 *
 * Layout (64 wide × 32 tall, origin top-left):
 *
 *   Left  drum : filled circle centred at ( 8, 26), r=4
 *   Right drum : filled circle centred at (55, 26), r=4
 *   Body       : rect (24, 16) 16×8
 *   Head       : rect (26,  6) 12×10
 *   Left  ear  : rect (24,  3)  4×4
 *   Right ear  : rect (36,  3)  4×4
 *   Left  eye  : 2×2 black hole at (28, 9)
 *   Right eye  : 2×2 black hole at (34, 9)
 *   L-whiskers : 2 lines (18,11)→(25,11) and (18,12)→(25,12)
 *   R-whiskers : 2 lines (39,11)→(46,11) and (39,12)→(46,12)
 *
 *   Arms (frame-dependent):
 *     0 idle : L (26,20)→(14,22)  R (38,20)→(50,22)
 *     1 left : L (26,20)→(10,26)  R (38,20)→(50,22)
 *     2 right: L (26,20)→(14,22)  R (38,20)→(52,26)
 */
static void draw_frame(lv_obj_t *canvas, uint8_t frame) {
    /* clear */
    lv_canvas_fill_bg(canvas, px_off(), LV_OPA_COVER);

    /* drums */
    fill_circle(canvas,  8, 26, 4);
    fill_circle(canvas, 55, 26, 4);

    /* body + head */
    fill_rect(canvas, 24, 16, 16, 8);
    fill_rect(canvas, 26,  6, 12, 10);

    /* ears */
    fill_rect(canvas, 24, 3, 4, 4);
    fill_rect(canvas, 36, 3, 4, 4);

    /* eyes (black holes punched into the white head) */
    set_px(canvas, 28, 9, px_off()); set_px(canvas, 29, 9, px_off());
    set_px(canvas, 28,10, px_off()); set_px(canvas, 29,10, px_off());
    set_px(canvas, 34, 9, px_off()); set_px(canvas, 35, 9, px_off());
    set_px(canvas, 34,10, px_off()); set_px(canvas, 35,10, px_off());

    /* whiskers */
    draw_line(canvas, 18, 11, 25, 11);
    draw_line(canvas, 18, 12, 25, 12);
    draw_line(canvas, 39, 11, 46, 11);
    draw_line(canvas, 39, 12, 46, 12);

    /* arms */
    if (frame == 1) {
        draw_line(canvas, 26, 20, 10, 26);  /* left paw down  */
        draw_line(canvas, 38, 20, 50, 22);
    } else if (frame == 2) {
        draw_line(canvas, 26, 20, 14, 22);
        draw_line(canvas, 38, 20, 52, 26);  /* right paw down */
    } else {
        draw_line(canvas, 26, 20, 14, 22);  /* both resting   */
        draw_line(canvas, 38, 20, 50, 22);
    }
}

/* ── Widget update ─────────────────────────────────────────────────────── */

static uint8_t tap_side = 0; /* toggles 0↔1 to alternate left/right */

static void update_cb(struct bongo_cat_state state) {
    uint8_t frame;
    if (state.wpm == 0) {
        frame = 0;
    } else {
        tap_side ^= 1;
        frame = tap_side ? 1 : 2;
    }

    struct zmk_widget_bongo_cat *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        draw_frame(widget->obj, frame);
        lv_obj_invalidate(widget->obj);
    }
}

static struct bongo_cat_state get_state(const zmk_event_t *eh) {
    return (struct bongo_cat_state){.wpm = zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_bongo_cat, struct bongo_cat_state, update_cb, get_state)
ZMK_SUBSCRIPTION(widget_bongo_cat, zmk_wpm_state_changed);

/* ── Init ──────────────────────────────────────────────────────────────── */

int zmk_widget_bongo_cat_init(struct zmk_widget_bongo_cat *widget, lv_obj_t *parent) {
    static uint8_t buf[CAT_BUF_SIZE];

    widget->obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(widget->obj, buf, CAT_W, CAT_H, LV_IMG_CF_INDEXED_1BIT);

    /* Palette: index 0 = black (background), index 1 = white (cat) */
    lv_canvas_set_palette(widget->obj, 0, LV_COLOR_MAKE(0x00, 0x00, 0x00));
    lv_canvas_set_palette(widget->obj, 1, LV_COLOR_MAKE(0xff, 0xff, 0xff));

    draw_frame(widget->obj, 0);

    sys_slist_append(&widgets, &widget->node);
    widget_bongo_cat_init();
    return 0;
}

lv_obj_t *zmk_widget_bongo_cat_obj(struct zmk_widget_bongo_cat *widget) {
    return widget->obj;
}
