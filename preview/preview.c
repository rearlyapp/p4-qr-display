/* Renders the actual firmware UI without a board or any network requests. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/libs/lodepng/lodepng.h"

/* Model a slow LVGL frame without running any network or inventory operations. */
static unsigned lock_timeouts;
static unsigned lock_waits;
static bool preview_lock_ui(int32_t timeout_ms)
{
    if (timeout_ms < 0) {
        ++lock_waits;
        return true;
    }
    if (lock_timeouts) {
        --lock_timeouts;
        return false;
    }
    return true;
}

#include "../main/stockin_ui.c"

__asm__(".section .rodata\n"
        ".global _binary_rearly_logo_png_start\n"
        "_binary_rearly_logo_png_start:\n"
        ".incbin \"" LOGO_PATH "\"\n"
        ".global _binary_rearly_logo_png_end\n"
        "_binary_rearly_logo_png_end:\n"
        ".previous\n");

static uint16_t framebuffer[800 * 1280];
static uint16_t draw_buffer[800 * 50];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    uint16_t *source = (uint16_t *)pixels;
    unsigned width = (unsigned)(area->x2 - area->x1 + 1);
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(framebuffer + y * 800 + area->x1, source, width * 2);
        source += width;
    }
    lv_display_flush_ready(display);
}

