#include "screen_manager.h"
#include "golden_screen.h"
#include "map_renderer.h"
#include "tile_cache.h"
#include "warning_overlay.h"
#include "ais_screen.h"
#include "ais_store.h"
#include "media_screen.h"

/* n2k component REQUIRES ui, so we can't add n2k as a dep of ui without
 * making the graph cyclic. Forward-declare the one call we need here as
 * a weak symbol — the linker resolves it against components/n2k when
 * that component is present, and a NULL call is elided by the runtime
 * guard below. */
struct map_renderer;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void n2k_viewport_set_map(struct map_renderer *mr);
#else
/* MSVC (sim build): weak symbols aren't a thing the same way. Just
 * declare a stub so the address-taken guard below evaluates false-y. */
static void n2k_viewport_set_map(struct map_renderer *mr) { (void)mr; }
#endif
#ifdef ESP_PLATFORM
  #include "last_position.h"
#endif
#include "icons/lv_image_telltale_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Debug-only on-screen frame counter (forces a redraw every frame). */
#define SCREEN_DEBUG_TICK 0

#define DISP_SIZE 800

/* Colors — shared with golden_screen */
#define COL_BG          lv_color_hex(0x0d1117)
#define COL_TEXT        lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM    lv_color_hex(0x8b949e)
#define COL_CHART_BG    lv_color_hex(0x0a1628)

static const char* tile_path = NULL;
static const char* alt_tile_path = NULL;
static lv_obj_t* tileview;
static lv_obj_t* demo_badge = NULL;   /* "DEMO" badge, shown when on simulated data */
#define NUM_SCREENS 6
/* Tile order, left → right. Settings sits left of the golden/home screen;
 * the AIS list sits between the full chart and the engine detail so
 * "chart" → "targets I see" reads as a natural progression. Media (the
 * Fusion control tile) sits between AIS and Engine — both are "boat
 * status" screens so grouping them keeps the mental model consistent. */
enum { SCREEN_SETTINGS = 0, SCREEN_GOLDEN, SCREEN_CHART, SCREEN_AIS, SCREEN_MEDIA, SCREEN_ENGINE };
static int32_t current_screen = SCREEN_GOLDEN;   /* boot on the golden screen */
static bool swiping = false;       /* true during tileview scroll animation */

/* Settings screen (tile 3) */
static screen_hooks_t s_hooks;
static uint8_t s_init_brightness = 100;
static bool    s_init_demo = true;
static uint8_t s_init_ais_range_nm = 25;
static bool    s_init_ap_enabled = true;
static lv_obj_t* s4_bright_val;
static lv_obj_t* s4_ais_range_val;
static warn_thresholds_t s_thr_vals;
static bool s_thr_set = false;
static lv_obj_t* thr_val_lbl[THR_COUNT];

/* Shared tile cache for all map renderers */
static tile_cache_t* g_tile_cache = NULL;

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

