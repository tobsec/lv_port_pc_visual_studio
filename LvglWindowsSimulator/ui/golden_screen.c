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
static map_renderer_t* gs_map = NULL;

/* ── Widget handles for update ── */
static lv_obj_t* rpm_scale;
static lv_obj_t* rpm_needle;
static lv_obj_t* sog_label;
static lv_obj_t* depth_label;
static lv_obj_t* watertemp_label;
static lv_obj_t* oilt_bar;
static lv_obj_t* oilt_val;
static lv_obj_t* oilp_bar;
static lv_obj_t* oilp_val;
static lv_obj_t* clt_bar;
static lv_obj_t* clt_val;
static lv_obj_t* lambda_bar;
static lv_obj_t* lambda_val;
static lv_obj_t* cltp_bar;
static lv_obj_t* cltp_val;
static lv_obj_t* batt_bar;
static lv_obj_t* batt_val;
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

/* Create a horizontal bar gauge row: "Label ████░░░░ value" */
static void create_bar_row(lv_obj_t* parent, int32_t y_pos,
    const char* name, int32_t min_val, int32_t max_val, int32_t init_val,
    lv_color_t bar_color,
    lv_obj_t** out_bar, lv_obj_t** out_val_label)
{
    /* Label on left */
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lbl, 185, y_pos + 2);

    /* Bar */
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 300, 18);
    lv_obj_set_pos(bar, 250, y_pos + 3);
    lv_bar_set_range(bar, min_val, max_val);
    lv_bar_set_value(bar, init_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    *out_bar = bar;

    /* Value label on right */
    lv_obj_t* val = lv_label_create(parent);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(val, 565, y_pos + 2);
    *out_val_label = val;
}

