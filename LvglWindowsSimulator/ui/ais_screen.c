#include "ais_screen.h"
#include "ais_store.h"
#include "screen_manager.h"
#include "map_renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Palette — kept in sync with screen_manager.c so the tile blends in
 * with the rest of the round display. */
#define COL_BG        lv_color_hex(0x0d1117)
#define COL_TEXT      lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM  lv_color_hex(0x8b949e)
#define COL_BADGE_BLE lv_color_hex(0x1f6feb)   /* blue = BLE bridge */
#define COL_BADGE_N2K lv_color_hex(0x2ea043)   /* green = CAN bus */
#define COL_BADGE_UNK lv_color_hex(0x6e7681)

/* One row = five children we keep handles to so update() can just relabel
 * without rebuilding the tree. NAME + BADGE + AGE + RANGE. `mmsi` is
 * kept per row so the click handler can look the target up in the store
 * to jump to it on the chart. */
typedef struct {
    lv_obj_t* row;
    lv_obj_t* name_lbl;
    lv_obj_t* src_lbl;      /* small pill: BLE / N2K / — */
    lv_obj_t* age_lbl;      /* "5s" / "2m" / "10m+" */
    lv_obj_t* range_lbl;    /* "1.4 NM" / "--" */
    uint32_t  mmsi;         /* which target this row currently shows */
} ais_row_t;

/* Rows kept in the widget tree. Rows are built lazily on first use, so
 * the cap only bounds how many can *eventually* exist, not how many are
 * created at boot. 40 covers a busy shipping-lane snapshot without
 * making the user scroll past dozens of empty slots; if the store
 * snapshot has more entries than that, extras are silently dropped from
 * the list (they still contribute to the summary counts up top).
 * Store caps are AIS_MAX_VESSELS=64 + AIS_MAX_ATONS=16 = 80 upper. */
#define MAX_ROWS 40
static ais_row_t s_rows[MAX_ROWS];
static int       s_row_count = 0;

static lv_obj_t* s_title_lbl = NULL;
static lv_obj_t* s_summary_lbl = NULL;
static lv_obj_t* s_list = NULL;
static lv_obj_t* s_empty_lbl = NULL;

/* Own position/SOG snapshot from the last ais_screen_update call, so the
 * detail modal (which is triggered asynchronously from a click event) can
 * compute BRG / range / CPA even though it doesn't get a fresh gauge_data.
 * NAN when not available. */
static double s_own_lat = NAN;
static double s_own_lon = NAN;
static float  s_own_sog = NAN;
static float  s_own_cog = NAN;
static uint32_t s_last_now_ms = 0;

/* Detail modal — shown when the user taps a row. Same info as the
 * chart's map-tap card, plus a "Show on chart" button that also opens
 * the map's card (when a map is available). */
static lv_obj_t* s_detail = NULL;
static lv_obj_t* s_detail_name = NULL;
static lv_obj_t* s_detail_line1 = NULL;
static lv_obj_t* s_detail_line2 = NULL;
static lv_obj_t* s_detail_line3 = NULL;
static lv_obj_t* s_detail_line4 = NULL;
static lv_obj_t* s_detail_show_btn = NULL;
static lv_obj_t* s_detail_parent = NULL;   /* tile handle, needed for lazy build */
static uint32_t  s_detail_mmsi = 0;

/* Compass bearing (0..360, clockwise from N) from A to B on the local
 * tangent plane. NAN when either lat/lon is unknown. Cheap; the map's
 * detail card uses the same approximation. */
static float bearing_from(double alat, double alon, double blat, double blon)
{
    if (isnan(alat) || isnan(alon) || isnan(blat) || isnan(blon)) return NAN;
    double mid = (alat + blat) * 0.5 * M_PI / 180.0;
    double dy = (blat - alat) * 110574.0;
    double dx = (blon - alon) * 111320.0 * cos(mid);
    if (fabs(dx) < 1e-6 && fabs(dy) < 1e-6) return NAN;
    double brg = atan2(dx, dy) * 180.0 / M_PI;
    if (brg < 0) brg += 360.0;
    return (float)brg;
}

