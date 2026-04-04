#include "screen_manager.h"
#include "golden_screen.h"
#include "map_renderer.h"
#include <stdio.h>

#define DISP_SIZE 800

/* Colors — shared with golden_screen */
#define COL_BG          lv_color_hex(0x0d1117)
#define COL_TEXT        lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM    lv_color_hex(0x8b949e)
#define COL_CHART_BG    lv_color_hex(0x0a1628)

static const char* tile_path = NULL;
static lv_obj_t* tileview;
static int32_t current_screen = 0;
#define NUM_SCREENS 3

/* Screen 2 widgets */
static map_renderer_t* s2_map = NULL;
static lv_obj_t* s2_sog_label;
static lv_obj_t* s2_cog_label;

/* Screen 3 widgets */
static lv_obj_t* s3_rpm_label;
static lv_obj_t* s3_oilt_label;
static lv_obj_t* s3_oilp_label;
static lv_obj_t* s3_clt_label;
static lv_obj_t* s3_iat_label;
static lv_obj_t* s3_lambda1_label;
static lv_obj_t* s3_lambda2_label;
static lv_obj_t* s3_map_label;
static lv_obj_t* s3_fp_label;
static lv_obj_t* s3_batt_label;
static lv_obj_t* s3_fuel_label;
static lv_obj_t* s3_cltp_label;
static lv_obj_t* s3_hours_label;

/* ── Helpers ── */

static lv_obj_t* create_big_value(lv_obj_t* parent, int32_t x, int32_t y,
    const char* label_text, const char* init_val)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lbl, x, y);

    lv_obj_t* val = lv_label_create(parent);
    lv_label_set_text(val, init_val);
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(val, x, y + 22);
    return val;
}

/* ── Screen 2: Full Chart ── */

static void create_screen2(lv_obj_t* tile)
{
    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* Full-size chart */
    lv_obj_t* chart_area = lv_obj_create(tile);
    lv_obj_set_size(chart_area, DISP_SIZE, DISP_SIZE);
    lv_obj_center(chart_area);
    lv_obj_set_style_bg_color(chart_area, COL_CHART_BG, 0);
    lv_obj_set_style_bg_opa(chart_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart_area, 0, 0);
    lv_obj_set_style_pad_all(chart_area, 0, 0);
    lv_obj_set_scrollbar_mode(chart_area, LV_SCROLLBAR_MODE_OFF);

    if (tile_path) {
        s2_map = map_renderer_create(chart_area, tile_path, DISP_SIZE);
        if (s2_map) {
            map_renderer_set_view(s2_map, 45.00, 14.61, 13);
            map_renderer_create_track_btn(s2_map, tile, 250, 200);
        }
    }

    /* SOG/COG overlay bar at bottom */
    lv_obj_t* overlay = lv_obj_create(tile);
    lv_obj_set_size(overlay, 300, 45);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_radius(overlay, 12, 0);
    lv_obj_set_style_bg_color(overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_scrollbar_mode(overlay, LV_SCROLLBAR_MODE_OFF);

    s2_sog_label = lv_label_create(overlay);
    lv_label_set_text(s2_sog_label, "0.0 kn");
    lv_obj_set_style_text_color(s2_sog_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s2_sog_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s2_sog_label, 25, 10);

    s2_cog_label = lv_label_create(overlay);
    lv_label_set_text(s2_cog_label, "000°");
    lv_obj_set_style_text_color(s2_cog_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s2_cog_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s2_cog_label, 200, 10);
}

/* ── Screen 3: Engine Detail ── */

static void create_screen3(lv_obj_t* tile)
{
    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* Title */
    lv_obj_t* title = lv_label_create(tile);
    lv_label_set_text(title, "ENGINE");
    lv_obj_set_style_text_color(title, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* RPM large */
    s3_rpm_label = lv_label_create(tile);
    lv_label_set_text(s3_rpm_label, "0 RPM");
    lv_obj_set_style_text_color(s3_rpm_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s3_rpm_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s3_rpm_label, LV_ALIGN_TOP_MID, 0, 65);

    /* 2-column grid of engine parameters */
    int32_t col1 = 180, col2 = 430;
    int32_t row_start = 150, row_h = 55;

    s3_oilt_label  = create_big_value(tile, col1, row_start,              "Oil Temp",     "---°C");
    s3_oilp_label  = create_big_value(tile, col2, row_start,              "Oil Press",    "--- bar");
    s3_clt_label   = create_big_value(tile, col1, row_start + row_h,      "Coolant",      "---°C");
    s3_iat_label   = create_big_value(tile, col2, row_start + row_h,      "Intake Air",   "---°C");
    s3_lambda1_label = create_big_value(tile, col1, row_start + row_h * 2, "Lambda 1",    "---");
    s3_lambda2_label = create_big_value(tile, col2, row_start + row_h * 2, "Lambda 2",    "---");
    s3_map_label   = create_big_value(tile, col1, row_start + row_h * 3,  "MAP",          "--- kPa");
    s3_fp_label    = create_big_value(tile, col2, row_start + row_h * 3,  "Fuel Press",   "--- kPa");
    s3_batt_label  = create_big_value(tile, col1, row_start + row_h * 4,  "Battery",      "---V");
    s3_fuel_label  = create_big_value(tile, col2, row_start + row_h * 4,  "Fuel Rate",    "--- l/h");
    s3_cltp_label  = create_big_value(tile, col1, row_start + row_h * 5,  "Coolant Press", "--- kPa");
    s3_hours_label = create_big_value(tile, col2, row_start + row_h * 5,  "Engine Hours", "---");
}

/* ── Public API ── */

void screen_manager_set_tile_path(const char* path)
{
    tile_path = path;
    golden_screen_set_tile_path(path);
}

void screen_manager_create(void)
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Circular mask — clips everything to round display */
    lv_obj_t* circle = lv_obj_create(scr);
    lv_obj_set_size(circle, DISP_SIZE, DISP_SIZE);
    lv_obj_center(circle);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(circle, true, 0);
    lv_obj_set_style_bg_color(circle, COL_BG, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);

    /* Tileview for horizontal swipe navigation */
    tileview = lv_tileview_create(circle);
    lv_obj_set_size(tileview, DISP_SIZE, DISP_SIZE);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    /* Tile 0: Golden screen (main) */
    lv_obj_t* t0 = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    golden_screen_create(t0);

    /* Tile 1: Full chart */
    lv_obj_t* t1 = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    create_screen2(t1);

    /* Tile 2: Engine detail */
    lv_obj_t* t2 = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_LEFT);
    create_screen3(t2);
}