/* Create a smaller horizontal bar for the secondary gauges */
static void create_small_bar_row(lv_obj_t* parent, int32_t y_pos,
    const char* name, int32_t min_val, int32_t max_val, int32_t init_val,
    lv_color_t bar_color,
    lv_obj_t** out_bar, lv_obj_t** out_val_label)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(lbl, 195, y_pos + 1);

    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 280, 14);
    lv_obj_set_pos(bar, 260, y_pos + 2);
    lv_bar_set_range(bar, min_val, max_val);
    lv_bar_set_value(bar, init_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    *out_bar = bar;

    lv_obj_t* val = lv_label_create(parent);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(val, 555, y_pos + 1);
    *out_val_label = val;
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
    lv_obj_set_style_bg_color(chart_area, COL_CHART_BG, 0);
    lv_obj_set_style_bg_opa(chart_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart_area, lv_color_hex(0x1a2540), 0);
    lv_obj_set_style_border_width(chart_area, 1, 0);
    lv_obj_set_style_pad_all(chart_area, 0, 0);
    lv_obj_set_scrollbar_mode(chart_area, LV_SCROLLBAR_MODE_OFF);

    /* Tile-based map renderer */
    if (tile_path) {
        gs_map = map_renderer_create(chart_area, tile_path, 600);
        if (gs_map) {
            map_renderer_set_view(gs_map, 45.00, 14.61, 13);
            /* Track button on root, positioned inside chart area (above bar backdrop) */
            map_renderer_create_track_btn(gs_map, root, 220, 60);
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
     *  Semi-transparent backdrop behind all bars — "frosted" overlay on chart
     * ════════════════════════════════════════════ */
    lv_obj_t* bar_backdrop = lv_obj_create(root);
    lv_obj_set_size(bar_backdrop, 460, 180);
    lv_obj_set_pos(bar_backdrop, (DISP_SIZE - 460) / 2, 530);
    lv_obj_set_style_radius(bar_backdrop, 20, 0);
    lv_obj_set_style_bg_color(bar_backdrop, COL_BG, 0);
    lv_obj_set_style_bg_opa(bar_backdrop, LV_OPA_80, 0);
    lv_obj_set_style_border_width(bar_backdrop, 0, 0);
    lv_obj_set_style_pad_all(bar_backdrop, 0, 0);
    lv_obj_set_scrollbar_mode(bar_backdrop, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(bar_backdrop, LV_OBJ_FLAG_CLICKABLE);

    /* ════════════════════════════════════════════
     *  Horizontal bar gauges — primary (OilT, OilP, CltT)
     * ════════════════════════════════════════════ */
    int32_t bar_y = 540;
    int32_t bar_sp = 30;

    create_bar_row(root, bar_y,
        "OilT", 40, 150, 92, COL_NORMAL, &oilt_bar, &oilt_val);
    create_bar_row(root, bar_y + bar_sp,
        "OilP", 0, 600, 420, COL_NORMAL, &oilp_bar, &oilp_val);
    create_bar_row(root, bar_y + bar_sp * 2,
        "CltT", 40, 110, 77, COL_NORMAL, &clt_bar, &clt_val);

    /* ════════════════════════════════════════════
     *  Horizontal bar gauges — secondary (Lambda, CltP, BattV)
     *  Slightly smaller, fitting between the "0" and "6" scale marks.
     * ════════════════════════════════════════════ */
    int32_t sbar_y = bar_y + bar_sp * 3 + 4;
    int32_t sbar_sp = 24;

    create_small_bar_row(root, sbar_y,
        "Lam", 70, 130, 100, COL_NORMAL, &lambda_bar, &lambda_val);
    create_small_bar_row(root, sbar_y + sbar_sp,
        "CltP", 0, 100, 55, COL_NORMAL, &cltp_bar, &cltp_val);
    create_small_bar_row(root, sbar_y + sbar_sp * 2,
        "Batt", 110, 150, 141, COL_NORMAL, &batt_bar, &batt_val);

    /* ════════════════════════════════════════════
     *  Bottom digital readouts
     *  Row 1: L1, L2, IAT, MAP (4 items)
     *  Row 2: FP, FC (2 items, centered lower in the circle)
     * ════════════════════════════════════════════ */
    int32_t row1_y = 715;
    int32_t row2_y = 740;
    int32_t col_w = 85;

    int32_t row1_start = (DISP_SIZE - col_w * 4) / 2;
    lbl_lambda1 = create_readout(root, row1_start,             row1_y, "L1 ---");
    lbl_lambda2 = create_readout(root, row1_start + col_w,     row1_y, "L2 ---");
    lbl_iat     = create_readout(root, row1_start + col_w * 2, row1_y, "IAT ---");
    lbl_map     = create_readout(root, row1_start + col_w * 3, row1_y, "MAP ---");

    /* Row 2: 2 items, wider spacing, centered lower in the circle */
    int32_t row2_w = 130;
    int32_t row2_start = (DISP_SIZE - row2_w * 2) / 2;
    lbl_fp      = create_readout(root, row2_start,          row2_y, "FP ---");
    lbl_fuel    = create_readout(root, row2_start + row2_w, row2_y, "FC ---");
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

    /* Oil temp bar */
    lv_bar_set_value(oilt_bar, (int32_t)d->oil_temp_c, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0f°C", d->oil_temp_c);
    lv_label_set_text(oilt_val, buf);
    lv_obj_set_style_bg_color(oilt_bar,
        d->oil_temp_c > 125 ? COL_RED : (d->oil_temp_c > 110 ? COL_YELLOW : COL_NORMAL),
        LV_PART_INDICATOR);

    /* Oil pressure bar (kPa, display as bar: 1 bar = 100 kPa) */
    lv_bar_set_value(oilp_bar, (int32_t)d->oil_pressure_kpa, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.1fbar", d->oil_pressure_kpa / 100.0f);
    lv_label_set_text(oilp_val, buf);
    lv_obj_set_style_bg_color(oilp_bar,
        d->oil_pressure_kpa < 150 ? COL_RED : (d->oil_pressure_kpa < 250 ? COL_YELLOW : COL_NORMAL),
        LV_PART_INDICATOR);

    /* Coolant temp bar */
    lv_bar_set_value(clt_bar, (int32_t)d->coolant_temp_c, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0f°C", d->coolant_temp_c);
    lv_label_set_text(clt_val, buf);
    lv_obj_set_style_bg_color(clt_bar,
        d->coolant_temp_c > 77 ? COL_RED : (d->coolant_temp_c > 70 ? COL_YELLOW : COL_NORMAL),
        LV_PART_INDICATOR);

    /* Lambda bar — show worst (leanest) of both banks, scale ×100 for bar range */
    float worst_lambda = d->lambda1 > d->lambda2 ? d->lambda1 : d->lambda2;
    lv_bar_set_value(lambda_bar, (int32_t)(worst_lambda * 100), LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.2f", worst_lambda);
    lv_label_set_text(lambda_val, buf);
    lv_obj_set_style_bg_color(lambda_bar,
        (worst_lambda < 0.85f || worst_lambda > 1.15f) ? COL_RED :
        (worst_lambda < 0.90f || worst_lambda > 1.10f) ? COL_YELLOW : COL_NORMAL,
        LV_PART_INDICATOR);

    /* Coolant pressure bar */
    lv_bar_set_value(cltp_bar, (int32_t)d->coolant_pressure_kpa, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.0fkPa", d->coolant_pressure_kpa);
    lv_label_set_text(cltp_val, buf);
    lv_obj_set_style_bg_color(cltp_bar,
        d->coolant_pressure_kpa < 15 ? COL_RED : (d->coolant_pressure_kpa < 25 ? COL_YELLOW : COL_NORMAL),
        LV_PART_INDICATOR);

    /* Battery voltage bar (range 11.0-15.0V, displayed as 110-150 in bar) */
    lv_bar_set_value(batt_bar, (int32_t)(d->battery_voltage * 10), LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%.1fV", d->battery_voltage);
    lv_label_set_text(batt_val, buf);
    lv_obj_set_style_bg_color(batt_bar,
        d->battery_voltage < 12.0f ? COL_RED : (d->battery_voltage < 12.8f ? COL_YELLOW : COL_NORMAL),
        LV_PART_INDICATOR);

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