/* Closest Point of Approach + Time to CPA between own boat and a
 * moving target, mirroring the map-side compute_cpa_tcpa but taking
 * own state as explicit args so we're not tied to a map_renderer.
 * Returns false when it can't answer (no own fix, both stationary,
 * etc.); *cpa_nm becomes current range if targets are diverging. */
static bool cpa_tcpa(double own_lat, double own_lon,
                     float own_sog, float own_cog,
                     double tlat, double tlon,
                     float tsog, float tcog,
                     float* cpa_nm, float* tcpa_min)
{
    if (isnan(own_lat) || isnan(own_lon)) return false;
    if (isnan(own_sog) || isnan(own_cog)) return false;
    if (own_sog < 0.1f && tsog < 0.1f) return false;

    double mid = own_lat * M_PI / 180.0;
    double dx = (tlon - own_lon) * 111320.0 * cos(mid);
    double dy = (tlat - own_lat) * 110574.0;

    double os = own_sog * 0.51444, or_ = own_cog * M_PI / 180.0;
    double ovx = os * sin(or_), ovy = os * cos(or_);
    double ts = tsog * 0.51444,  tr = tcog * M_PI / 180.0;
    double tvx = ts * sin(tr),   tvy = ts * cos(tr);

    double rvx = tvx - ovx, rvy = tvy - ovy;
    double v2 = rvx * rvx + rvy * rvy;
    double cur = sqrt(dx * dx + dy * dy);

    if (v2 < 1e-6) { *cpa_nm = (float)(cur / 1852.0); *tcpa_min = 0.0f; return true; }
    double t_s = -(dx * rvx + dy * rvy) / v2;
    if (t_s < 0.0) {
        *cpa_nm = (float)(cur / 1852.0);
        *tcpa_min = (float)(t_s / 60.0);
        return true;
    }
    double cx = dx + rvx * t_s;
    double cy = dy + rvy * t_s;
    *cpa_nm = (float)(sqrt(cx * cx + cy * cy) / 1852.0);
    *tcpa_min = (float)(t_s / 60.0);
    return true;
}

/* Half-vincenty on the local tangent plane is plenty for the ranges we
 * show (worst case ~40 NM); avoids the cost + linkage of a real great
 * circle. Result in nautical miles. */
static float haversine_nm(double lat1, double lon1, double lat2, double lon2)
{
    if (isnan(lat1) || isnan(lon1) || isnan(lat2) || isnan(lon2)) return NAN;
    const double R_NM = 3440.065;   /* Earth radius in NM */
    double p1 = lat1 * M_PI / 180.0;
    double p2 = lat2 * M_PI / 180.0;
    double dp = (lat2 - lat1) * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (float)(R_NM * c);
}

static void fmt_age(char* out, size_t cap, uint32_t age_ms)
{
    if (age_ms < 60u * 1000u) {
        snprintf(out, cap, "%us", (unsigned)(age_ms / 1000u));
    } else if (age_ms < 10u * 60u * 1000u) {
        snprintf(out, cap, "%um", (unsigned)(age_ms / 60000u));
    } else {
        snprintf(out, cap, "10m+");
    }
}

static void fmt_range(char* out, size_t cap, float nm)
{
    if (isnan(nm)) { snprintf(out, cap, "--"); return; }
    if (nm < 10.0f) snprintf(out, cap, "%.1f NM", nm);
    else            snprintf(out, cap, "%.0f NM", nm);
}

/* One "BLE" / "N2K" / "—" pill. Keep the label short so the row fits at
 * the 20-px font we use for the rest of the UI. */
static void set_src_pill(lv_obj_t* lbl, ais_source_t src)
{
    lv_color_t c;
    const char* text;
    switch (src) {
        case AIS_SOURCE_BLE: c = COL_BADGE_BLE; text = "BLE"; break;
        case AIS_SOURCE_N2K: c = COL_BADGE_N2K; text = "N2K"; break;
        default:             c = COL_BADGE_UNK; text = "—";   break;
    }
    lv_label_set_text(lbl, text);
    lv_obj_set_style_bg_color(lbl, c, 0);
}