void screen_manager_update(const gauge_data_t* d)
{
    char buf[32];

    /* Update golden screen (Screen 1) */
    golden_screen_update(d);

    /* Update Screen 2: map position + SOG/COG overlay */
    if (s2_map && d->latitude != 0.0 && d->longitude != 0.0) {
        map_renderer_set_position(s2_map, d->latitude, d->longitude, d->cog_degrees);
    }
    snprintf(buf, sizeof(buf), "%.1f kn", d->sog_knots);
    lv_label_set_text(s2_sog_label, buf);
    snprintf(buf, sizeof(buf), "%.0f°", d->cog_degrees);
    lv_label_set_text(s2_cog_label, buf);

    /* Update Screen 3: Engine detail */
    snprintf(buf, sizeof(buf), "%d RPM", (int)d->rpm);
    lv_label_set_text(s3_rpm_label, buf);
    snprintf(buf, sizeof(buf), "%.0f°C", d->oil_temp_c);
    lv_label_set_text(s3_oilt_label, buf);
    snprintf(buf, sizeof(buf), "%.1f bar", d->oil_pressure_kpa / 100.0f);
    lv_label_set_text(s3_oilp_label, buf);
    snprintf(buf, sizeof(buf), "%.0f°C", d->coolant_temp_c);
    lv_label_set_text(s3_clt_label, buf);
    snprintf(buf, sizeof(buf), "%.0f°C", d->iat_c);
    lv_label_set_text(s3_iat_label, buf);
    snprintf(buf, sizeof(buf), "%.3f", d->lambda1);
    lv_label_set_text(s3_lambda1_label, buf);
    snprintf(buf, sizeof(buf), "%.3f", d->lambda2);
    lv_label_set_text(s3_lambda2_label, buf);
    snprintf(buf, sizeof(buf), "%.0f kPa", d->map_kpa);
    lv_label_set_text(s3_map_label, buf);
    snprintf(buf, sizeof(buf), "%.0f kPa", d->fuel_pressure_kpa);
    lv_label_set_text(s3_fp_label, buf);
    snprintf(buf, sizeof(buf), "%.1fV", d->battery_voltage);
    lv_label_set_text(s3_batt_label, buf);
    snprintf(buf, sizeof(buf), "%.1f l/h", d->fuel_rate_lph);
    lv_label_set_text(s3_fuel_label, buf);
    snprintf(buf, sizeof(buf), "%.0f kPa", d->coolant_pressure_kpa);
    lv_label_set_text(s3_cltp_label, buf);
    snprintf(buf, sizeof(buf), "%dh", d->engine_hours_s / 3600);
    lv_label_set_text(s3_hours_label, buf);
}

void screen_manager_next(void)
{
    if (current_screen < NUM_SCREENS - 1) {
        current_screen++;
        lv_tileview_set_tile_by_index(tileview, current_screen, 0, LV_ANIM_ON);
    }
}

void screen_manager_prev(void)
{
    if (current_screen > 0) {
        current_screen--;
        lv_tileview_set_tile_by_index(tileview, current_screen, 0, LV_ANIM_ON);
    }
}

map_renderer_t* screen_manager_get_active_map(void)
{
    switch (current_screen) {
        case 0: return golden_screen_get_map();
        case 1: return s2_map;
        default: return NULL;
    }
}
