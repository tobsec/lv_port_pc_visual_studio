#include "golden_screen.h"
#include "map_renderer.h"
#include <stdio.h>

/* ── Layout constants ── */
#define DISP_SIZE       800
#define RPM_SCALE_SIZE  780
#define RPM_MAX         6000
#define RPM_YELLOW      4800
#define RPM_REDLINE     5200
#define RPM_NEEDLE_LEN  (-30)  /* negative = radius minus this value, stops near tick tips */

/* Color palette */
#define COL_BG          lv_color_hex(0x0d1117)
#define COL_PANEL       lv_color_hex(0x161b22)
#define COL_NORMAL      lv_color_hex(0xc9d1d9)   /* neutral bright — same feel as text */
#define COL_GREEN       lv_color_hex(0x00c853)   /* kept for warning-clear transitions */
#define COL_YELLOW      lv_color_hex(0xffd600)
#define COL_RED         lv_color_hex(0xff1744)
#define COL_ARC_TRACK   lv_color_hex(0x21262d)
#define COL_BAR_TRACK   lv_color_hex(0x21262d)
#define COL_TEXT        lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM    lv_color_hex(0x8b949e)
#define COL_CHART_BG    lv_color_hex(0x0a1628)

/* Tile base path — set before golden_screen_create() */
static const char* tile_path = NULL;
static const char* alt_tile_path = NULL;
static map_renderer_t* gs_map = NULL;

/* ── Widget handles for update ── */
static lv_obj_t* rpm_scale;
static lv_obj_t* rpm_needle;
static lv_obj_t* sog_label;
static lv_obj_t* depth_label;
static lv_obj_t* watertemp_label;
/* Pod widgets: each pod has a status dot, value label, and bar */
typedef struct {
    lv_obj_t* pod;    /* container */
    lv_obj_t* dot;    /* status indicator */
    lv_obj_t* val;    /* value label */
    lv_obj_t* bar;    /* thin range bar */
} gauge_pod_t;

static gauge_pod_t pod_oilt, pod_oilp, pod_clt;
static gauge_pod_t pod_lam, pod_cltp, pod_batt;

/* Bottom readouts (remaining params without pods) */
static lv_obj_t* lbl_lambda1;
static lv_obj_t* lbl_lambda2;
static lv_obj_t* lbl_iat;
static lv_obj_t* lbl_map;
static lv_obj_t* lbl_fp;
static lv_obj_t* lbl_fuel;

/* ── Helpers ── */

/* RPM scale section styles — static so they persist */
static lv_style_t style_section_green_main;
static lv_style_t style_section_green_indicator;
static lv_style_t style_section_green_items;
static lv_style_t style_section_yellow_main;
static lv_style_t style_section_yellow_indicator;
static lv_style_t style_section_yellow_items;
static lv_style_t style_section_red_main;
static lv_style_t style_section_red_indicator;
static lv_style_t style_section_red_items;

static void init_section_styles(void)
{
    /* Normal section: 0-4800 RPM — neutral/bright */
    lv_style_init(&style_section_green_main);
    lv_style_set_arc_color(&style_section_green_main, COL_NORMAL);

    lv_style_init(&style_section_green_indicator);
    lv_style_set_line_color(&style_section_green_indicator, COL_NORMAL);

    lv_style_init(&style_section_green_items);
    lv_style_set_line_color(&style_section_green_items, COL_NORMAL);

    /* Yellow section: 4800-5200 RPM */
    lv_style_init(&style_section_yellow_main);
    lv_style_set_arc_color(&style_section_yellow_main, COL_YELLOW);

    lv_style_init(&style_section_yellow_indicator);
    lv_style_set_line_color(&style_section_yellow_indicator, COL_YELLOW);

    lv_style_init(&style_section_yellow_items);
    lv_style_set_line_color(&style_section_yellow_items, COL_YELLOW);

    /* Red section: 5200-6000 RPM */
    lv_style_init(&style_section_red_main);
    lv_style_set_arc_color(&style_section_red_main, COL_RED);

    lv_style_init(&style_section_red_indicator);
    lv_style_set_line_color(&style_section_red_indicator, COL_RED);

    lv_style_init(&style_section_red_items);
    lv_style_set_line_color(&style_section_red_items, COL_RED);
}

