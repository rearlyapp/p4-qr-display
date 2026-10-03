#include "stockin_ui.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifndef STOCKIN_UI_PREVIEW
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "lvgl.h"
#include "packing_visual.h"

#ifndef STOCKIN_UI_PREVIEW
static const char *TAG = "stockin_ui";
#endif

#define UI_GREEN       0x39FF88
#define UI_GREEN_DARK  0x00E676
#define UI_AMBER       0xFBBF24
#define UI_RED         0xFF4057
#define UI_WHITE       0xFFFFFF
#define UI_MUTED       0xB8D8C3
#define UI_BG          0x000504
#define UI_PANEL       0x00110F
#define TRANSIENT_MS   5000

static lv_obj_t *s_base_panel;
static lv_obj_t *s_base_title;
static lv_obj_t *s_plan_meta;
static lv_obj_t *s_plan_numbers;
static lv_obj_t *s_grades_title;
static lv_obj_t *s_grade_list;
static lv_obj_t *s_grade_page_label;
static packing_visual_t *s_base_visual;
static packing_visual_t *s_scan_visual;
static lv_obj_t *s_return_bar;
static lv_obj_t *s_pipeline_nodes[3];
static lv_obj_t *s_pipeline_labels[3];
static lv_obj_t *s_pipeline_links[2];

#define GRADES_PER_PAGE 3
typedef struct {
    lv_obj_t *root;
    lv_obj_t *product;
    lv_obj_t *grade;
    lv_obj_t *numbers;
    lv_obj_t *state;
    lv_obj_t *progress;
} grade_card_t;
static grade_card_t s_grade_cards[GRADES_PER_PAGE];
static size_t s_grade_page;

static lv_obj_t *s_scan_panel;
static lv_obj_t *s_scan_title;
static lv_obj_t *s_scan_status;
static lv_obj_t *s_scan_qr;
static lv_obj_t *s_scan_product;
static lv_obj_t *s_scan_meta;
static lv_obj_t *s_scan_session;

static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_eth_dot;
static lv_obj_t *s_network_lines[2];
static lv_obj_t *s_bt_dot;
static lv_obj_t *s_usb_dot;
static lv_timer_t *s_return_timer;
static uint32_t s_processing_id;
static bool s_scan_busy;

static stockin_packing_state_t s_packing_state = STOCKIN_PACKING_NONE;
static stockin_packing_session_view_t s_session;

LV_FONT_DECLARE(ui_bold_20);
LV_FONT_DECLARE(ui_bold_28);
LV_FONT_DECLARE(ui_bold_40);
LV_FONT_DECLARE(ui_bold_48);

extern const uint8_t rearly_logo_png_start[] asm("_binary_rearly_logo_png_start");
extern const uint8_t rearly_logo_png_end[] asm("_binary_rearly_logo_png_end");
static lv_image_dsc_t s_rearly_logo;

static const char *safe_text(const char *text)
{
    return text && text[0] ? text : "-";
}

/* Operator-facing names only; never change API warehouse identifiers. */
static const char *warehouse_caption(const char *warehouse)
{
    char normalized[16] = {0};
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)safe_text(warehouse); *p; ++p) {
        if (*p == ' ' || *p == '-') continue;
        if (used == sizeof(normalized) - 1) return safe_text(warehouse);
        normalized[used++] = (char)tolower(*p);
    }
    if (!strcmp(normalized, "labelsv")) return "Carton";
    if (!strcmp(normalized, "storesv")) return "Ante-Room";
    return safe_text(warehouse);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void style_panel(lv_obj_t *panel, uint32_t accent)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(panel, 8, 0);
    lv_obj_set_style_radius(panel, 22, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_status_dot(lv_obj_t *dot, bool online)
{
    const uint32_t color = online ? UI_GREEN : UI_RED;
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(dot, 14, 0);
}

static lv_obj_t *make_status(lv_obj_t *parent, const char *text)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 50);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_pad_column(item, 9, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(item);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 4, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    style_status_dot(dot, false);
    make_label(item, text, &ui_bold_20, UI_WHITE);
    return dot;
}

static void set_hidden(lv_obj_t *object, bool hidden)
{
    if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void slide_y(void *object, int32_t value)
{
    lv_obj_set_style_translate_y(object, value, 0);
}

static void entrance(lv_obj_t *object, int distance, unsigned delay)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, slide_y);
    lv_anim_set_values(&animation, distance, 0);
    lv_anim_set_duration(&animation, 320);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static void return_progress(void *object, int32_t value)
{
    lv_bar_set_value(object, value, LV_ANIM_OFF);
}