static void advance(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 16) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static void snapshot(const char *directory, const char *name)
{
    uint8_t *rgb = malloc(800 * 1280 * 3);
    assert(rgb);
    for (size_t i = 0; i < 800 * 1280; ++i) {
        unsigned value = framebuffer[i];
        rgb[i * 3] = ((value >> 11) & 31) * 255 / 31;
        rgb[i * 3 + 1] = ((value >> 5) & 63) * 255 / 63;
        rgb[i * 3 + 2] = (value & 31) * 255 / 31;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    unsigned error = lodepng_encode24(&encoded, &encoded_size, rgb, 800, 1280);
    assert(error == 0);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, encoded_size, file) == encoded_size);
    fclose(file);
    lv_free(encoded);
    free(rgb);
    printf("Rendered %s\n", path);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_display_t *display = lv_display_create(800, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    stockin_ui_start();
    stockin_ui_set_network(true, true,
        "ETH  http://192.168.29.123   SETUP CODE: 12345678",
        "WI-FI  http://192.168.29.156");
    stockin_ui_set_bluetooth(true);
    stockin_ui_set_usb(true, "");
    advance(400);
    snapshot(out, "01-idle");

    stockin_packing_session_view_t session = {
        .active = true, .sheet_ref = "LN-2026-0929 / NILA EXPORTS",
        .started_at = "2026-09-29T14:30:00", .elapsed_seconds = 2820,
        .labels_prepared = 124, .grade_count = 4, .grade_total = 4,
        .grades = {
            { .product = "Vannamei HLSO", .grade = "31/40", .state = "packing",
              .expected_blocks = 240, .labels_prepared = 48, .expected_labels = 60 },
            { .product = "Black Tiger PDTO", .grade = "16/20", .state = "planned",
              .expected_blocks = 200, .labels_prepared = 26, .expected_labels = 50 },
            { .product = "Vannamei PD", .grade = "51/60", .state = "complete",
              .expected_blocks = 200, .labels_prepared = 50, .expected_labels = 50 },
            { .product = "White Shrimp HOSO", .grade = "41/50", .state = "planned",
              .expected_blocks = 100, .labels_prepared = 0, .expected_labels = 25 },
        },
    };
    stockin_ui_set_activity(37, 2, 0, 87);
    stockin_ui_set_packing_session(STOCKIN_PACKING_PLANNED, &session);
    advance(500);
    snapshot(out, "02-planned");
    stockin_ui_set_packing_session(STOCKIN_PACKING_ON, &session);
    advance(1400);
    snapshot(out, "03-running");

    const char *qr = "0123456789ABCDEF01234567";
    stockin_carton_view_t carton = {
        .qr = qr, .order_ref = "LN-2026-0929", .product_name = "Vannamei HLSO",
        .grade = "31/40", .trace_code = "NILA / 29-SEP / A2", .declared_net_kg = "10.0",
        .warehouse = "Stores - V", .actual_blocks = 4, .blocks_per_carton = 4,
    };
    stockin_ui_show_queued(1, qr, 1);
    lock_timeouts = 1;
    stockin_ui_show_processing(1, qr);
    assert(s_processing_id == 1);
    assert(s_scan_busy);
    assert(lock_timeouts == 0 && lock_waits == 1);
    advance(500);
    snapshot(out, "04-scanning");
    /* A queued scan and duplicate must not overwrite the in-flight carton. */
    stockin_ui_show_queued(2, "999999999999999999999999", 1);
    stockin_ui_show_duplicate(qr);
    assert(strcmp(lv_label_get_text(s_scan_qr), qr) == 0);
    stockin_ui_show_carton(1, &carton, "STOCKING IN", UI_AMBER);
    advance(500);
    snapshot(out, "05-transfer");
    lock_timeouts = 1;
    stockin_ui_show_success(1, &carton, "");
    assert(lock_timeouts == 0 && lock_waits == 2);
    assert(strcmp(lv_label_get_text(s_scan_title), "PACKED") == 0);
    assert(strcmp(lv_label_get_text(s_scan_status), "ANTE-ROOM") == 0);
    assert(!s_scan_busy && !lv_timer_get_paused(s_return_timer));
    /* Reproduce a queue notification delivered after the worker finished. */
    stockin_ui_show_queued(1, qr, 1);
    assert(strcmp(lv_label_get_text(s_scan_title), "PACKED") == 0);
    assert(strcmp(lv_label_get_text(s_scan_status), "ANTE-ROOM") == 0);
    assert(!lv_timer_get_paused(s_return_timer));
    assert(strstr(lv_label_get_text(s_scan_session), "Ante-Room"));
    assert(!strstr(lv_label_get_text(s_scan_session), "Stores"));
    assert(strcmp(warehouse_caption("Labels - V"), "Carton") == 0);
    assert(strcmp(warehouse_caption("LABELS-V"), "Carton") == 0);
    assert(strcmp(warehouse_caption("STORES-V"), "Ante-Room") == 0);
    assert(strcmp(warehouse_caption("Other room"), "Other room") == 0);
    stockin_ui_set_activity(38, 2, 0, 86);
    advance(1200);
    snapshot(out, "06-accepted");
    advance(4000);
    assert(lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_base_title), "PACKING ON") == 0);
    snapshot(out, "07-returned");
    stockin_ui_show_processing(2, qr);
    lock_timeouts = 1;
    stockin_ui_show_not_in_labels(2);
    assert(lock_timeouts == 0 && lock_waits == 3);
    assert(strcmp(lv_label_get_text(s_scan_title), "CARTON NOT FOUND") == 0);
    advance(100);
    snapshot(out, "08-rejected");
    advance(5100);
    assert(lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN));
    stockin_ui_show_duplicate(qr);
    advance(400);
    snapshot(out, "09-duplicate");
    advance(2400);
    assert(lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN));
    stockin_ui_show_processing(3, qr);
    lock_timeouts = 1;
    stockin_ui_show_error(3, qr, "CONNECTION TIMEOUT", "");
    assert(lock_timeouts == 0 && lock_waits == 4);
    assert(strcmp(lv_label_get_text(s_scan_status), "RESULT UNKNOWN") == 0);
    advance(5200);

    /* Simulate 100 successive check-ins: slow-frame timeouts in >=50% of
     * transitions, out-of-order notifications, and return-timer interruption.
     * These are UI-only events, with no carton transfers or network calls.
     */
    for (uint32_t id = 4; id < 104; ++id) {
        char fast_qr[25];
        snprintf(fast_qr, sizeof(fast_qr), "%024u", id);
        stockin_carton_view_t fast_carton = carton;
        fast_carton.qr = fast_qr;
        stockin_ui_show_queued(id, fast_qr, 1);
        if (id % 2 == 0) lock_timeouts = 1;
        stockin_ui_show_processing(id, fast_qr);
        assert(lock_timeouts == 0);
        assert(s_processing_id == id && s_scan_busy);
        assert(lv_timer_get_paused(s_return_timer));
        /* A late accepted message and the next queued QR cannot displace
         * the current worker's scan, nor can an old processing notification.
         */
        stockin_ui_show_queued(id, fast_qr, 1);
        stockin_ui_show_queued(id + 1, qr, 1);
        stockin_ui_show_processing(id - 1, qr);
        assert(strcmp(lv_label_get_text(s_scan_qr), fast_qr) == 0);
        if (id % 3 == 0) lock_timeouts = 1;
        stockin_ui_show_carton(id, &fast_carton, "STOCKING IN", UI_AMBER);
        assert(lock_timeouts == 0);
        if (id % 2 != 0) lock_timeouts = 1;
        stockin_ui_show_success(id, &fast_carton, "");
        assert(lock_timeouts == 0);
        stockin_ui_show_queued(id, fast_qr, 1);
        stockin_ui_show_queued(id - 1, qr, 1);
        stockin_ui_show_error(id - 1, qr, "NETWORK OFFLINE", "");
        assert(strcmp(lv_label_get_text(s_scan_title), "PACKED") == 0);
        assert(strcmp(lv_label_get_text(s_scan_status), "ANTE-ROOM") == 0);
        assert(strcmp(lv_label_get_text(s_scan_qr), fast_qr) == 0);
        assert(!s_scan_busy && !lv_timer_get_paused(s_return_timer));
    }
    advance(5200);
    assert(lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_base_title), "PACKING ON") == 0);
    stockin_ui_show_queued(103, qr, 1);
    assert(lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN));
    assert(scan_is_newer(1, UINT32_MAX));
    assert(!scan_is_newer(UINT32_MAX, 1));
    assert(!scan_is_newer(0, UINT32_MAX));

    stockin_ui_set_network(false, false,
        "SETUP: Rearly-Setup-0214   KEY: 12345678", "http://192.168.4.1");
    advance(300);
    snapshot(out, "10-network-setup");
    lv_obj_invalidate(lv_screen_active());
    advance(100);
    snapshot(out, "11-network-full-redraw");
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_y(s_network_lines[1]) + lv_obj_get_height(s_network_lines[1]) <=
           lv_obj_get_content_height(lv_obj_get_parent(s_network_lines[1])));
    puts("PASS: scan ownership, result timers, return state, timeout status; "
         "100 rapid UI check-ins with lock timeouts and stale queue notifications");
    return 0;
}