/* Pod colors */
#define COL_POD_BG      lv_color_hex(0x1c1c1e)
#define COL_POD_BORDER  lv_color_hex(0x3a3a3c)
#define POD_OPACITY     LV_OPA_COVER   /* change to LV_OPA_70 for glass effect */

/**
 * Create a gauge pod: opaque container with large value, label, thin bar, status dot.
 *
 *  ┌──────────────────┐
 *  │  ●    92°C       │  ← dot + large value centered
 *  │      OIL         │  ← small label
 *  │  ▓▓▓▓▓▓▓▓░░░░░  │  ← thin bar
 *  └──────────────────┘
 */
static void create_pod(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
    const char* label_text, const char* init_val,
    int32_t bar_min, int32_t bar_max, int32_t bar_init,
    gauge_pod_t* out)
{
    /* Container */
    lv_obj_t* pod = lv_obj_create(parent);
    lv_obj_set_pos(pod, x, y);
    lv_obj_set_size(pod, w, h);
    lv_obj_set_style_radius(pod, 12, 0);
    lv_obj_set_style_bg_color(pod, COL_POD_BG, 0);
    lv_obj_set_style_bg_opa(pod, POD_OPACITY, 0);
    lv_obj_set_style_border_color(pod, COL_POD_BORDER, 0);
    lv_obj_set_style_border_width(pod, 2, 0);
    lv_obj_set_style_pad_all(pod, 0, 0);
    lv_obj_set_scrollbar_mode(pod, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(pod, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pod, LV_OBJ_FLAG_SCROLLABLE);
    out->pod = pod;
    out->dot = NULL;

    /* Large value — centered */
    lv_obj_t* val = lv_label_create(pod);
    lv_label_set_text(val, init_val);
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_align(val, LV_ALIGN_TOP_MID, 0, 6);
    out->val = val;

    /* Small label below value — dimmer, smaller */
    lv_obj_t* lbl = lv_label_create(pod);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x6e7681), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 34);

    /* Thin bar at bottom */
    lv_obj_t* bar = lv_bar_create(pod);
    lv_obj_set_size(bar, w - 20, 4);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_bar_set_range(bar, bar_min, bar_max);
    lv_bar_set_value(bar, bar_init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_ARC_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_NORMAL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    out->bar = bar;
}

/* Update pod alert state: changes border, value text, bar, and dot color */
static void pod_set_alert(gauge_pod_t* p, lv_color_t color)
{
    lv_obj_set_style_border_color(p->pod, color, 0);
    lv_obj_set_style_text_color(p->val, color, 0);
    lv_obj_set_style_bg_color(p->bar, color, LV_PART_INDICATOR);
}

static void pod_set_normal(gauge_pod_t* p)
{
    lv_obj_set_style_border_color(p->pod, COL_POD_BORDER, 0);
    lv_obj_set_style_text_color(p->val, COL_TEXT, 0);
    lv_obj_set_style_bg_color(p->bar, COL_NORMAL, LV_PART_INDICATOR);
}

/* Create a tiny label for the bottom readout row */
static lv_obj_t* create_readout(lv_obj_t* parent, int32_t x, int32_t y, const char* text)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

void golden_screen_set_tile_path(const char* path)
{
    tile_path = path;
}

void golden_screen_set_alt_tile_path(const char* path)
{
    alt_tile_path = path;
}

/* ── Main screen builder ── */