static void build_detail(void);
static void populate_detail(uint32_t mmsi);
static void show_detail(void);
static void hide_detail(void);

/* Row click → open the detail modal on the AIS tile. The modal shows the
 * same data as the chart's tap card and works regardless of whether SD /
 * the map is up; a "Show on chart" button inside the modal takes the
 * user to the map (and enables its own card there) when a map exists. */
static void row_click_cb(lv_event_t* e)
{
    ais_row_t* r = (ais_row_t*)lv_event_get_user_data(e);
    if (!r || !r->mmsi) return;
    s_detail_mmsi = r->mmsi;
    populate_detail(r->mmsi);
    show_detail();
}

/* Close button on the modal. */
static void detail_close_cb(lv_event_t* e)
{
    (void)e;
    hide_detail();
}

/* "Show on chart" — switch to the chart tile and open its tap card
 * on the same target. If there's no map (no SD → tile_path was NULL),
 * this still triggers the tile switch; the map itself is a "no map
 * data" placeholder and focus_mmsi is a no-op. */
static void detail_show_chart_cb(lv_event_t* e)
{
    (void)e;
    uint32_t mmsi = s_detail_mmsi;
    hide_detail();
    screen_manager_show_chart();
    map_renderer_t* mr = screen_manager_get_chart_map();
    if (mr) map_renderer_focus_mmsi(mr, mmsi);
}

/* Build one row on demand; visibility is toggled on/off in update() based
 * on how many entries the ais_store snapshot actually contains. The parent
 * list uses flex column layout so the scrollable region grows naturally
 * with children — necessary for the AIS tile to actually scroll when the
 * user has more targets than fit in the viewport. Batch cost is fine
 * because we build rows lazily, one per store entry per frame at most. */