static void set_pipeline(unsigned step, uint32_t accent, bool failed)
{
    static const char *names[] = {"QR READ", "VERIFIED", "PACKED"};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t color = i <= step ? accent : 0x305449;
        lv_obj_set_style_bg_color(s_pipeline_nodes[i], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(s_pipeline_labels[i], lv_color_hex(color), 0);
        lv_label_set_text(s_pipeline_labels[i], failed && i == step ? "STOPPED" : names[i]);
        if (i < 2) lv_obj_set_style_bg_color(s_pipeline_links[i],
                         lv_color_hex(i < step ? accent : 0x173B2D), 0);
    }
}

static void format_elapsed(uint32_t seconds, char *out, size_t out_size)
{
    unsigned hours = seconds / 3600;
    unsigned minutes = (seconds % 3600) / 60;
    if (hours) snprintf(out, out_size, "%uh %02um", hours, minutes);
    else snprintf(out, out_size, "%u min", minutes);
}

static void format_started(const char *iso, char *out, size_t out_size)
{
    if (iso && strlen(iso) >= 16) {
        snprintf(out, out_size, "%.5s  %.5s", iso + 5, iso + 11);
    } else {
        snprintf(out, out_size, "%s", safe_text(iso));
    }
}

static void create_grade_card(grade_card_t *card)
{
    lv_obj_t *row = lv_obj_create(s_grade_list);
    card->root = row;
    lv_obj_set_size(row, lv_pct(100), 112);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x092C22), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x236048), 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    card->product = make_label(row, "", &ui_bold_28, UI_WHITE);
    lv_obj_set_width(card->product, 435);
    lv_label_set_long_mode(card->product, LV_LABEL_LONG_DOT);
    lv_obj_align(card->product, LV_ALIGN_TOP_LEFT, 0, -3);
    card->grade = make_label(row, "", &ui_bold_20, UI_GREEN);
    lv_obj_set_width(card->grade, 163);
    lv_obj_set_style_text_align(card->grade, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(card->grade, lv_color_hex(0x123E2B), 0);
    lv_obj_set_style_bg_opa(card->grade, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(card->grade, 3, 0);
    lv_obj_set_style_radius(card->grade, 8, 0);
    lv_label_set_long_mode(card->grade, LV_LABEL_LONG_DOT);
    lv_obj_align(card->grade, LV_ALIGN_TOP_RIGHT, 0, 0);
    card->numbers = make_label(row, "", &ui_bold_20, UI_MUTED);
    lv_obj_align(card->numbers, LV_ALIGN_TOP_LEFT, 0, 39);
    card->state = make_label(row, "", &ui_bold_20, UI_GREEN);
    lv_obj_set_width(card->state, 160);
    lv_label_set_long_mode(card->state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(card->state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(card->state, LV_ALIGN_TOP_RIGHT, 0, 39);
    card->progress = lv_bar_create(row);
    lv_obj_set_size(card->progress, lv_pct(100), 5);
    lv_obj_align(card->progress, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(card->progress, lv_color_hex(0x153F2F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->progress, lv_color_hex(UI_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(card->progress, 500, 0);
}

static void render_grades(bool animate)
{
    size_t pages = (s_session.grade_count + GRADES_PER_PAGE - 1) / GRADES_PER_PAGE;
    if (!pages) pages = 1;
    s_grade_page %= pages;
    lv_label_set_text_fmt(s_grade_page_label, "%u / %u",
                          (unsigned)s_grade_page + 1, (unsigned)pages);
    for (size_t i = 0; i < GRADES_PER_PAGE; ++i) {
        size_t index = s_grade_page * GRADES_PER_PAGE + i;
        grade_card_t *card = &s_grade_cards[i];
        set_hidden(card->root, index >= s_session.grade_count);
        if (index >= s_session.grade_count) continue;
        const stockin_grade_view_t *grade = &s_session.grades[index];
        lv_label_set_text(card->product, safe_text(grade->product));
        lv_label_set_text_fmt(card->grade, "GRADE %s", safe_text(grade->grade));
        lv_label_set_text_fmt(card->numbers, "%d BLOCKS   /   %d OF %d LABELS",
                             grade->expected_blocks, grade->labels_prepared,
                             grade->expected_labels);
        lv_label_set_text(card->state, safe_text(grade->state));
        int percent = grade->expected_labels > 0 ?
                      (int)(100LL * grade->labels_prepared / grade->expected_labels) : 0;
        lv_bar_set_value(card->progress, percent, animate ? LV_ANIM_ON : LV_ANIM_OFF);
        if (animate) entrance(card->root, 14, i * 60);
    }
}

static void rotate_grades(lv_timer_t *timer)
{
    (void)timer;
    if (!lv_obj_is_visible(s_grade_list) || s_session.grade_count <= GRADES_PER_PAGE) return;
    ++s_grade_page;
    render_grades(true);
}

static void render_base(void)
{
    bool returning = !lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN);
    set_hidden(s_scan_panel, true);
    set_hidden(s_base_panel, false);
    if (returning) entrance(s_base_panel, 18, 0);
    s_scan_busy = false;
    lv_anim_delete(s_return_bar, return_progress);
    set_hidden(s_return_bar, true);

    const bool active = s_session.active && s_packing_state != STOCKIN_PACKING_NONE;
    set_hidden(s_plan_meta, !active);
    set_hidden(s_plan_numbers, !active);
    set_hidden(s_grades_title, !active);
    set_hidden(s_grade_list, !active);
    set_hidden(s_grade_page_label, !active);
    packing_visual_set_mode(s_base_visual, !active ? PACKING_VISUAL_IDLE :
        s_packing_state == STOCKIN_PACKING_ON ? PACKING_VISUAL_RUNNING : PACKING_VISUAL_PLANNED);

    if (!active) {
        lv_label_set_text(s_base_title, "NO PACKING\nPLANNED");
        lv_obj_set_style_text_color(s_base_title, lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_text_align(s_base_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_base_title, lv_pct(100));
        lv_obj_align(s_base_title, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_set_y(packing_visual_object(s_base_visual), 350);
        lv_obj_set_style_border_color(s_base_panel, lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_shadow_color(s_base_panel, lv_color_hex(UI_GREEN_DARK), 0);
        return;
    }

    const uint32_t accent = s_packing_state == STOCKIN_PACKING_ON ?
                            UI_GREEN : UI_AMBER;
    lv_label_set_text(s_base_title, s_packing_state == STOCKIN_PACKING_ON ?
                      "PACKING ON" : "PACKING PLANNED");
    lv_obj_set_style_text_color(s_base_title, lv_color_hex(accent), 0);
    lv_obj_set_style_text_align(s_base_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(s_base_title, lv_pct(100));
    lv_obj_align(s_base_title, LV_ALIGN_TOP_LEFT, 0, -3);
    lv_obj_set_y(packing_visual_object(s_base_visual), 111);
    lv_obj_set_style_border_color(s_base_panel, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_color(s_base_panel, lv_color_hex(accent), 0);

    lv_label_set_text_fmt(s_plan_meta, "LOAD NOTE   %s",
                          safe_text(s_session.sheet_ref));
    char elapsed[32];
    char started[32];
    format_elapsed(s_session.elapsed_seconds, elapsed, sizeof(elapsed));
    format_started(s_session.started_at, started, sizeof(started));
    lv_label_set_text_fmt(s_plan_numbers,
        "STARTED  %s\nELAPSED  %s       LABELS PREPARED  %d",
        started, elapsed, s_session.labels_prepared);

    render_grades(returning);
}

static void return_to_base(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    render_base();
}

static void arm_return(unsigned delay_ms)
{
    s_scan_busy = false;
    set_hidden(s_return_bar, false);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_return_bar);
    lv_anim_set_exec_cb(&animation, return_progress);
    lv_anim_set_values(&animation, 1000, 0);
    lv_anim_set_duration(&animation, delay_ms);
    lv_anim_start(&animation);
    lv_timer_set_period(s_return_timer, delay_ms);
    lv_timer_reset(s_return_timer);
    lv_timer_resume(s_return_timer);
}

static void show_scan_screen(uint32_t accent, const char *title,
                             const char *status, const char *qr)
{
    if (s_return_timer) lv_timer_pause(s_return_timer);
    lv_anim_delete(s_return_bar, return_progress);
    set_hidden(s_return_bar, true);
    bool entering = lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN);
    set_hidden(s_base_panel, true);
    set_hidden(s_scan_panel, false);
    if (entering) entrance(s_scan_panel, 24, 0);
    lv_obj_set_style_border_color(s_scan_panel, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_color(s_scan_panel, lv_color_hex(accent), 0);
    lv_label_set_text(s_scan_title, safe_text(title));
    lv_obj_set_style_text_color(s_scan_title, lv_color_hex(accent), 0);
    lv_label_set_text(s_scan_status, safe_text(status));
    lv_obj_set_style_text_color(s_scan_status, lv_color_hex(accent), 0);
    lv_label_set_text(s_scan_qr, safe_text(qr));
    lv_label_set_text(s_scan_product, "");
    lv_label_set_text(s_scan_meta, "");
    lv_label_set_text(s_scan_session, "");
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x001817), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, lv_pct(100), 132);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x001B19), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(UI_GREEN), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 8, 0);
    lv_obj_set_style_shadow_color(header, lv_color_hex(UI_GREEN_DARK), 0);
    lv_obj_set_style_shadow_opa(header, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(header, 18, 0);
    lv_obj_set_style_pad_hor(header, 42, 0);
    lv_obj_set_style_pad_ver(header, 12, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = make_label(header, "Nila Sea Foods", &ui_bold_48, UI_WHITE);
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_t *subtitle = make_label(header, "PACKING ROOM", &ui_bold_28, UI_GREEN);
    lv_obj_set_style_text_letter_space(subtitle, 2, 0);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_LEFT, 2, 2);

    /* A small marine wave mark balances the brand without image assets. */
    static const lv_point_precise_t wave[] = {
        {0, 12}, {14, 3}, {28, 12}, {42, 21}, {56, 12}, {70, 3}
    };
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *mark = lv_line_create(header);
        lv_line_set_points(mark, wave, 6);
        lv_obj_set_style_line_width(mark, 4, 0);
        lv_obj_set_style_line_rounded(mark, true, 0);
        lv_obj_set_style_line_color(mark, lv_color_hex(i == 1 ? 0x56DDE0 : UI_GREEN), 0);
        lv_obj_align(mark, LV_ALIGN_TOP_RIGHT, 0, 8 + i * 21);
    }

    s_base_panel = lv_obj_create(screen);
    lv_obj_set_size(s_base_panel, lv_pct(91), 944);
    lv_obj_align(s_base_panel, LV_ALIGN_TOP_MID, 0, 154);
    style_panel(s_base_panel, UI_MUTED);

    s_base_title = make_label(s_base_panel, "NO PACKING\nPLANNED",
                              &ui_bold_48, UI_MUTED);

    s_plan_meta = make_label(s_base_panel, "", &ui_bold_20, UI_MUTED);
    lv_obj_set_width(s_plan_meta, lv_pct(100));
    lv_label_set_long_mode(s_plan_meta, LV_LABEL_LONG_DOT);
    lv_obj_align(s_plan_meta, LV_ALIGN_TOP_LEFT, 0, 66);

    s_base_visual = packing_visual_create(s_base_panel);
    lv_obj_align(packing_visual_object(s_base_visual), LV_ALIGN_TOP_MID, 0, 111);

    s_plan_numbers = make_label(s_base_panel, "", &ui_bold_20, UI_MUTED);
    lv_obj_set_width(s_plan_numbers, lv_pct(100));
    lv_obj_align(s_plan_numbers, LV_ALIGN_TOP_LEFT, 0, 380);

    s_grades_title = make_label(s_base_panel, "VARIETIES / GRADES",
                                &ui_bold_20, UI_GREEN);
    lv_obj_set_style_text_letter_space(s_grades_title, 2, 0);
    lv_obj_align(s_grades_title, LV_ALIGN_TOP_LEFT, 0, 438);
    s_grade_page_label = make_label(s_base_panel, "1 / 1", &ui_bold_20, UI_MUTED);
    lv_obj_align(s_grade_page_label, LV_ALIGN_TOP_RIGHT, 0, 438);

    s_grade_list = lv_obj_create(s_base_panel);
    lv_obj_set_size(s_grade_list, lv_pct(100), 352);
    lv_obj_align(s_grade_list, LV_ALIGN_TOP_MID, 0, 476);
    lv_obj_set_flex_flow(s_grade_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_grade_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_grade_list, 0, 0);
    lv_obj_set_style_pad_row(s_grade_list, 8, 0);
    lv_obj_set_style_bg_opa(s_grade_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grade_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_grade_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_grade_list, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < GRADES_PER_PAGE; ++i) create_grade_card(&s_grade_cards[i]);
    lv_timer_create(rotate_grades, 7000, NULL);

    s_scan_panel = lv_obj_create(screen);
    lv_obj_set_size(s_scan_panel, lv_pct(91), 944);
    lv_obj_align(s_scan_panel, LV_ALIGN_TOP_MID, 0, 154);
    style_panel(s_scan_panel, UI_GREEN);

    s_scan_title = make_label(s_scan_panel, "QR RECEIVED", &ui_bold_40, UI_GREEN);
    lv_obj_set_width(s_scan_title, lv_pct(100));
    lv_obj_set_style_text_align(s_scan_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_scan_title, LV_ALIGN_TOP_MID, 0, 0);

    s_scan_status = make_label(s_scan_panel, "QUEUED", &ui_bold_28, UI_GREEN);
    lv_obj_set_width(s_scan_status, lv_pct(100));
    lv_obj_set_style_text_align(s_scan_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_scan_status, LV_ALIGN_TOP_MID, 0, 62);

    s_scan_visual = packing_visual_create(s_scan_panel);
    lv_obj_align(packing_visual_object(s_scan_visual), LV_ALIGN_TOP_MID, 0, 127);

    for (unsigned i = 0; i < 3; ++i) {
        if (i < 2) {
            s_pipeline_links[i] = lv_obj_create(s_scan_panel);
            lv_obj_remove_style_all(s_pipeline_links[i]);
            lv_obj_set_size(s_pipeline_links[i], 154, 3);
            lv_obj_set_pos(s_pipeline_links[i], 132 + i * 216, 418);
            lv_obj_set_style_bg_opa(s_pipeline_links[i], LV_OPA_COVER, 0);
        }
        s_pipeline_nodes[i] = lv_obj_create(s_scan_panel);
        lv_obj_remove_style_all(s_pipeline_nodes[i]);
        lv_obj_set_size(s_pipeline_nodes[i], 14, 14);
        lv_obj_set_pos(s_pipeline_nodes[i], 109 + i * 216, 413);
        lv_obj_set_style_bg_opa(s_pipeline_nodes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_pipeline_nodes[i], LV_RADIUS_CIRCLE, 0);
        s_pipeline_labels[i] = make_label(s_scan_panel, "", &ui_bold_20, UI_MUTED);
        lv_obj_set_pos(s_pipeline_labels[i], 33 + i * 216, 441);
        lv_obj_set_width(s_pipeline_labels[i], 165);
        lv_obj_set_style_text_align(s_pipeline_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    set_pipeline(0, UI_GREEN, false);

    s_scan_qr = make_label(s_scan_panel, "", &ui_bold_20, UI_MUTED);
    lv_obj_set_width(s_scan_qr, lv_pct(100));
    lv_obj_set_style_text_align(s_scan_qr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_scan_qr, lv_color_hex(0x000806), 0);
    lv_obj_set_style_bg_opa(s_scan_qr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_scan_qr, lv_color_hex(UI_GREEN_DARK), 0);
    lv_obj_set_style_border_width(s_scan_qr, 1, 0);
    lv_obj_set_style_radius(s_scan_qr, 12, 0);
    lv_obj_set_style_pad_ver(s_scan_qr, 10, 0);
    lv_obj_align(s_scan_qr, LV_ALIGN_TOP_MID, 0, 493);

    s_scan_product = make_label(s_scan_panel, "", &ui_bold_40, UI_WHITE);
    lv_obj_set_width(s_scan_product, lv_pct(100));
    lv_obj_set_height(s_scan_product, 115);
    lv_label_set_long_mode(s_scan_product, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_scan_product, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_scan_product, LV_ALIGN_TOP_MID, 0, 557);

    s_scan_meta = make_label(s_scan_panel, "", &ui_bold_28, UI_WHITE);
    lv_obj_set_width(s_scan_meta, lv_pct(100));
    lv_obj_set_style_text_align(s_scan_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_scan_meta, LV_ALIGN_TOP_MID, 0, 683);

    s_scan_session = make_label(s_scan_panel, "", &ui_bold_20, UI_MUTED);
    lv_obj_set_width(s_scan_session, lv_pct(100));
    lv_obj_set_style_text_align(s_scan_session, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(s_scan_session, 80);
    lv_label_set_long_mode(s_scan_session, LV_LABEL_LONG_DOT);
    lv_obj_align(s_scan_session, LV_ALIGN_TOP_MID, 0, 774);

    s_return_bar = lv_bar_create(s_scan_panel);
    lv_obj_set_size(s_return_bar, lv_pct(100), 6);
    lv_obj_align(s_return_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_return_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(0x123527), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(UI_GREEN), LV_PART_INDICATOR);

    lv_obj_t *footer = lv_obj_create(screen);
    lv_obj_set_size(footer, lv_pct(100), 160);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x001512), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(UI_GREEN), 0);
    lv_obj_set_style_border_width(footer, 8, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_hor(footer, 24, 0);
    lv_obj_set_style_pad_ver(footer, 8, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *statuses = lv_obj_create(footer);
    lv_obj_set_size(statuses, LV_SIZE_CONTENT, 55);
    lv_obj_align(statuses, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(statuses, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(statuses, 0, 0);
    lv_obj_set_style_pad_column(statuses, 18, 0);
    lv_obj_set_style_bg_opa(statuses, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(statuses, 0, 0);
    lv_obj_clear_flag(statuses, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_dot = make_status(statuses, "WI-FI");
    s_eth_dot = make_status(statuses, "ETH");
    s_bt_dot = make_status(statuses, "BLE");
    s_usb_dot = make_status(statuses, "QR");

    s_rearly_logo = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RAW_ALPHA,
            .w = 180,
            .h = 48,
            .stride = 0,
        },
        .data_size = (uint32_t)(rearly_logo_png_end - rearly_logo_png_start),
        .data = rearly_logo_png_start,
    };
    lv_obj_t *logo = lv_image_create(footer);
    lv_image_set_src(logo, &s_rearly_logo);
    lv_obj_align(logo, LV_ALIGN_TOP_RIGHT, 0, 0);

    for (unsigned i = 0; i < 2; ++i) {
        s_network_lines[i] = make_label(footer, i ? "" : "NETWORK STARTING", &ui_bold_20, UI_MUTED);
        lv_obj_set_width(s_network_lines[i], lv_pct(100));
        lv_label_set_long_mode(s_network_lines[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_network_lines[i], LV_ALIGN_TOP_LEFT, 0, 60 + i * 32);
    }

    s_return_timer = lv_timer_create(return_to_base, TRANSIENT_MS, NULL);
    lv_timer_pause(s_return_timer);
    render_base();
    lv_screen_load(screen);
}

static bool lock_ui_with_timeout(int32_t timeout_ms)
{
#ifdef STOCKIN_UI_PREVIEW
    return preview_lock_ui(timeout_ms);
#else
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
#endif
}

static bool lock_ui(void)
{
    return lock_ui_with_timeout(250);
}

/* Worker transitions must not disappear when an animated frame takes >250 ms.
 * Only the HTTP worker waits here; scanner queue notifications stay best-effort.
 */
static bool lock_scan_ui(uint32_t scan_id, const char *stage)
{
    if (lock_ui()) return true;
#ifndef STOCKIN_UI_PREVIEW
    ESP_LOGW(TAG, "QR id=%lu UI busy at %s; waiting for display lock",
             (unsigned long)scan_id, stage);
#else
    (void)scan_id;
    (void)stage;
#endif
    bool locked = lock_ui_with_timeout(-1);
#ifndef STOCKIN_UI_PREVIEW
    if (!locked) ESP_LOGE(TAG, "QR id=%lu could not commit UI stage %s",
                         (unsigned long)scan_id, stage);
#endif
    return locked;
}

static bool scan_is_newer(uint32_t scan_id, uint32_t previous_id)
{
    return scan_id && (!previous_id || (int32_t)(scan_id - previous_id) > 0);
}

static void unlock_ui(void)
{
#ifndef STOCKIN_UI_PREVIEW
    esp_lv_adapter_report_activity();
    esp_lv_adapter_unlock();
#endif
}

esp_err_t stockin_ui_start(void)
{
#ifdef STOCKIN_UI_PREVIEW
    create_ui();
    return ESP_OK;
#else
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_stack_size = 12 * 1024;
    adapter.task_core_id = 1;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter), TAG, "LVGL init failed");

    bsp_lcd_handles_t handles = {0};
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(NULL, &handles), TAG,
                        "10.1-inch DSI panel init failed");

    esp_lv_adapter_display_config_t display = {
        .panel = handles.panel,
        .panel_io = handles.io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 50,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_display(&display), ESP_FAIL,
                        TAG, "display registration failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "LVGL task start failed");

    esp_err_t backlight_err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && backlight_err != ESP_OK; ++attempt) {
        backlight_err = bsp_display_brightness_set(100);
        if (backlight_err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (backlight_err != ESP_OK) {
        ESP_LOGW(TAG, "Backlight controller unavailable (%s)",
                 esp_err_to_name(backlight_err));
    } else {
        ESP_LOGI(TAG, "Backlight set to 100%%");
    }

    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), TAG, "LVGL lock failed");
    create_ui();
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "Display ready: Waveshare 10.1-inch A, %dx%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
#endif
}

void stockin_ui_set_wifi(bool connected, const char *detail)
{
    (void)detail;
    if (!lock_ui()) return;
    style_status_dot(s_wifi_dot, connected);
    unlock_ui();
}

void stockin_ui_set_bluetooth(bool ready)
{
    if (!lock_ui()) return;
    style_status_dot(s_bt_dot, ready);
    unlock_ui();
}

bool stockin_ui_set_network(bool wifi, bool ethernet, const char *line1, const char *line2)
{
    if (!lock_ui()) return false;
    style_status_dot(s_wifi_dot, wifi);
    style_status_dot(s_eth_dot, ethernet);
    lv_label_set_text(s_network_lines[0], line1 ? line1 : "");
    lv_label_set_text(s_network_lines[1], line2 ? line2 : "");
    unlock_ui();
    return true;
}

void stockin_ui_set_usb(bool connected, const char *detail)
{
    (void)detail;
    if (!lock_ui()) return;
    style_status_dot(s_usb_dot, connected);
    unlock_ui();
}

void stockin_ui_show_ready(void)
{
    if (!lock_ui()) return;
    render_base();
    unlock_ui();
}

void stockin_ui_set_packing_session(stockin_packing_state_t state,
                                    const stockin_packing_session_view_t *session)
{
    if (!lock_scan_ui(0, "packing status")) return;
    bool new_plan = session && strcmp(s_session.sheet_ref, session->sheet_ref) != 0;
    if (new_plan) s_grade_page = 0;
    s_packing_state = state;
    if (session) s_session = *session;
    else memset(&s_session, 0, sizeof(s_session));
    if (lv_obj_has_flag(s_scan_panel, LV_OBJ_FLAG_HIDDEN)) render_base();
    unlock_ui();
}

void stockin_ui_show_queued(uint32_t scan_id, const char *qr, unsigned pending)
{
    /* The worker may already have processed this QR by the time USB publishes
     * its queue notification. Never regress a processing/completed scan.
     * Avoid blocking the USB reader just to display an intermediate state.
     */
    if (!pending || !lock_ui_with_timeout(0)) return;
    if (s_scan_busy || !scan_is_newer(scan_id, s_processing_id)) {
        unlock_ui();
        return;
    }
    char status[48];
    snprintf(status, sizeof(status), "QUEUED %u", pending);
    show_scan_screen(UI_GREEN, "QR RECEIVED", status, qr);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_SCAN);
    set_pipeline(0, 0x56DDE0, false);
    unlock_ui();
}

void stockin_ui_show_processing(uint32_t scan_id, const char *qr)
{
    if (!lock_scan_ui(scan_id, "processing")) return;
    if (!scan_is_newer(scan_id, s_processing_id)) { unlock_ui(); return; }
    s_processing_id = scan_id;
    s_scan_busy = true;
    show_scan_screen(0x56DDE0, "CHECKING CARTON", "CARTON", qr);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_SCAN);
    set_pipeline(0, 0x56DDE0, false);
    unlock_ui();
}

/* The caller owns the LVGL lock and has checked the scan ID. */
static void render_carton(const stockin_carton_view_t *carton,
                           const char *status, uint32_t accent)
{
    lv_label_set_text(s_scan_status, safe_text(status));
    lv_obj_set_style_text_color(s_scan_status, lv_color_hex(accent), 0);
    lv_label_set_text(s_scan_qr, safe_text(carton->qr));
    lv_label_set_text(s_scan_product, safe_text(carton->product_name));
    lv_label_set_text_fmt(s_scan_meta,
        "GRADE  %s\n%d / %d BLOCKS     %s kg",
        safe_text(carton->grade), carton->actual_blocks,
        carton->blocks_per_carton, safe_text(carton->declared_net_kg));
    lv_label_set_text_fmt(s_scan_session,
        "LOAD NOTE  %s\nTRACE  %s\n%s",
        safe_text(carton->order_ref), safe_text(carton->trace_code),
        warehouse_caption(carton->warehouse));
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_TRANSFER);
    set_pipeline(1, UI_GREEN, false);
}

void stockin_ui_show_carton(uint32_t scan_id, const stockin_carton_view_t *carton,
                            const char *status, uint32_t accent)
{
    if (!carton || !lock_scan_ui(scan_id, "carton details")) return;
    if (scan_id != s_processing_id) { unlock_ui(); return; }
    render_carton(carton, status, accent);
    unlock_ui();
}

void stockin_ui_show_success(uint32_t scan_id, const stockin_carton_view_t *carton,
                             const char *message)
{
    (void)message;
    if (!carton || !lock_scan_ui(scan_id, "packed result")) return;
    if (scan_id != s_processing_id) { unlock_ui(); return; }
    /* Commit the complete terminal screen with one lock, including its timer.
     * A partial success update must not leave QUEUED/STOCKING IN on screen.
     */
    show_scan_screen(UI_GREEN, "PACKED", "ANTE-ROOM", carton->qr);
    render_carton(carton, "ANTE-ROOM", UI_GREEN);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_ACCEPTED);
    set_pipeline(2, UI_GREEN, false);
    entrance(s_scan_title, 12, 0);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(UI_GREEN), LV_PART_INDICATOR);
    arm_return(TRANSIENT_MS);
    unlock_ui();
#ifndef STOCKIN_UI_PREVIEW
    ESP_LOGI(TAG, "QR id=%lu UI result=PACKED", (unsigned long)scan_id);
#endif
}

void stockin_ui_show_not_in_labels(uint32_t scan_id)
{
    if (!lock_scan_ui(scan_id, "carton not found")) return;
    if (scan_id != s_processing_id) { unlock_ui(); return; }
    lv_label_set_text(s_scan_title, "CARTON NOT FOUND");
    lv_label_set_text(s_scan_status, "NOT MOVED");
    lv_obj_set_style_text_color(s_scan_title, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_text_color(s_scan_status, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_border_color(s_scan_panel, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_shadow_color(s_scan_panel, lv_color_hex(UI_RED), 0);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_REJECTED);
    set_pipeline(1, UI_RED, true);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(UI_RED), LV_PART_INDICATOR);
    arm_return(TRANSIENT_MS);
    unlock_ui();
}

void stockin_ui_show_duplicate(const char *qr)
{
    if (!lock_ui()) return;
    if (s_scan_busy) { unlock_ui(); return; }
    show_scan_screen(UI_AMBER, "DUPLICATE QR", "IGNORED", qr);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_DUPLICATE);
    set_pipeline(0, UI_AMBER, true);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(UI_AMBER), LV_PART_INDICATOR);
    arm_return(2500);
    unlock_ui();
}

void stockin_ui_show_error(uint32_t scan_id, const char *qr,
                           const char *title, const char *message)
{
    (void)message;
    if (scan_id ? !lock_scan_ui(scan_id, "failed result") : !lock_ui()) return;
    if (scan_id && scan_id != s_processing_id) { unlock_ui(); return; }
    if (s_scan_busy && scan_id != s_processing_id) { unlock_ui(); return; }
    show_scan_screen(UI_RED, title && title[0] ? title : "SCAN FAILED",
                     title && strcmp(title, "CONNECTION TIMEOUT") == 0 ?
                     "RESULT UNKNOWN" : "NOT MOVED", qr);
    packing_visual_set_mode(s_scan_visual, PACKING_VISUAL_REJECTED);
    set_pipeline(1, UI_RED, true);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(UI_RED), LV_PART_INDICATOR);
    arm_return(TRANSIENT_MS);
    unlock_ui();
}

void stockin_ui_set_activity(unsigned packed, unsigned rejected,
                             unsigned queued, unsigned available)
{
    /* Counters remain in the worker, but are deliberately absent from the HMI. */
    (void)packed;
    (void)rejected;
    (void)queued;
    (void)available;
}