void golden_screen_create(lv_obj_t* parent)
{
    lv_obj_t* root;

    if (parent) {
        /* Build inside provided parent (e.g. tileview tile) */
        root = parent;
    } else {
        /* Standalone: create our own circular mask on the active screen */
        lv_obj_t* scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        root = lv_obj_create(scr);
        lv_obj_set_size(root, DISP_SIZE, DISP_SIZE);
        lv_obj_center(root);
        lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(root, true, 0);
    }

    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    /* ════════════════════════════════════════════
     *  RPM SCALE — analog tachometer, 270° sweep
     * ════════════════════════════════════════════ */
    init_section_styles();

    rpm_scale = lv_scale_create(root);
    lv_obj_set_size(rpm_scale, RPM_SCALE_SIZE, RPM_SCALE_SIZE);
    lv_obj_center(rpm_scale);
    lv_scale_set_mode(rpm_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(rpm_scale, 0, RPM_MAX);
    lv_scale_set_angle_range(rpm_scale, 270);
    lv_scale_set_rotation(rpm_scale, 135);

    /* Ticks every 200 RPM, major every 1000 RPM with labels */
    lv_scale_set_total_tick_count(rpm_scale, 31);
    lv_scale_set_major_tick_every(rpm_scale, 5);
    lv_scale_set_label_show(rpm_scale, true);

    static const char* rpm_labels[] = {"0", "1", "2", "3", "4", "5", "6", NULL};
    lv_scale_set_text_src(rpm_scale, rpm_labels);

    /* Minor ticks (200 RPM intervals) */
    lv_obj_set_style_line_color(rpm_scale, COL_TEXT_DIM, LV_PART_ITEMS);
    lv_obj_set_style_line_width(rpm_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(rpm_scale, 12, LV_PART_ITEMS);

    /* Major ticks (1000 RPM) with labels */
    lv_obj_set_style_text_color(rpm_scale, COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(rpm_scale, &lv_font_montserrat_26, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(rpm_scale, COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(rpm_scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(rpm_scale, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(rpm_scale, COL_ARC_TRACK, LV_PART_MAIN);        /* background arc */
    lv_obj_set_style_arc_width(rpm_scale, 7, LV_PART_MAIN);

    /* Colored sections */
    lv_scale_section_t* sec_green = lv_scale_add_section(rpm_scale);
    lv_scale_set_section_range(rpm_scale, sec_green, 0, RPM_YELLOW);
    lv_scale_set_section_style_main(rpm_scale, sec_green, &style_section_green_main);
    lv_scale_set_section_style_indicator(rpm_scale, sec_green, &style_section_green_indicator);
    lv_scale_set_section_style_items(rpm_scale, sec_green, &style_section_green_items);

    lv_scale_section_t* sec_yellow = lv_scale_add_section(rpm_scale);
    lv_scale_set_section_range(rpm_scale, sec_yellow, RPM_YELLOW, RPM_REDLINE);
    lv_scale_set_section_style_main(rpm_scale, sec_yellow, &style_section_yellow_main);
    lv_scale_set_section_style_indicator(rpm_scale, sec_yellow, &style_section_yellow_indicator);
    lv_scale_set_section_style_items(rpm_scale, sec_yellow, &style_section_yellow_items);

    lv_scale_section_t* sec_red = lv_scale_add_section(rpm_scale);
    lv_scale_set_section_range(rpm_scale, sec_red, RPM_REDLINE, RPM_MAX);
    lv_scale_set_section_style_main(rpm_scale, sec_red, &style_section_red_main);
    lv_scale_set_section_style_indicator(rpm_scale, sec_red, &style_section_red_indicator);
    lv_scale_set_section_style_items(rpm_scale, sec_red, &style_section_red_items);

    /* Needle — a line object managed by the scale */
    rpm_needle = lv_line_create(rpm_scale);
    lv_obj_set_style_line_color(rpm_needle, COL_RED, 0);
    lv_obj_set_style_line_width(rpm_needle, 5, 0);
    lv_scale_set_line_needle_value(rpm_scale, rpm_needle, RPM_NEEDLE_LEN, 0);

    /* Draw ticks on top of the needle for a layered look */
    lv_scale_set_post_draw(rpm_scale, true);

    /* ════════════════════════════════════════════
     *  Chart — large background circle, created first for lowest z-order.
     *  520px fills the space inside the scale ticks, offset slightly down
     *  to center between SOG row and bottom bars.
     * ════════════════════════════════════════════ */
    lv_obj_t* chart_area = lv_obj_create(root);
    lv_obj_set_size(chart_area, 600, 600);
    lv_obj_center(chart_area);
    lv_obj_set_style_radius(chart_area, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(chart_area, true, 0);
    lv_obj_set_style_bg_color(chart_area, COL_BG, 0);
    lv_obj_set_style_bg_opa(chart_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart_area, 0, 0);
    lv_obj_set_style_pad_all(chart_area, 0, 0);
    lv_obj_set_scrollbar_mode(chart_area, LV_SCROLLBAR_MODE_OFF);

    /* Tile-based map renderer */
    if (tile_path) {
        gs_map = map_renderer_create(chart_area, tile_path, 600);
        if (gs_map) {
            map_renderer_set_view(gs_map, 45.00, 14.61, 13);
            map_renderer_set_vignette(gs_map, 0.65f, COL_BG);
            map_renderer_create_track_btn(gs_map, root, 220, 60);
            if (alt_tile_path) {
                map_renderer_set_alt_tiles(gs_map, alt_tile_path, root, -220, 60);
            }
        }
    }

    /* ════════════════════════════════════════════
     *  SOG — with dark backdrop for readability over chart
     * ════════════════════════════════════════════ */
    lv_obj_t* sog_panel = lv_obj_create(root);
    lv_obj_set_size(sog_panel, 200, 60);
    lv_obj_align(sog_panel, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_radius(sog_panel, 14, 0);
    lv_obj_set_style_bg_color(sog_panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(sog_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(sog_panel, 0, 0);
    lv_obj_set_style_pad_all(sog_panel, 0, 0);
    lv_obj_set_scrollbar_mode(sog_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(sog_panel, LV_OBJ_FLAG_CLICKABLE);

    sog_label = lv_label_create(sog_panel);
    lv_label_set_text(sog_label, "0.0");
    lv_obj_set_style_text_color(sog_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(sog_label, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(sog_label, 20, 5);

    lv_obj_t* sog_unit = lv_label_create(sog_panel);
    lv_label_set_text(sog_unit, "kn");
    lv_obj_set_style_text_color(sog_unit, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sog_unit, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(sog_unit, 145, 25);

    /* ════════════════════════════════════════════
     *  Depth + Water Temp — with dark backdrop
     * ════════════════════════════════════════════ */
    lv_obj_t* nav_panel = lv_obj_create(root);
    lv_obj_set_size(nav_panel, 260, 35);
    lv_obj_align(nav_panel, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_radius(nav_panel, 10, 0);
    lv_obj_set_style_bg_color(nav_panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(nav_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(nav_panel, 0, 0);
    lv_obj_set_style_pad_all(nav_panel, 0, 0);
    lv_obj_set_scrollbar_mode(nav_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(nav_panel, LV_OBJ_FLAG_CLICKABLE);

    depth_label = lv_label_create(nav_panel);
    lv_label_set_text(depth_label, "---m");
    lv_obj_set_style_text_color(depth_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(depth_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(depth_label, 30, 4);

    lv_obj_t* sep = lv_label_create(nav_panel);
    lv_label_set_text(sep, "|");
    lv_obj_set_style_text_color(sep, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sep, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(sep, 120, 4);

    watertemp_label = lv_label_create(nav_panel);
    lv_label_set_text(watertemp_label, "---°C");
    lv_obj_set_style_text_color(watertemp_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(watertemp_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(watertemp_label, 145, 4);

    /* ════════════════════════════════════════════
     *  Gauge pods — Row 1: 3 pods (OilT, OilP, CltT)
     *  145×58px each, 10px gaps, centered in 460px
     * ════════════════════════════════════════════ */
    int32_t pod_w1 = 145, pod_h1 = 72, pod_gap = 10;
    int32_t row1_total = pod_w1 * 3 + pod_gap * 2;
    int32_t row1_x = (DISP_SIZE - row1_total) / 2;
    int32_t row1_y = 545;

    create_pod(root, row1_x,                      row1_y, pod_w1, pod_h1,
        "OIL",   "92°C",  40, 150, 92,  &pod_oilt);
    create_pod(root, row1_x + pod_w1 + pod_gap,   row1_y, pod_w1, pod_h1,
        "PRESS", "4.2bar", 0, 600, 420, &pod_oilp);
    create_pod(root, row1_x + (pod_w1 + pod_gap)*2, row1_y, pod_w1, pod_h1,
        "CLT",   "72°C",  40, 110, 77,  &pod_clt);

    /* ════════════════════════════════════════════
     *  Gauge pods — Row 2: 3 pods (Lambda, CltP, Batt)
     *  130×55px each, narrower to follow circle curve
     * ════════════════════════════════════════════ */
    int32_t pod_w2 = 130, pod_h2 = 68;
    int32_t row2_total = pod_w2 * 3 + pod_gap * 2;
    int32_t row2_x = (DISP_SIZE - row2_total) / 2;
    int32_t row2_y = row1_y + pod_h1 + pod_gap;

    create_pod(root, row2_x,                      row2_y, pod_w2, pod_h2,
        "LAM",  "1.00",   70, 130, 100, &pod_lam);
    create_pod(root, row2_x + pod_w2 + pod_gap,   row2_y, pod_w2, pod_h2,
        "CP",   "55kPa",  0,  100, 55,  &pod_cltp);
    create_pod(root, row2_x + (pod_w2 + pod_gap)*2, row2_y, pod_w2, pod_h2,
        "BAT",  "14.1V",  110, 150, 141, &pod_batt);

    /* ════════════════════════════════════════════
     *  Bottom digital readouts
     *  Row 1: L1, L2, IAT, MAP (4 items)
     *  Row 2: FP, FC (2 items, centered lower in the circle)
     * ════════════════════════════════════════════ */
    int32_t rdout1_y = row2_y + pod_h2 + 12;
    int32_t rdout2_y = rdout1_y + 22;
    int32_t col_w = 85;

    int32_t rdout1_start = (DISP_SIZE - col_w * 4) / 2;
    lbl_lambda1 = create_readout(root, rdout1_start,             rdout1_y, "L1 ---");
    lbl_lambda2 = create_readout(root, rdout1_start + col_w,     rdout1_y, "L2 ---");
    lbl_iat     = create_readout(root, rdout1_start + col_w * 2, rdout1_y, "IAT ---");
    lbl_map     = create_readout(root, rdout1_start + col_w * 3, rdout1_y, "MAP ---");

    int32_t rdout2_w = 130;
    int32_t rdout2_start = (DISP_SIZE - rdout2_w * 2) / 2;
    lbl_fp      = create_readout(root, rdout2_start,          rdout2_y, "FP ---");
    lbl_fuel    = create_readout(root, rdout2_start + rdout2_w, rdout2_y, "FC ---");
}

/* ════════════════════════════════════════════
 *  Update all widgets from gauge data
 * ════════════════════════════════════════════ */
void golden_screen_update(const gauge_data_t* d)
{
    char buf[32];

    /* RPM needle + digital readout */
    lv_scale_set_line_needle_value(rpm_scale, rpm_needle, RPM_NEEDLE_LEN, (int32_t)d->rpm);

    /* Needle color follows zone */
    if (d->rpm >= RPM_REDLINE) {
        lv_obj_set_style_line_color(rpm_needle, COL_RED, 0);
    } else if (d->rpm >= RPM_YELLOW) {
        lv_obj_set_style_line_color(rpm_needle, COL_YELLOW, 0);
    } else {
        lv_obj_set_style_line_color(rpm_needle, lv_color_white(), 0);
    }

    /* SOG */
    snprintf(buf, sizeof(buf), "%.1f", d->sog_knots);
    lv_label_set_text(sog_label, buf);

    /* Depth */
    snprintf(buf, sizeof(buf), "%.1fm", d->depth_m);
    lv_label_set_text(depth_label, buf);

    /* Water temp */
    snprintf(buf, sizeof(buf), "%.0f°C", d->water_temp_c);
    lv_label_set_text(watertemp_label, buf);

    /* ── Pod updates with alert system ── */

    /* Oil temp pod */
    lv_bar_set_value(pod_oilt.bar, (int32_t)d->oil_temp_c, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0f°C", d->oil_temp_c);
    lv_label_set_text(pod_oilt.val, buf);
    if (d->oil_temp_c > 125)      pod_set_alert(&pod_oilt, COL_RED);
    else if (d->oil_temp_c > 110)  pod_set_alert(&pod_oilt, COL_YELLOW);
    else                           pod_set_normal(&pod_oilt);

    /* Oil pressure pod */
    lv_bar_set_value(pod_oilp.bar, (int32_t)d->oil_pressure_kpa, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.1f", d->oil_pressure_kpa / 100.0f);
    lv_label_set_text(pod_oilp.val, buf);
    if (d->oil_pressure_kpa < 150)       pod_set_alert(&pod_oilp, COL_RED);
    else if (d->oil_pressure_kpa < 250)  pod_set_alert(&pod_oilp, COL_YELLOW);
    else                                 pod_set_normal(&pod_oilp);

    /* Coolant temp pod */
    lv_bar_set_value(pod_clt.bar, (int32_t)d->coolant_temp_c, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0f°C", d->coolant_temp_c);
    lv_label_set_text(pod_clt.val, buf);
    if (d->coolant_temp_c > 77)       pod_set_alert(&pod_clt, COL_RED);
    else if (d->coolant_temp_c > 70)  pod_set_alert(&pod_clt, COL_YELLOW);
    else                              pod_set_normal(&pod_clt);

    /* Lambda pod — worst (leanest) of both banks */
    float worst_lambda = d->lambda1 > d->lambda2 ? d->lambda1 : d->lambda2;
    lv_bar_set_value(pod_lam.bar, (int32_t)(worst_lambda * 100), LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.2f", worst_lambda);
    lv_label_set_text(pod_lam.val, buf);
    if (worst_lambda < 0.85f || worst_lambda > 1.15f)       pod_set_alert(&pod_lam, COL_RED);
    else if (worst_lambda < 0.90f || worst_lambda > 1.10f)  pod_set_alert(&pod_lam, COL_YELLOW);
    else                                                    pod_set_normal(&pod_lam);

    /* Coolant pressure pod */
    lv_bar_set_value(pod_cltp.bar, (int32_t)d->coolant_pressure_kpa, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0f", d->coolant_pressure_kpa);
    lv_label_set_text(pod_cltp.val, buf);
    if (d->coolant_pressure_kpa < 15)       pod_set_alert(&pod_cltp, COL_RED);
    else if (d->coolant_pressure_kpa < 25)  pod_set_alert(&pod_cltp, COL_YELLOW);
    else                                    pod_set_normal(&pod_cltp);

    /* Battery voltage pod */
    lv_bar_set_value(pod_batt.bar, (int32_t)(d->battery_voltage * 10), LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.1fV", d->battery_voltage);
    lv_label_set_text(pod_batt.val, buf);
    if (d->battery_voltage < 12.0f)       pod_set_alert(&pod_batt, COL_RED);
    else if (d->battery_voltage < 12.8f)  pod_set_alert(&pod_batt, COL_YELLOW);
    else                                  pod_set_normal(&pod_batt);

    /* Bottom digital readouts — "KEY value unit" format */
    snprintf(buf, sizeof(buf), "L1 %.2f", d->lambda1);
    lv_label_set_text(lbl_lambda1, buf);

    snprintf(buf, sizeof(buf), "L2 %.2f", d->lambda2);
    lv_label_set_text(lbl_lambda2, buf);

    snprintf(buf, sizeof(buf), "IAT %.0f°C", d->iat_c);
    lv_label_set_text(lbl_iat, buf);

    snprintf(buf, sizeof(buf), "MAP %.0f kPa", d->map_kpa);
    lv_label_set_text(lbl_map, buf);

    snprintf(buf, sizeof(buf), "FP %.0f kPa", d->fuel_pressure_kpa);
    lv_label_set_text(lbl_fp, buf);

    snprintf(buf, sizeof(buf), "FC %.1f l/h", d->fuel_rate_lph);
    lv_label_set_text(lbl_fuel, buf);

    /* Update map position marker */
    if (gs_map && d->latitude != 0.0 && d->longitude != 0.0) {
        map_renderer_set_position(gs_map, d->latitude, d->longitude, d->cog_degrees);
    }
}

map_renderer_t* golden_screen_get_map(void) { return gs_map; }