/* Label = formatted value, or a "---" placeholder when the source PDU is stale. */
static void set_val(lv_obj_t* lbl, bool valid, const char* fmt, double v, const char* dash)
{
    char b[32];
    if (valid) snprintf(b, sizeof(b), fmt, v);
    else       snprintf(b, sizeof(b), "%s", dash);
    const char* cur = lv_label_get_text(lbl);   /* skip if the text is unchanged */
    if (!cur || strcmp(cur, b) != 0) lv_label_set_text(lbl, b);
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
            /* View set later (after the cache exists) so tiles load async. */
            map_renderer_create_track_btn(s2_map, tile, 250, 200);
            map_renderer_create_zoom_btns(s2_map, tile, -30, 200, 30, 200);
            if (alt_tile_path) {
                map_renderer_set_alt_tiles(s2_map, alt_tile_path, tile, -250, 200);
            }
        }
    } else {
        /* No SD / no map data — replace the empty chart area with a clear
         * "card missing" overlay so the screen isn't just a black hole. */
        lv_obj_t* icon = lv_image_create(chart_area);
        lv_image_set_src(icon, &tt_icon_sd_missing);
        lv_obj_set_style_image_recolor(icon, COL_TEXT_DIM, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t* lbl = lv_label_create(chart_area);
        lv_label_set_text(lbl, "No map data\nSD card not available");
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 40);
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

/* ── Screen 4: Settings ── */

/* Editable threshold descriptors, indexed by thr_id_t. */
typedef struct {
    const char* label;
    uint16_t min, max, step;
    float    scale;     /* stored value * scale = displayed number */
    uint8_t  decimals;
    const char* unit;
} thr_desc_t;

static const thr_desc_t THR_DESC[THR_COUNT] = {
    /* RPM_REDLINE  */ { "RPM redline",  4000, 7000, 50,  1.0f,  0, "" },
    /* OIL_TEMP_MAX */ { "Oil temp max", 100,  150,  1,   1.0f,  0, " \xC2\xB0""C" },
    /* CLT_TEMP_MAX */ { "Coolant max",  60,   110,  1,   1.0f,  0, " \xC2\xB0""C" },
    /* OIL_PRESS_MIN*/ { "Oil press min",50,   400,  10,  0.01f, 1, " bar" },
    /* DEPTH_MIN    */ { "Shallow water",50,   1000, 10,  0.01f, 1, " m" },
};

static void fmt_thr(char* buf, size_t n, int id)
{
    const thr_desc_t* d = &THR_DESC[id];
    snprintf(buf, n, "%.*f%s", d->decimals, s_thr_vals.v[id] * d->scale, d->unit);
}

static void bright_slider_cb(lv_event_t* e)
{
    lv_obj_t* sl = (lv_obj_t*)lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 21) v = 21;
    char b[16];
    snprintf(b, sizeof(b), "%d%%", (int)v);
    lv_label_set_text(s4_bright_val, b);
    if (s_hooks.set_brightness) s_hooks.set_brightness((uint8_t)v);
}

/* AIS range slider — snap to 5-NM steps so the user always lands on a
 * whole round number. The gauge's PGN 130961 broadcaster reloads the
 * NVS-persisted range on its next tick (≤5 s after save). */
static void ais_range_slider_cb(lv_event_t* e)
{
    lv_obj_t* sl = (lv_obj_t*)lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    v = ((v + 2) / 5) * 5;              /* round to nearest 5 */
    if (v < 5)  v = 5;
    if (v > 40) v = 40;
    lv_slider_set_value(sl, v, LV_ANIM_OFF);
    char b[24];
    snprintf(b, sizeof(b), "AIS range %d NM", (int)v);
    lv_label_set_text(s4_ais_range_val, b);
    if (s_hooks.set_ais_range_nm) s_hooks.set_ais_range_nm((uint8_t)v);
}

static void demo_switch_cb(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool demo = lv_obj_has_state(sw, LV_STATE_CHECKED);
    screen_manager_set_demo(demo);            /* DEMO badge */
    if (s_hooks.set_demo) s_hooks.set_demo(demo);
}

static void ap_switch_cb(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_hooks.set_ap_enabled) s_hooks.set_ap_enabled(en);
}

static void thr_step(int id, int dir)
{
    const thr_desc_t* d = &THR_DESC[id];
    int32_t nv = (int32_t)s_thr_vals.v[id] + dir * (int32_t)d->step;
    if (nv < d->min) nv = d->min;
    if (nv > d->max) nv = d->max;
    s_thr_vals.v[id] = (uint16_t)nv;

    char b[24];
    fmt_thr(b, sizeof(b), id);
    lv_label_set_text(thr_val_lbl[id], b);

    warning_overlay_set_thresholds(&s_thr_vals);            /* apply now */
    if (s_hooks.set_threshold) s_hooks.set_threshold(id, s_thr_vals.v[id]);  /* persist */
}

static void thr_dec_cb(lv_event_t* e) { thr_step((int)(intptr_t)lv_event_get_user_data(e), -1); }
static void thr_inc_cb(lv_event_t* e) { thr_step((int)(intptr_t)lv_event_get_user_data(e), +1); }

