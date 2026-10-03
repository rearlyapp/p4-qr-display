#include "packing_visual.h"

#define GREEN 0x39FF88
#define AMBER 0xFBBF24
#define RED 0xFF6076
#define CYAN 0x56DDE0
#define DIM 0x31564D
#define W 664

LV_FONT_DECLARE(ui_bold_20);

struct packing_visual {
    lv_obj_t *root;
    lv_obj_t *belt;
    lv_obj_t *ticks[12];
    lv_obj_t *cartons[3];
    lv_obj_t *gate;
    lv_obj_t *beam;
    lv_obj_t *beacon;
    lv_obj_t *halo;
    lv_obj_t *badge;
    lv_obj_t *check;
    lv_obj_t *cross[2];
    lv_obj_t *pause[2];
    lv_obj_t *sparks[8];
    lv_obj_t *warehouse;
    lv_timer_t *timer;
    packing_visual_mode_t mode;
    uint32_t started;
};

static const lv_point_precise_t box_outline[] = {
    {2, 18}, {37, 2}, {72, 18}, {72, 58}, {37, 74}, {2, 58}, {2, 18}
};
static const lv_point_precise_t box_lid[] = {{2, 18}, {37, 34}, {72, 18}};
static const lv_point_precise_t box_seam[] = {{37, 34}, {37, 74}};
static const lv_point_precise_t box_tape[] = {{19, 10}, {54, 26}, {54, 45}};
static const lv_point_precise_t tick_mark[] = {{15, 34}, {29, 48}, {51, 21}};
static const lv_point_precise_t cross_a[] = {{21, 21}, {47, 47}};
static const lv_point_precise_t cross_b[] = {{21, 47}, {47, 21}};
static const lv_point_precise_t roof[] = {{0, 30}, {48, 0}, {96, 30}};
static const int8_t spark_x[] = {0, 7, 10, 7, 0, -7, -10, -7};
static const int8_t spark_y[] = {-10, -7, 0, 7, 10, 7, 0, -7};