static void build_row(int i, lv_obj_t* parent)
{
    (void)i;
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, 560, 44);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, &s_rows[i]);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, "");
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 260);
    lv_obj_set_style_text_color(name, COL_TEXT, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* src = lv_label_create(row);
    lv_label_set_text(src, "—");
    lv_obj_set_style_text_color(src, lv_color_white(), 0);
    lv_obj_set_style_text_font(src, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(src, COL_BADGE_UNK, 0);
    lv_obj_set_style_bg_opa(src, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(src, 6, 0);
    lv_obj_set_style_pad_hor(src, 6, 0);
    lv_obj_set_style_pad_ver(src, 2, 0);
    lv_obj_align(src, LV_ALIGN_LEFT_MID, 270, 0);

    lv_obj_t* age = lv_label_create(row);
    lv_label_set_text(age, "");
    lv_obj_set_style_text_color(age, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(age, &lv_font_montserrat_18, 0);
    lv_obj_align(age, LV_ALIGN_RIGHT_MID, -100, 0);

    lv_obj_t* range = lv_label_create(row);
    lv_label_set_text(range, "");
    lv_obj_set_style_text_color(range, COL_TEXT, 0);
    lv_obj_set_style_text_font(range, &lv_font_montserrat_18, 0);
    lv_obj_align(range, LV_ALIGN_RIGHT_MID, 0, 0);

    s_rows[i].row       = row;
    s_rows[i].name_lbl  = name;
    s_rows[i].src_lbl   = src;
    s_rows[i].age_lbl   = age;
    s_rows[i].range_lbl = range;
}

/* Returns true if row `i` is available for use (either was already built
 * or was successfully built now). Cheap after the first hit for that
 * index. */
static bool ensure_row(int i)
{
    if (i < 0 || i >= MAX_ROWS) return false;
    if (s_rows[i].row) return true;
    if (!s_list) return false;
    build_row(i, s_list);
    return s_rows[i].row != NULL;
}

void ais_screen_create(lv_obj_t* tile)
{
    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* 620x600 keeps rows out of the round-corner cutoff, same as
     * create_screen4. */
    lv_obj_t* col = lv_obj_create(tile);
    lv_obj_set_size(col, 620, 600);
    lv_obj_center(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    s_title_lbl = lv_label_create(col);
    lv_label_set_text(s_title_lbl, "AIS TARGETS");
    lv_obj_set_style_text_color(s_title_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_20, 0);

    s_summary_lbl = lv_label_create(col);
    lv_label_set_text(s_summary_lbl, "0 targets");
    lv_obj_set_style_text_color(s_summary_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_summary_lbl, &lv_font_montserrat_18, 0);

    /* Scrollable list. The vertical scroll stays inside the list so
     * the tileview's horizontal swipe is preserved (LV_OBJ_FLAG_SCROLL_CHAIN
     * is cleared). */
    /* Flex column list. LVGL 9's `lv_obj_set_content_height` on a plain
     * container makes the widget taller — it does NOT stretch the scroll
     * region — so a non-flex list with lazily-built rows would only be
     * scrollable up to the last-built row. Flex layout has the scroll
     * region track the actual child bounding box, which is what we want. */
    s_list = lv_obj_create(col);
    lv_obj_set_size(s_list, 600, 500);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_CHAIN);

    /* Rows are built on demand — most users have 0-3 targets in view and
     * paying for 20 rows worth of LVGL widget creation up-front at boot
     * pushed the LVGL task past its 5-s watchdog. See ensure_row(). */
    s_row_count = 0;

    /* Empty-state text lives behind the list, revealed when
     * s_row_count == 0. Simpler than fiddling with layout. */
    s_empty_lbl = lv_label_create(col);
    lv_label_set_text(s_empty_lbl, "No AIS targets yet.\nSend BLE data from the app or\nwait for N2K traffic.");
    lv_obj_set_style_text_color(s_empty_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_empty_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Modal widgets are built lazily on first tap. Stash the tile
     * handle so we can create them under the same parent later. Keeping
     * boot-time widget count small matters — the extra ~10 modal
     * widgets, added to two 25-tile map_renderers + the AIS list, was
     * enough to fragment internal RAM to the point that the LVGL task's
     * 48 KB DRAM stack could no longer be allocated at boot. See
     * commit history for the diagnostic. */
    s_detail_parent = tile;
}

/* Build the modal widget tree on first use. Cheap after that — reuses
 * the same widgets and just re-labels them in populate_detail. */
static void build_detail(void)
{
    if (s_detail || !s_detail_parent) return;

    /* 560 × 380 fits inside the round-display clip mask with margin —
     * the 620 × 420 first cut had its bottom corners clipped by the
     * 400 px circular clip. Corners now sit ~338 px from center
     * (sqrt(280² + 190²)) with 62 px of headroom to the mask edge. */
    s_detail = lv_obj_create(s_detail_parent);
    lv_obj_set_size(s_detail, 560, 380);
    lv_obj_center(s_detail);
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_detail, 14, 0);
    lv_obj_set_style_border_color(s_detail, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_detail, 2, 0);
    lv_obj_set_style_pad_all(s_detail, 18, 0);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    s_detail_name = lv_label_create(s_detail);
    lv_label_set_long_mode(s_detail_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail_name, 460);   /* modal 560 minus pad + close-btn area */
    lv_obj_set_style_text_color(s_detail_name, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_detail_name, &lv_font_montserrat_24, 0);
    lv_obj_align(s_detail_name, LV_ALIGN_TOP_LEFT, 0, 0);

    s_detail_line1 = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_line1, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_detail_line1, &lv_font_montserrat_18, 0);
    lv_obj_align(s_detail_line1, LV_ALIGN_TOP_LEFT, 0, 44);

    s_detail_line2 = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_line2, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_detail_line2, &lv_font_montserrat_20, 0);
    lv_obj_align(s_detail_line2, LV_ALIGN_TOP_LEFT, 0, 92);

    s_detail_line3 = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_line3, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_detail_line3, &lv_font_montserrat_20, 0);
    lv_obj_align(s_detail_line3, LV_ALIGN_TOP_LEFT, 0, 132);

    s_detail_line4 = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_line4, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_detail_line4, &lv_font_montserrat_18, 0);
    lv_obj_align(s_detail_line4, LV_ALIGN_TOP_LEFT, 0, 172);

    lv_obj_t* close_btn = lv_button_create(s_detail);
    lv_obj_set_size(close_btn, 44, 44);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x30363d), 0);
    lv_obj_add_event_cb(close_btn, detail_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    s_detail_show_btn = lv_button_create(s_detail);
    lv_obj_set_size(s_detail_show_btn, 260, 56);
    lv_obj_align(s_detail_show_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_detail_show_btn, detail_show_chart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* show_lbl = lv_label_create(s_detail_show_btn);
    lv_label_set_text(show_lbl, "Show on chart");
    lv_obj_set_style_text_font(show_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(show_lbl);
}

/* ── Detail modal helpers ── */

static void show_detail(void)
{
    build_detail();                 /* no-op after the first call */
    if (!s_detail) return;
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
}

static void hide_detail(void)
{
    if (!s_detail) return;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_detail_mmsi = 0;
}

static void populate_detail(uint32_t mmsi)
{
    build_detail();                 /* first-time lazy build */
    if (!s_detail || !mmsi) return;

    /* Static snapshots — safer than holding a store lock across the
     * label churn below, cheap at 64+16 entries. */
    static ais_vessel_t vessels[AIS_MAX_VESSELS];
    static ais_aton_t   atons[AIS_MAX_ATONS];
    size_t nv = ais_store_get_vessels(vessels, AIS_MAX_VESSELS);
    size_t na = ais_store_get_atons(atons, AIS_MAX_ATONS);

    char buf[96];

    /* Vessel branch */
    for (size_t i = 0; i < nv; i++) {
        const ais_vessel_t* v = &vessels[i];
        if (v->mmsi != mmsi) continue;

        lv_label_set_text(s_detail_name, v->name[0] ? v->name : "(unknown)");

        char age_buf[16];
        uint32_t age = (v->pos_update_ms == 0) ? UINT32_MAX
                                               : (s_last_now_ms - v->pos_update_ms);
        fmt_age(age_buf, sizeof(age_buf), age);
        const char* src = (v->source == AIS_SOURCE_BLE) ? "BLE" :
                          (v->source == AIS_SOURCE_N2K) ? "N2K" : "—";
        snprintf(buf, sizeof(buf), "MMSI %u  •  %s  •  %s  •  %s",
                 (unsigned)v->mmsi, v->is_class_a ? "Class A" : "Class B",
                 src, age_buf);
        lv_label_set_text(s_detail_line1, buf);

        float brg = bearing_from(s_own_lat, s_own_lon, v->lat, v->lon);
        float rng = haversine_nm(s_own_lat, s_own_lon, v->lat, v->lon);
        if (!isnan(brg) && !isnan(rng)) {
            snprintf(buf, sizeof(buf),
                     "SOG %.1f kn   COG %.0f°   BRG %.0f°   D %.2f NM",
                     v->sog_knots, v->cog_deg, brg, rng);
        } else {
            snprintf(buf, sizeof(buf), "SOG %.1f kn   COG %.0f°",
                     v->sog_knots, v->cog_deg);
        }
        lv_label_set_text(s_detail_line2, buf);

        /* "x" is a plain ASCII letter because U+00D7 (×) isn't in
         * montserrat_18 and shows as a tofu box on hardware. Same
         * reason a lot of the other card lines avoid special glyphs. */
        if (!isnan(v->heading_deg) && v->length_m > 0.0f) {
            snprintf(buf, sizeof(buf), "HDG %.0f°   %.0f x %.0f m",
                     v->heading_deg, v->length_m, v->beam_m);
        } else if (!isnan(v->heading_deg)) {
            snprintf(buf, sizeof(buf), "HDG %.0f°", v->heading_deg);
        } else if (v->length_m > 0.0f) {
            snprintf(buf, sizeof(buf), "%.0f x %.0f m", v->length_m, v->beam_m);
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(s_detail_line3, buf);

        float cpa_nm, tcpa_min;
        if (cpa_tcpa(s_own_lat, s_own_lon, s_own_sog, s_own_cog,
                     v->lat, v->lon, v->sog_knots, v->cog_deg,
                     &cpa_nm, &tcpa_min)) {
            if (tcpa_min < 0.0f)
                snprintf(buf, sizeof(buf), "CPA %.2f NM   diverging", cpa_nm);
            else if (tcpa_min < 60.0f)
                snprintf(buf, sizeof(buf), "CPA %.2f NM   TCPA %.0f min", cpa_nm, tcpa_min);
            else
                snprintf(buf, sizeof(buf), "CPA %.2f NM   TCPA %.1f h", cpa_nm, tcpa_min / 60.0f);
        } else {
            snprintf(buf, sizeof(buf), "CPA / TCPA unavailable (need own SOG+COG)");
        }
        lv_label_set_text(s_detail_line4, buf);
        return;
    }

    /* AtoN branch */
    for (size_t i = 0; i < na; i++) {
        const ais_aton_t* a = &atons[i];
        if (a->mmsi != mmsi) continue;

        lv_label_set_text(s_detail_name, a->name[0] ? a->name : "(unnamed AtoN)");

        char age_buf[16];
        uint32_t age = (a->pos_update_ms == 0) ? UINT32_MAX
                                               : (s_last_now_ms - a->pos_update_ms);
        fmt_age(age_buf, sizeof(age_buf), age);
        const char* src = (a->source == AIS_SOURCE_BLE) ? "BLE" :
                          (a->source == AIS_SOURCE_N2K) ? "N2K" : "—";
        snprintf(buf, sizeof(buf), "MMSI %u  •  AtoN%s  •  %s  •  %s",
                 (unsigned)a->mmsi, a->virtual_aton ? " (virtual)" : "", src, age_buf);
        lv_label_set_text(s_detail_line1, buf);

        float brg = bearing_from(s_own_lat, s_own_lon, a->lat, a->lon);
        float rng = haversine_nm(s_own_lat, s_own_lon, a->lat, a->lon);
        if (!isnan(brg) && !isnan(rng))
            snprintf(buf, sizeof(buf), "BRG %.0f°   D %.2f NM", brg, rng);
        else
            snprintf(buf, sizeof(buf), "position unknown");
        lv_label_set_text(s_detail_line2, buf);

        snprintf(buf, sizeof(buf), "AtoN type %u", (unsigned)a->aton_type);
        lv_label_set_text(s_detail_line3, buf);
        lv_label_set_text(s_detail_line4, "");    /* no CPA for AtoN */
        return;
    }

    /* Not found — treat as stale click. */
    hide_detail();
}

/* Only touch labels whose text actually changed. LVGL's label_set_text
 * invalidates the widget and forces a redraw, and this update runs at the
 * full frame rate while the tile is visible. */
static void set_if_changed(lv_obj_t* lbl, const char* text)
{
    const char* cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(lbl, text);
}

void ais_screen_update(const gauge_data_t* d, uint32_t now_ms)
{
    if (!s_list || !d) return;   /* create() hasn't been called yet */

    /* Cache own state so the detail modal (opened later, from a click
     * event, without another d handed in) can still compute BRG / range
     * / CPA. NAN whenever the corresponding gauge_data field isn't
     * valid, which the helpers detect and degrade gracefully. */
    s_own_lat     = d->valid.position ? d->latitude  : NAN;
    s_own_lon     = d->valid.position ? d->longitude : NAN;
    s_own_sog     = d->valid.cogsog   ? d->sog_knots : NAN;
    s_own_cog     = d->valid.cogsog   ? d->cog_degrees : NAN;
    s_last_now_ms = now_ms;

    /* Local aliases so the loops below stay readable. */
    double own_lat = s_own_lat;
    double own_lon = s_own_lon;

    /* If the detail modal is currently visible, keep it fresh — same
     * cadence as the list, so a target's SOG/COG update lands in the
     * open modal without a second tap. */
    if (s_detail && s_detail_mmsi &&
        !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
        populate_detail(s_detail_mmsi);
    }

    /* Snapshot: static-sized arrays kept off the caller's stack. */
    static ais_vessel_t vessels[AIS_MAX_VESSELS];
    static ais_aton_t   atons[AIS_MAX_ATONS];
    size_t nv = ais_store_get_vessels(vessels, AIS_MAX_VESSELS);
    size_t na = ais_store_get_atons(atons, AIS_MAX_ATONS);

    int shown = 0;
    int ble_count = 0, n2k_count = 0;

    /* Vessels first (they usually matter more), then AtoN. Order within
     * each group is snapshot order, which after ais_store_get_* is
     * whatever came out of the packed store — good enough; sorting by
     * range would cost per-frame qsort and thrash the row labels. */
    for (size_t i = 0; i < nv && shown < MAX_ROWS; i++) {
        const ais_vessel_t* v = &vessels[i];
        if (!ensure_row(shown)) break;
        ais_row_t* r = &s_rows[shown];
        r->mmsi = v->mmsi;

        char name[24];
        if (v->name[0]) snprintf(name, sizeof(name), "%s", v->name);
        else            snprintf(name, sizeof(name), "MMSI %u", (unsigned)v->mmsi);
        set_if_changed(r->name_lbl, name);

        set_src_pill(r->src_lbl, v->source);
        if (v->source == AIS_SOURCE_BLE) ble_count++;
        else if (v->source == AIS_SOURCE_N2K) n2k_count++;

        char age_buf[8];
        uint32_t age = (v->pos_update_ms == 0) ? UINT32_MAX
                                               : (now_ms - v->pos_update_ms);
        fmt_age(age_buf, sizeof(age_buf), age);
        set_if_changed(r->age_lbl, age_buf);

        char range_buf[16];
        fmt_range(range_buf, sizeof(range_buf),
                  haversine_nm(own_lat, own_lon, v->lat, v->lon));
        set_if_changed(r->range_lbl, range_buf);

        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        shown++;
    }

    for (size_t i = 0; i < na && shown < MAX_ROWS; i++) {
        const ais_aton_t* a = &atons[i];
        if (!ensure_row(shown)) break;
        ais_row_t* r = &s_rows[shown];
        r->mmsi = a->mmsi;

        /* AtoN names go up to 34 chars per the store — truncate to fit
         * the row buffer; LV_LABEL_LONG_DOT further ellipsises visually
         * if the pixel width overflows. */
        char name[24];
        if (a->name[0]) snprintf(name, sizeof(name), "* %.20s", a->name);
        else            snprintf(name, sizeof(name), "* AtoN %u", (unsigned)a->mmsi);
        set_if_changed(r->name_lbl, name);

        set_src_pill(r->src_lbl, a->source);
        if (a->source == AIS_SOURCE_BLE) ble_count++;
        else if (a->source == AIS_SOURCE_N2K) n2k_count++;

        char age_buf[8];
        uint32_t age = (a->pos_update_ms == 0) ? UINT32_MAX
                                               : (now_ms - a->pos_update_ms);
        fmt_age(age_buf, sizeof(age_buf), age);
        set_if_changed(r->age_lbl, age_buf);

        char range_buf[16];
        fmt_range(range_buf, sizeof(range_buf),
                  haversine_nm(own_lat, own_lon, a->lat, a->lon));
        set_if_changed(r->range_lbl, range_buf);

        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        shown++;
    }

    /* Hide any rows that were populated last frame but aren't this frame.
     * Only touches rows that were actually built (lazy allocation). */
    for (int i = shown; i < s_row_count; i++) {
        if (s_rows[i].row) lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }
    s_row_count = shown;

    /* Summary + empty-state visibility. */
    char sb[48];
    snprintf(sb, sizeof(sb), "%d target%s   %d BLE / %d N2K",
             shown, shown == 1 ? "" : "s", ble_count, n2k_count);
    set_if_changed(s_summary_lbl, sb);
    if (shown == 0) {
        lv_obj_remove_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
}