/* One threshold row: "Label            value [-][+]" */
static void create_thr_row(lv_obj_t* parent, int id)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, 560, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, THR_DESC[id].label);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    thr_val_lbl[id] = lv_label_create(row);
    char b[24];
    fmt_thr(b, sizeof(b), id);
    lv_label_set_text(thr_val_lbl[id], b);
    lv_obj_set_style_text_color(thr_val_lbl[id], COL_TEXT, 0);
    lv_obj_set_style_text_font(thr_val_lbl[id], &lv_font_montserrat_20, 0);
    lv_obj_align(thr_val_lbl[id], LV_ALIGN_RIGHT_MID, -120, 0);

    lv_obj_t* minus = lv_button_create(row);
    lv_obj_set_size(minus, 48, 44);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -56, 0);
    lv_obj_add_event_cb(minus, thr_dec_cb, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    lv_obj_t* ml = lv_label_create(minus); lv_label_set_text(ml, "-"); lv_obj_center(ml);

    lv_obj_t* plus = lv_button_create(row);
    lv_obj_set_size(plus, 48, 44);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(plus, thr_inc_cb, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    lv_obj_t* pl = lv_label_create(plus); lv_label_set_text(pl, "+"); lv_obj_center(pl);
}

static void create_screen4(lv_obj_t* tile)
{
    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* Vertical, scrollable column kept inside the round display's safe band
     * (centred 620x600 window) so 560-wide rows never hit the curved edges.
     * Scrolls vertically as more settings are added; the tileview keeps the
     * horizontal swipe. */
    lv_obj_t* col = lv_obj_create(tile);
    lv_obj_set_size(col, 620, 600);
    lv_obj_center(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t* title = lv_label_create(col);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    /* Brightness */
    lv_obj_t* sl = lv_slider_create(col);
    lv_obj_set_size(sl, 360, 18);
    /* Below ~21% the panel backlight goes dark, so 21% is the usable minimum. */
    lv_slider_set_range(sl, 21, 100);
    lv_slider_set_value(sl, s_init_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s4_bright_val = lv_label_create(col);
    char b[16];
    snprintf(b, sizeof(b), "Brightness %d%%", (int)s_init_brightness);
    lv_label_set_text(s4_bright_val, b);
    lv_obj_set_style_text_color(s4_bright_val, COL_TEXT, 0);
    lv_obj_set_style_text_font(s4_bright_val, &lv_font_montserrat_18, 0);

    /* Demo / live row */
    lv_obj_t* drow = lv_obj_create(col);
    lv_obj_set_size(drow, 560, 48);
    lv_obj_set_style_bg_opa(drow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drow, 0, 0);
    lv_obj_set_style_pad_all(drow, 0, 0);
    lv_obj_remove_flag(drow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* dl = lv_label_create(drow);
    lv_label_set_text(dl, "Demo data");
    lv_obj_set_style_text_color(dl, COL_TEXT, 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
    lv_obj_align(dl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* sw = lv_switch_create(drow);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_init_demo) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, demo_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* AIS request range — how wide the sim / iOS bridge should pull AIS
     * around own position when the user hasn't panned away. See
     * docs/gauge-viewport-protocol.md. */
    lv_obj_t* ais_sl = lv_slider_create(col);
    lv_obj_set_size(ais_sl, 360, 18);
    lv_slider_set_range(ais_sl, 5, 40);
    lv_slider_set_value(ais_sl, s_init_ais_range_nm, LV_ANIM_OFF);
    lv_obj_add_event_cb(ais_sl, ais_range_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s4_ais_range_val = lv_label_create(col);
    char ab[24];
    snprintf(ab, sizeof(ab), "AIS range %d NM", (int)s_init_ais_range_nm);
    lv_label_set_text(s4_ais_range_val, ab);
    lv_obj_set_style_text_color(s4_ais_range_val, COL_TEXT, 0);
    lv_obj_set_style_text_font(s4_ais_range_val, &lv_font_montserrat_18, 0);

    /* Wi-Fi hotspot row — mirrors the iOS app's SoftAP switch so the
     * user can flip it directly on the gauge. Reads/writes the same
     * NVS-backed setting through the hook. */
    lv_obj_t* aprow = lv_obj_create(col);
    lv_obj_set_size(aprow, 560, 48);
    lv_obj_set_style_bg_opa(aprow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(aprow, 0, 0);
    lv_obj_set_style_pad_all(aprow, 0, 0);
    lv_obj_remove_flag(aprow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* apl = lv_label_create(aprow);
    lv_label_set_text(apl, "Wi-Fi hotspot");
    lv_obj_set_style_text_color(apl, COL_TEXT, 0);
    lv_obj_set_style_text_font(apl, &lv_font_montserrat_20, 0);
    lv_obj_align(apl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* apsw = lv_switch_create(aprow);
    lv_obj_align(apsw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_init_ap_enabled) lv_obj_add_state(apsw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(apsw, ap_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Editable warning thresholds */
    for (int i = 0; i < THR_COUNT; i++) create_thr_row(col, i);
}

/* Tileview scroll events — freeze updates during swipe transitions.
 *
 * NB: an earlier attempt to snapshot tiles via lv_snapshot_take at
 * SCROLL_BEGIN and swap them in for the live widgets did not help —
 * diagnostics showed snapshot_take of the golden tile alone takes ~365 ms
 * (full uncached render through a different LVGL code path than incremental
 * redraw), longer than the swipe animation itself. By the time the
 * snapshots were ready the swipe was already over. Per-tile rendering cost
 * is the bottleneck, not the buffer pipeline. */
static void tileview_scroll_begin_cb(lv_event_t* e)
{
    (void)e;
    swiping = true;
}

/* Sync map view state (center lat/lon + zoom) FROM `src` TO `dst`. Called
 * on tileview swipe-end so both maps always show the same area — user's
 * ask ("have the feeling it's really the same"). Side effect: shrinks
 * the total unique AIS-viewport area (both maps agree on one bbox) so
 * the app doesn't have to fetch a superset for whichever map is bigger. */
static void sync_map_view(map_renderer_t* src, map_renderer_t* dst)
{
    if (!src || !dst || src == dst) return;
    double lat = map_renderer_get_lat(src);
    double lon = map_renderer_get_lon(src);
    int32_t zoom = map_renderer_get_zoom(src);
    if (map_renderer_get_lat(dst) == lat &&
        map_renderer_get_lon(dst) == lon &&
        map_renderer_get_zoom(dst) == zoom) return;   /* already in sync */
    map_renderer_set_view(dst, lat, lon, zoom);
}

/* Heavy post-swipe work: view sync, pos mirror, viewport re-target,
 * and render. sync_map_view can trigger set_view → load_grid_tiles
 * which memcpys up to 25 × 128 KB (3.2 MB) from tile_cache into the
 * per-map buffers; doing that synchronously in the scroll_end event
 * stalled the LVGL task ~500 ms and the first post-animation frame
 * felt stuck. Deferring here via lv_async_call lets LVGL render at
 * least one clean settled frame before the reload runs. */
static void scroll_end_deferred_cb(void *arg)
{
    int prev_screen = (int)(intptr_t)arg;
    map_renderer_t* gs = golden_screen_get_map();
    bool prev_was_map = (prev_screen == SCREEN_GOLDEN || prev_screen == SCREEN_CHART);
    bool now_is_map   = (current_screen == SCREEN_GOLDEN || current_screen == SCREEN_CHART);
    if (prev_was_map && now_is_map && prev_screen != current_screen) {
        map_renderer_t* src = (prev_screen == SCREEN_GOLDEN) ? gs      : s2_map;
        map_renderer_t* dst = (current_screen == SCREEN_GOLDEN) ? gs   : s2_map;
        sync_map_view(src, dst);
        map_renderer_mirror_pos(dst, src);
    }
    /* Push the currently-visible map's bbox to the ais_store so the
     * incoming feed gets filtered to what the user is actually looking
     * at. Runs on every scroll_end, even when the user swipes back to
     * the same-view state (cheap, only prunes if bbox changed). */
    {
        map_renderer_t* active = NULL;
        if      (current_screen == SCREEN_GOLDEN && gs)     active = gs;
        else if (current_screen == SCREEN_CHART  && s2_map) active = s2_map;
        if (active) {
            map_viewport_bbox_t bb;
            map_renderer_get_viewport_bbox(active, &bb);
#ifdef ESP_PLATFORM
            ais_store_set_viewport(bb.lat_min, bb.lat_max,
                                   bb.lon_min, bb.lon_max);
#else
            (void)bb;
#endif
        }
    }
    if (&n2k_viewport_set_map) {
        map_renderer_t* vp_src = NULL;
        if      (current_screen == SCREEN_CHART  && s2_map) vp_src = s2_map;
        else if (current_screen == SCREEN_GOLDEN && gs)     vp_src = gs;
        if (vp_src) n2k_viewport_set_map((struct map_renderer *)vp_src);
    }
    /* Only render the freshly-visible map — the other one isn't on
     * screen and can pick up cache-arrived tiles the next time it
     * becomes visible via this same async path. */
    if (current_screen == SCREEN_GOLDEN && gs) map_renderer_render(gs);
    if (current_screen == SCREEN_CHART  && s2_map) map_renderer_render(s2_map);
}

static void tileview_scroll_end_cb(lv_event_t* e)
{
    (void)e;
    swiping = false;
    int32_t prev_screen = current_screen;
    /* Update current_screen from the tileview's active tile */
    lv_obj_t* active = lv_tileview_get_tile_active(tileview);
    if (active) {
        int32_t col = lv_obj_get_x(active) / DISP_SIZE;
        if (col >= 0 && col < NUM_SCREENS) current_screen = col;
    }
    /* Defer the heavy work so the settled screen paints first — see
     * comment on scroll_end_deferred_cb. */
    lv_async_call(scroll_end_deferred_cb, (void *)(intptr_t)prev_screen);
}

/* Tile-ready timer: polls for background tile loads, re-renders maps */
static void tile_ready_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    if (swiping || !g_tile_cache || !tile_cache_check_ready(g_tile_cache)) return;
    if (current_screen == SCREEN_GOLDEN) {
        map_renderer_t* gs_map = golden_screen_get_map();
        if (gs_map) map_renderer_render(gs_map);
    }
    if (current_screen == SCREEN_CHART && s2_map) map_renderer_render(s2_map);
}

/* ── Public API ── */

void screen_manager_set_tile_path(const char* path)
{
    tile_path = path;
    golden_screen_set_tile_path(path);
}

void screen_manager_set_alt_tile_path(const char* path)
{
    alt_tile_path = path;
    golden_screen_set_alt_tile_path(path);
}

void screen_manager_create(void)
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* "Circular" container — the physical panel is round, so anything in the
     * 800×800 framebuffer outside the inscribed circle is invisible. On the
     * hardware target we skip the per-pixel clip_corner mask entirely: that
     * mask runs over every rendered pixel, and on a round display it's pure
     * wasted bandwidth. The sim still gets the visual circle for dev. */
    lv_obj_t* circle = lv_obj_create(scr);
    lv_obj_set_size(circle, DISP_SIZE, DISP_SIZE);
    lv_obj_center(circle);
#ifndef ESP_PLATFORM
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(circle, true, 0);
#endif
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

    /* Detect swipe transitions to freeze updates */
    lv_obj_add_event_cb(tileview, tileview_scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(tileview, tileview_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    if (!s_thr_set) warn_thresholds_defaults(&s_thr_vals);

    /* Tile order: [settings] [golden] [chart] [engine] */
    lv_obj_t* tset = lv_tileview_add_tile(tileview, SCREEN_SETTINGS, 0, LV_DIR_RIGHT);
    create_screen4(tset);

    lv_obj_t* tg = lv_tileview_add_tile(tileview, SCREEN_GOLDEN, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    golden_screen_create(tg);

    lv_obj_t* tc = lv_tileview_add_tile(tileview, SCREEN_CHART, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    create_screen2(tc);

    lv_obj_t* tais = lv_tileview_add_tile(tileview, SCREEN_AIS, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
#ifdef ESP_PLATFORM
    /* The AIS list reads from ais_store, which is a firmware-only component
     * (needs FreeRTOS + esp_timer). Sim keeps an empty AIS tile so tile
     * ordering + swipe navigation stay consistent between hw and sim. */
    ais_screen_create(tais);
#else
    /* Placeholder so the tile isn't just a black square — makes it obvious
     * we haven't crashed, and names the reason it's empty. */
    lv_obj_set_style_bg_color(tais, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_bg_opa(tais, LV_OPA_COVER, 0);
    lv_obj_t* ais_stub = lv_label_create(tais);
    lv_label_set_text(ais_stub,
        "AIS\n\n(firmware-only —\nno store on sim)");
    lv_obj_set_style_text_align(ais_stub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ais_stub, lv_color_hex(0x8b949e), 0);
    lv_obj_set_style_text_font(ais_stub, &lv_font_montserrat_20, 0);
    lv_obj_center(ais_stub);
#endif

    lv_obj_t* tmed = lv_tileview_add_tile(tileview, SCREEN_MEDIA, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    media_screen_create(tmed);

    lv_obj_t* te = lv_tileview_add_tile(tileview, SCREEN_ENGINE, 0, LV_DIR_LEFT);
    create_screen3(te);

    /* Warning overlay — sibling of tileview, inside circle mask, on top of everything */
    warning_overlay_init(circle);
    warning_overlay_set_thresholds(&s_thr_vals);   /* apply persisted thresholds */

#if SCREEN_DEBUG_TICK
    golden_screen_show_tick_counter(circle);
#endif

    /* DEMO badge — sibling on top, hidden until screen_manager_set_demo(true) */
    demo_badge = lv_label_create(circle);
    lv_label_set_text(demo_badge, "DEMO");
    lv_obj_set_style_text_color(demo_badge, lv_color_hex(0xffb300), 0);
    lv_obj_set_style_text_font(demo_badge, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(demo_badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(demo_badge, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(demo_badge, 8, 0);
    lv_obj_set_style_pad_ver(demo_badge, 2, 0);
    lv_obj_set_style_radius(demo_badge, 8, 0);
    lv_obj_align(demo_badge, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(demo_badge, LV_OBJ_FLAG_HIDDEN);

    /* Create shared tile cache and attach to map renderers */
    if (tile_path) {
        g_tile_cache = tile_cache_create(64);  /* 64 tiles × 128 KB = 8 MB */
        if (g_tile_cache) {
            /* Restore the last persisted position (NVS) so we open near
             * where the boat was last powered down. Falls back to the
             * Krk fairway default if NVS is empty (first boot).
             * Sim build skips the NVS lookup — no persistence target. */
            double init_lat = 45.00, init_lon = 14.61;
            int32_t init_zoom = 13;
#ifdef ESP_PLATFORM
            last_position_t lp;
            if (last_position_load(&lp)) {
                init_lat  = lp.lat;
                init_lon  = lp.lon;
                init_zoom = lp.zoom;
            }
#endif

            map_renderer_t* gs_map = golden_screen_get_map();
            /* Attach cache, then set the view — tiles are requested via the
             * background loader (async), so the UI appears before they arrive. */
            if (gs_map) {
                map_renderer_set_cache(gs_map, g_tile_cache);
                map_renderer_set_view(gs_map, init_lat, init_lon, init_zoom);
            }
            if (s2_map) {
                map_renderer_set_cache(s2_map, g_tile_cache);
                map_renderer_set_view(s2_map, init_lat, init_lon, init_zoom);
            }
            tile_cache_start_loader(g_tile_cache);
            lv_timer_create(tile_ready_timer_cb, 500, NULL);  /* 2 Hz poll */
        }
    }

    /* Start on the golden/home screen (settings is the tile to its left). */
    lv_tileview_set_tile_by_index(tileview, SCREEN_GOLDEN, 0, LV_ANIM_OFF);
    current_screen = SCREEN_GOLDEN;
}

void screen_manager_set_demo(bool demo)
{
    if (!demo_badge) return;
    if (demo) lv_obj_remove_flag(demo_badge, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(demo_badge, LV_OBJ_FLAG_HIDDEN);
}

void screen_manager_set_hooks(const screen_hooks_t* hooks, uint8_t init_brightness, bool init_demo)
{
    if (hooks) s_hooks = *hooks;
    s_init_brightness = init_brightness;
    s_init_demo = init_demo;
}

void screen_manager_set_ais_range(uint8_t nm)
{
    if (nm < 5)  nm = 5;
    if (nm > 40) nm = 40;
    s_init_ais_range_nm = nm;
}

void screen_manager_set_ap_enabled(bool enabled)
{
    s_init_ap_enabled = enabled;
}

void screen_manager_set_thresholds(const warn_thresholds_t* t)
{
    if (!t) return;
    s_thr_vals = *t;
    s_thr_set = true;
    for (int i = 0; i < THR_COUNT; i++) {
        if (thr_val_lbl[i]) { char b[24]; fmt_thr(b, sizeof(b), i); lv_label_set_text(thr_val_lbl[i], b); }
    }
}

void screen_manager_update(const gauge_data_t* d)
{
#if SCREEN_DEBUG_TICK
    golden_screen_update_tick();   /* forces a redraw every frame — debug only */
#endif

    /* Freeze all updates during swipe transition */
    if (swiping) return;

    /* Warning overlay — always run: its debounce/timeouts are time-based. */
    warning_overlay_update(d);

    /* Store-driven refreshes MUST run above the gauge_data change-guard
     * below. `d` from the CAN pipeline can be bit-for-bit identical for
     * many seconds (boat idle at the dock, no live GPS, engine off)
     * while BLE-forwarded AIS targets keep arriving in the background.
     * Anything that depends on ais_store rather than d has to tick on
     * its own cadence, otherwise the map goes blank of AIS icons and
     * the AIS list freezes until something else nudges `d`.
     *
     * All three timers throttle to their own rates so this stays cheap:
     * 2 s for the two map refreshes (matches the pre-guard cadence),
     * 1 s for the list. */
#ifdef ESP_PLATFORM
    uint32_t now_ms_ais = (uint32_t)(lv_tick_get());

    if (current_screen == SCREEN_AIS) {
        static uint32_t s_ais_last_ms = 0;
        if (now_ms_ais - s_ais_last_ms >= 1000) {
            s_ais_last_ms = now_ms_ais;
            ais_screen_update(d, now_ms_ais);
        }
    }

    /* Chart map's AIS overlay — every 2 s while visible, but frozen
     * while the user is swiping between tiles OR dragging the chart.
     * refresh_ais snapshots the store and repaints every visible
     * widget; running it mid-gesture stutters the drag / swipe for no
     * benefit (reposition_ais_only inside apply_pan_offset already
     * keeps icons attached to the map during pan). */
    if (current_screen == SCREEN_CHART && s2_map &&
        !swiping && !map_renderer_is_panning(s2_map)) {
        static uint32_t s_chart_ais_last = 0;
        if (now_ms_ais - s_chart_ais_last >= 2000) {
            s_chart_ais_last = now_ms_ais;
            map_renderer_refresh_ais(s2_map);
        }
    }

    /* Golden inset map's AIS overlay — same cadence + same freeze. */
    if (current_screen == SCREEN_GOLDEN && !swiping) {
        map_renderer_t* gs = golden_screen_get_map();
        if (gs && !map_renderer_is_panning(gs)) {
            static uint32_t s_gold_ais_last = 0;
            if (now_ms_ais - s_gold_ais_last >= 2000) {
                s_gold_ais_last = now_ms_ais;
                map_renderer_refresh_ais(gs);
            }
        }
    }
#endif

    /* Skip all value/widget updates when nothing relevant changed, so a static
     * screen (e.g. no CAN data, all "---") does no rendering and idles the CPU.
     * Ignore the churning last_update_ms timestamp; force a refresh when the
     * active screen changes (a swipe lands on a new tile).
     *
     * Catch: golden_screen_update staggers slow widgets across a 5-phase
     * `slow_slot % 5` cycle (nav / oil / clt+lam / cltp+batt / bottom rdouts).
     * If we early-return as soon as data goes static, only the phase that
     * happened to be running at that instant lands; the rest stay frozen at
     * their previous values. So whenever data changes, schedule a 5-frame
     * refresh window — enough to cover every phase once — before the guard
     * can kick back in. */
    static gauge_data_t prev;
    static bool have_prev = false;
    static int32_t last_render_screen = -1;
    static uint8_t  force_refresh = 0;
    gauge_data_t cur = *d;
    cur.last_update_ms = 0;
    bool changed = !have_prev || current_screen != last_render_screen ||
                   memcmp(&cur, &prev, sizeof(cur)) != 0;
    if (changed) force_refresh = 5;
    if (!changed && force_refresh == 0) {
        return;
    }
    if (force_refresh > 0) force_refresh--;
    prev = cur;
    have_prev = true;
    last_render_screen = current_screen;

    /* Golden gauge (comet + map + pods) — only when visible */
    if (current_screen == SCREEN_GOLDEN)
        golden_screen_update(d);

    /* Full-size map + SOG/COG overlay. NVS position persistence lives
     * here (and mirrored from the golden inset in golden_screen_update)
     * so pan history survives a reboot regardless of which map screen
     * the user last touched. */
    if (current_screen == SCREEN_CHART) {
        static uint32_t s2_map_frame = 0;
        if (++s2_map_frame >= 40) {
            s2_map_frame = 0;
            if (s2_map && d->latitude != 0.0 && d->longitude != 0.0) {
                map_renderer_set_position(s2_map, d->latitude, d->longitude, d->cog_degrees);
                map_renderer_set_own_sog(s2_map, d->sog_knots);
#ifdef ESP_PLATFORM
                last_position_maybe_save(d->latitude, d->longitude,
                                         d->cog_degrees,
                                         map_renderer_get_zoom(s2_map));
#endif
            }
            /* refresh_ais moved above the change-guard — see the comment
             * up there. Leaving a redundant call here would just cost an
             * extra store snapshot + widget invalidation cycle. */
        }
        static uint32_t s2_label_frame = 0;
        if (++s2_label_frame >= 5) {
            s2_label_frame = 0;
            set_val(s2_sog_label, d->valid.cogsog, "%.1f kn", d->sog_knots, "-.- kn");
            set_val(s2_cog_label, d->valid.cogsog, "%.0f°", d->cog_degrees, "---°");
        }
    }

    /* Engine detail labels */
    /* Media (Fusion) — refresh once per screen update tick; the
     * screen's own rev-guard makes it a no-op when nothing changed. */
    if (current_screen == SCREEN_MEDIA) {
        media_screen_update();
    }

    if (current_screen == SCREEN_ENGINE) {
        static uint32_t s3_frame = 0;
        if (++s3_frame < 2) goto s3_skip;  /* ~10Hz at 20Hz input */
        s3_frame = 0;

        set_val(s3_rpm_label,    d->valid.engine_rapid, "%.0f RPM",  d->rpm, "--- RPM");
        set_val(s3_oilt_label,   d->valid.engine_dyn,   "%.0f°C",    d->oil_temp_c, "---°C");
        set_val(s3_oilp_label,   d->valid.engine_dyn,   "%.1f bar",  d->oil_pressure_kpa / 100.0, "-.- bar");
        set_val(s3_clt_label,    d->valid.engine_dyn,   "%.0f°C",    d->coolant_temp_c, "---°C");
        set_val(s3_iat_label,    d->valid.iat,          "%.0f°C",    d->iat_c, "---°C");
        set_val(s3_lambda1_label, d->valid.lambda1,     "%.3f",      d->lambda1, "-.---");
        set_val(s3_lambda2_label, d->valid.lambda2,     "%.3f",      d->lambda2, "-.---");
        set_val(s3_map_label,    d->valid.engine_rapid, "%.0f kPa",  d->map_kpa, "--- kPa");
        set_val(s3_fp_label,     d->valid.engine_dyn,   "%.0f kPa",  d->fuel_pressure_kpa, "--- kPa");
        set_val(s3_batt_label,   d->valid.engine_dyn,   "%.1fV",     d->battery_voltage, "-.-V");
        set_val(s3_fuel_label,   d->valid.engine_dyn,   "%.1f l/h",  d->fuel_rate_lph, "-.- l/h");
        set_val(s3_cltp_label,   d->valid.engine_dyn,   "%.0f kPa",  d->coolant_pressure_kpa, "--- kPa");
        set_val(s3_hours_label,  d->valid.engine_dyn,   "%.0fh",     d->engine_hours_s / 3600.0, "---h");
        s3_skip:;
    }
}

void screen_manager_next(void)
{
    if (current_screen < NUM_SCREENS - 1) {
        /* Don't update current_screen here — scroll_end callback handles it.
         * This prevents the destination screen from rendering during the animation. */
        swiping = true;
        lv_tileview_set_tile_by_index(tileview, current_screen + 1, 0, LV_ANIM_ON);
    }
}

void screen_manager_prev(void)
{
    if (current_screen > 0) {
        swiping = true;
        lv_tileview_set_tile_by_index(tileview, current_screen - 1, 0, LV_ANIM_ON);
    }
}

map_renderer_t* screen_manager_get_active_map(void)
{
    switch (current_screen) {
        case SCREEN_GOLDEN: return golden_screen_get_map();
        case SCREEN_CHART:  return s2_map;
        case SCREEN_AIS:    return NULL;
        default: return NULL;
    }
}

map_renderer_t* screen_manager_get_chart_map(void) { return s2_map; }

void screen_manager_show_chart(void)
{
    if (!tileview) return;
    swiping = true;
    lv_tileview_set_tile_by_index(tileview, SCREEN_CHART, 0, LV_ANIM_ON);
}