static void hidden(lv_obj_t *obj, bool hide)
{
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h,
                        uint32_t fill, uint32_t edge, int border, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(obj, border, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *line(lv_obj_t *parent, const lv_point_precise_t *points,
                       unsigned count, uint32_t color, int width)
{
    lv_obj_t *obj = lv_line_create(parent);
    lv_line_set_points(obj, points, count);
    lv_obj_set_style_line_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(obj, width, 0);
    lv_obj_set_style_line_rounded(obj, true, 0);
    return obj;
}

static lv_obj_t *carton(lv_obj_t *parent, int x)
{
    lv_obj_t *obj = block(parent, x, 84, 76, 78, 0x06231C, GREEN, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    line(obj, box_outline, 7, GREEN, 3);
    line(obj, box_lid, 3, GREEN, 3);
    line(obj, box_seam, 2, GREEN, 3);
    line(obj, box_tape, 3, 0x91FFC1, 3);
    block(obj, 12, 34, 14, 13, 0xD5FFE7, 0xD5FFE7, 0, 2);
    block(obj, 15, 37, 3, 3, 0x07392A, 0, 0, 0);
    block(obj, 21, 41, 3, 3, 0x07392A, 0, 0, 0);
    return obj;
}

static void caption(lv_obj_t *root, const char *text, int x, int width)
{
    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, 219);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &ui_bold_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xA2C8BD), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void animate_scene(lv_timer_t *timer)
{
    packing_visual_t *v = lv_timer_get_user_data(timer);
    if (!lv_obj_is_visible(v->root)) return;
    uint32_t elapsed = lv_tick_elaps(v->started);
    uint32_t breath = (elapsed % 2400) / 12;
    if (breath > 100) breath = 200 - breath;
    lv_obj_set_style_shadow_opa(v->beacon, 45 + breath, 0);

    bool travel = v->mode == PACKING_VISUAL_RUNNING ||
                  v->mode == PACKING_VISUAL_TRANSFER;
    if (travel) {
        unsigned period = v->mode == PACKING_VISUAL_RUNNING ? 8000 : 3400;
        for (unsigned i = 0; i < 3; ++i) {
            int x = (int)((elapsed % period) * 580 / period + i * 194) % 580;
            lv_obj_set_x(v->cartons[i], x + 8);
            /* Enter and exit without a visible jump back along the belt. */
            unsigned opacity = x < 36 ? x * 255 / 36 :
                               x > 510 ? (580 - x) * 255 / 70 : 255;
            lv_obj_set_style_opa(v->cartons[i], opacity, 0);
        }
        for (unsigned i = 0; i < 12; ++i) {
            lv_obj_set_x(v->ticks[i], 14 + (i * 51 + elapsed / 35) % 612);
        }
    }

    if (v->mode == PACKING_VISUAL_SCAN || v->mode == PACKING_VISUAL_TRANSFER) {
        lv_obj_set_y(v->beam, 63 + (elapsed % 1700) * 92 / 1700);
        lv_arc_set_rotation(v->halo, (elapsed / 5) % 360);
    }
    if (v->mode == PACKING_VISUAL_ACCEPTED) {
        unsigned t = elapsed > 1100 ? 1100 : elapsed;
        lv_obj_set_x(v->cartons[0], 291 + t * 235 / 1100);
        for (unsigned i = 0; i < 8; ++i) {
            unsigned r = 43 + (elapsed < 800 ? elapsed * 30 / 800 : 30);
            lv_obj_set_pos(v->sparks[i], 326 + spark_x[i] * r / 10,
                            66 + spark_y[i] * r / 10);
            lv_obj_set_style_opa(v->sparks[i], elapsed >= 800 ? 0 :
                                 255 - elapsed * 255 / 800, 0);
        }
    }
    if (v->mode == PACKING_VISUAL_REJECTED && elapsed < 500) {
        int shake = (elapsed / 55) % 2 ? 7 : -7;
        lv_obj_set_x(v->cartons[0], 291 + shake * (500 - (int)elapsed) / 500);
    } else if (v->mode == PACKING_VISUAL_REJECTED) {
        lv_obj_set_x(v->cartons[0], 291);
    }
}

static void delete_scene(lv_event_t *event)
{
    packing_visual_t *v = lv_event_get_user_data(event);
    lv_timer_delete(v->timer);
    lv_free(v);
}

packing_visual_t *packing_visual_create(lv_obj_t *parent)
{
    packing_visual_t *v = lv_malloc_zeroed(sizeof(*v));
    LV_ASSERT_MALLOC(v);
    v->root = block(parent, 0, 0, W, 254, 0x031A17, 0x123D31, 1, 20);
    /* Fine graph-paper lines give the schematic depth without a bitmap. */
    for (unsigned x = 34; x < W; x += 60) {
        block(v->root, x, 18, 1, 190, 0x0A2B23, 0, 0, 0);
    }
    for (unsigned y = 34; y < 210; y += 44) {
        block(v->root, 18, y, W - 36, 1, 0x0A2B23, 0, 0, 0);
    }
    block(v->root, 59, 191, 7, 23, DIM, 0, 0, 2);
    block(v->root, 596, 191, 7, 23, DIM, 0, 0, 2);
    v->belt = block(v->root, 12, 165, 640, 36, 0x0A2920, GREEN, 3, 18);
    for (unsigned i = 0; i < 12; ++i) {
        v->ticks[i] = block(v->root, 18 + i * 51, 178, 22, 7, 0x368A63, 0, 0, 4);
    }
    v->warehouse = block(v->root, 538, 43, 96, 117, 0x082820, DIM, 0, 0);
    lv_obj_set_style_bg_opa(v->warehouse, LV_OPA_TRANSP, 0);
    line(v->warehouse, roof, 3, 0x368A63, 4);
    lv_obj_t *shed = block(v->warehouse, 5, 30, 86, 85, 0x082820, 0x368A63, 3, 3);
    block(shed, 14, 18, 52, 64, 0x021510, DIM, 2, 2);
    for (unsigned y = 26; y < 68; y += 14) {
        block(shed, 16, y, 48, 2, DIM, 0, 0, 0);
    }
    for (unsigned i = 0; i < 3; ++i) v->cartons[i] = carton(v->root, 53 + i * 194);

    v->gate = block(v->root, 276, 45, 110, 119, 0x0A3528, 0x3BBF85, 4, 12);
    lv_obj_set_style_bg_opa(v->gate, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(v->gate, LV_BORDER_SIDE_TOP |
        LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
    block(v->gate, 36, 5, 30, 8, CYAN, 0, 0, 4);
    v->beam = block(v->root, 283, 64, 96, 4, CYAN, 0, 0, 2);
    lv_obj_set_style_shadow_color(v->beam, lv_color_hex(CYAN), 0);
    lv_obj_set_style_shadow_width(v->beam, 14, 0);
    lv_obj_set_style_shadow_opa(v->beam, LV_OPA_60, 0);
    v->beacon = block(v->root, 320, 21, 18, 10, GREEN, 0, 0, 5);
    lv_obj_set_style_shadow_color(v->beacon, lv_color_hex(GREEN), 0);
    lv_obj_set_style_shadow_width(v->beacon, 24, 0);

    v->halo = lv_arc_create(v->root);
    lv_obj_set_size(v->halo, 92, 92);
    lv_obj_set_pos(v->halo, 284, 20);
    lv_arc_set_bg_angles(v->halo, 0, 360);
    lv_arc_set_angles(v->halo, 0, 105);
    lv_obj_set_style_arc_width(v->halo, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(v->halo, lv_color_hex(0x134638), LV_PART_MAIN);
    lv_obj_set_style_arc_width(v->halo, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(v->halo, lv_color_hex(CYAN), LV_PART_INDICATOR);
    lv_obj_remove_style(v->halo, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(v->halo, LV_OBJ_FLAG_CLICKABLE);

    v->badge = block(v->root, 296, 32, 68, 68, 0x053322, GREEN, 3, LV_RADIUS_CIRCLE);
    v->check = line(v->badge, tick_mark, 3, GREEN, 6);
    v->cross[0] = line(v->badge, cross_a, 2, RED, 6);
    v->cross[1] = line(v->badge, cross_b, 2, RED, 6);
    v->pause[0] = block(v->badge, 21, 20, 8, 27, AMBER, 0, 0, 2);
    v->pause[1] = block(v->badge, 38, 20, 8, 27, AMBER, 0, 0, 2);
    for (unsigned i = 0; i < 8; ++i) {
        v->sparks[i] = block(v->root, 330, 66, 6, 6, GREEN, 0, 0, 3);
    }
    caption(v->root, "CARTON", 8, 190);
    caption(v->root, "PACKING", 234, 190);
    caption(v->root, "ANTE-ROOM", 466, 190);
    v->timer = lv_timer_create(animate_scene, 66, v);
    lv_obj_add_event_cb(v->root, delete_scene, LV_EVENT_DELETE, v);
    v->mode = (packing_visual_mode_t)-1;
    packing_visual_set_mode(v, PACKING_VISUAL_IDLE);
    return v;
}

lv_obj_t *packing_visual_object(packing_visual_t *v)
{
    return v->root;
}

void packing_visual_set_mode(packing_visual_t *v, packing_visual_mode_t mode)
{
    if (v->mode == mode && mode <= PACKING_VISUAL_RUNNING) return;
    v->mode = mode;
    v->started = lv_tick_get();
    bool result = mode == PACKING_VISUAL_ACCEPTED || mode == PACKING_VISUAL_REJECTED ||
                  mode == PACKING_VISUAL_DUPLICATE;
    bool scanning = mode == PACKING_VISUAL_SCAN || mode == PACKING_VISUAL_TRANSFER;
    uint32_t color = mode == PACKING_VISUAL_IDLE ? DIM :
                     mode == PACKING_VISUAL_REJECTED ? RED :
                     mode == PACKING_VISUAL_PLANNED || mode == PACKING_VISUAL_DUPLICATE ? AMBER :
                     scanning ? CYAN : GREEN;
    lv_obj_set_style_border_color(v->belt, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(v->beacon, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(v->beacon, lv_color_hex(color), 0);
    hidden(v->gate, result);
    hidden(v->beam, !scanning);
    hidden(v->halo, !scanning && !result);
    hidden(v->badge, !result);
    hidden(v->check, mode != PACKING_VISUAL_ACCEPTED);
    hidden(v->cross[0], mode != PACKING_VISUAL_REJECTED);
    hidden(v->cross[1], mode != PACKING_VISUAL_REJECTED);
    hidden(v->pause[0], mode != PACKING_VISUAL_DUPLICATE);
    hidden(v->pause[1], mode != PACKING_VISUAL_DUPLICATE);
    lv_obj_set_style_border_color(v->badge, lv_color_hex(color), 0);
    lv_obj_set_style_arc_color(v->halo, lv_color_hex(color), LV_PART_INDICATOR);
    lv_arc_set_angles(v->halo, 0, result ? 359 : 105);
    lv_arc_set_rotation(v->halo, 0);
    for (unsigned i = 0; i < 3; ++i) {
        hidden(v->cartons[i], mode == PACKING_VISUAL_IDLE || ((result || mode == PACKING_VISUAL_SCAN) && i));
        lv_obj_set_x(v->cartons[i], result || mode == PACKING_VISUAL_SCAN ? 291 : 53 + i * 194);
        lv_obj_set_style_opa(v->cartons[i], mode == PACKING_VISUAL_PLANNED ? 180 : 255, 0);
    }
    for (unsigned i = 0; i < 8; ++i) hidden(v->sparks[i], mode != PACKING_VISUAL_ACCEPTED);
}
