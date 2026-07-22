#include "map_renderer.h"
#include "tile_cache.h"
#include "icons/lv_image_telltale_icons.h"
#include "ais_store.h"   /* pure header — safe on sim (types + enums only) */
#ifdef ESP_PLATFORM
  #include "esp_heap_caps.h"
  #include "esp_timer.h"
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Single shared scratch for AIS store snapshots. Used by refresh_ais,
 * reposition_ais_only, focus_mmsi and the click callbacks. All of
 * these only ever run on the LVGL task, so a shared static is safe
 * and stays out of the LVGL task stack (128-vessel array is ~15 KB —
 * too much stack per nested call). Kept in PSRAM: as BSS the
 * 15 KB + 3 KB pair ate enough internal DRAM that the LVGL task's
 * 32 KB stack couldn't be allocated at boot (rc=-1, splash stayed
 * frozen). Lazy-init on first use because map_renderer_create can be
 * called from either map's construction. */
#ifdef ESP_PLATFORM
static ais_vessel_t *s_ais_v_scratch = NULL;
static ais_aton_t   *s_ais_a_scratch = NULL;
static bool ensure_ais_scratch(void)
{
    if (s_ais_v_scratch && s_ais_a_scratch) return true;
    if (!s_ais_v_scratch) {
        s_ais_v_scratch = (ais_vessel_t *)heap_caps_malloc(
            sizeof(ais_vessel_t) * AIS_MAX_VESSELS,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_ais_a_scratch) {
        s_ais_a_scratch = (ais_aton_t *)heap_caps_malloc(
            sizeof(ais_aton_t) * AIS_MAX_ATONS,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_ais_v_scratch && s_ais_a_scratch;
}
#endif

/* Compile-time diagnostic for the AIS blink investigation. When ON,
 * refresh_ais logs slot transitions (new MMSI at slot X, slot Y hidden,
 * slot Z opacity flip) so we can see what's actually churning between
 * refreshes. Rate-limited to keep the serial log survivable — one
 * summary line per refresh + one detail line per transition. Turn OFF
 * once we've caught the pattern. */
#define AIS_BLINK_DEBUG 0
#if AIS_BLINK_DEBUG
  #include "esp_log.h"
  #define AIS_DBG_TAG "ais_dbg"
#endif

/* Pixel buffers on target should live in PSRAM. On P4 internal DRAM is
 * only ~378 KB total and pixel buffers here would happily eat 100+ KB
 * across the two map_renderers (marker + AIS pools + tile grid). Plain
 * malloc() prefers internal DRAM for small allocations and only spills
 * to PSRAM when internal is short — which starves later consumers
 * (mDNS task stack, NimBLE pools) that MUST be in DRAM. Use PSRAM_ALLOC
 * for the pixel bufs so DRAM stays free for things that actually need
 * it. Large tile bufs (128 KB) already fell back to PSRAM by size, but
 * the 6.4 KB AIS icon bufs and 4 KB marker buf were slipping into DRAM
 * per slot. */
#ifdef ESP_PLATFORM
#define PSRAM_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define PSRAM_ALLOC(size) malloc(size)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Map panning uses a custom drag handler (lv_obj_set_pos on the tile widgets).
 * LVGL native scroll was tried but abandoned: lv_obj_create for tiles inside a
 * scrollable container crashes via LVGL layer allocation on both Windows and
 * ESP32. See docs/experiments/ for the parked native-scroll patch. */

#define TILE_PX      256
#define TILE_BYTES   (TILE_PX * TILE_PX * 2)  /* RGB565 */

/* Track-mode "look-ahead": in tracking mode we don't put the own-boat
 * marker at dead-centre of the viewport, we bias the map so the
 * marker lands OPPOSITE to the direction of travel (COG). Result:
 * more chart is always visible ahead of the vessel, regardless of
 * heading. Live-boat report: with the previous fixed north-side
 * anchor, heading south drifted the boat southward into the golden
 * screen's engine-pod row before the deadband triggered. Course-up
 * anchor puts the boat NORTH of centre when heading south, so it
 * drifts back TOWARD centre before it can reach the pods.
 *
 * When COG is unknown or the vessel is essentially stationary
 * (SOG < 0.5 kn) we fall back to dead-centre — no direction-of-travel
 * to bias against.
 *
 * Only affects tracking recentres; set_view / focus_mmsi still put
 * the passed lat/lon at dead-centre. */
#define MARKER_ANCHOR_OFFSET_PX      60
/* SOG below which we treat COG as too noisy to steer the anchor bias.
 * Bumped from 0.5 (drifting) to 1.5 kn — anything you'd get "in gear"
 * clears this cleanly, but GPS-derived COG jitter at sub-kn speeds
 * would make the anchor twitch. */
#define MARKER_ANCHOR_MIN_SOG_KNOTS  1.5f
/* z10 was added to the tile pipeline so users can zoom out further
 * (~4 km / pixel at 45° lat — good for regional context). Anything
 * beyond z16 would be pointless: the tile source stops there and the
 * gauge's 800×800 display has no meaningful use for that detail. */
#define ZOOM_MIN     10
#define ZOOM_MAX     16
#define PATH_BUF_LEN 300
#define GRID_COLS    5
#define GRID_ROWS    5
#define GRID_TILES   (GRID_COLS * GRID_ROWS)
#define GRID_PX_W    (GRID_COLS * TILE_PX)
#define GRID_PX_H    (GRID_ROWS * TILE_PX)
#define MISSING_COLOR 0x0883   /* base: very dark teal */
#define HATCH_COLOR   0x18A6   /* slightly lighter — visible diagonal */
#define ICON_COLOR    0x4208   /* grey for the centered missing-tile icon */
#define MARKER_SIZE  30

/* AIS marker dimensions (px, ARGB8888). Slightly larger than a strict fit
 * so the outline reads at arm's-length; ext_click_area then extends the
 * touch hitbox further without inflating the visible icon. */
#define AIS_VESSEL_PX      40
#define AIS_ATON_PX        30
#define AIS_TOUCH_EXTEND   16   /* +16 px on each side → ~72 px touch zone */
/* Match ais_store's cap. Reverted to 64/16 with the store — see comment
 * on AIS_MAX_VESSELS in ais_store.h. */
#define AIS_VESSEL_POOL 64
#define AIS_ATON_POOL   16
/* AIS color convention — matches tools/tile_viewer.py for visual parity. */
#define AIS_COLOR_A    0xFFFF5050   /* Class A — red */
#define AIS_COLOR_B    0xFF50A0FF   /* Class B — blue */
#define AIS_COLOR_ATON 0xFFFFCC00   /* Yellow diamond */
#define AIS_COLOR_OUTL 0xFF000000   /* Black outline */

/* Vessels haven't broadcast a position in this long → dim the marker
 * (and card) to signal "aging". ais_store still prunes at 10 min via
 * AIS_STALE_AFTER_MS, so the marker vanishes shortly after aging kicks
 * in. Threshold picked to cover Class B at anchor (3 min cadence). */
#define AIS_VESSEL_STALE_MS   (4 * 60 * 1000)

struct map_renderer {
    lv_obj_t*  parent;
    lv_obj_t*  map_container;      /* scrollable viewport */
    lv_obj_t*  map_group;          /* tile grid parent (1280x1280) */
    lv_obj_t*  tile_widgets[GRID_TILES];
    lv_draw_buf_t tile_dbufs[GRID_TILES];
    uint8_t*   tile_bufs[GRID_TILES];
    bool       tile_loaded[GRID_TILES];
    int32_t    grid_origin_tx;     /* tile X coord of top-left grid slot */
    int32_t    grid_origin_ty;     /* tile Y coord of top-left grid slot */

    /* Marker overlay */
    lv_obj_t*  marker_img;
    lv_draw_buf_t marker_dbuf;
    uint8_t*   marker_buf;

    /* AIS overlay: pool of small ARGB widgets, one per visible target. We
     * (re)build the visible-set on each map_renderer_refresh_ais() call by
     * snapshotting the global ais_store. Unused slots are hidden. */
    lv_obj_t*  ais_layer;                   /* parent inside map_group */
    bool       ais_enabled;
    /* pool storage — see AIS_VESSEL_POOL / AIS_ATON_POOL below */
    lv_obj_t*  ais_vessel_imgs[AIS_VESSEL_POOL];
    lv_draw_buf_t ais_vessel_dbufs[AIS_VESSEL_POOL];
    uint8_t*   ais_vessel_bufs[AIS_VESSEL_POOL];
    uint32_t   ais_vessel_mmsis[AIS_VESSEL_POOL];   /* MMSI stored per slot for click lookup */
    lv_obj_t*  ais_aton_imgs[AIS_ATON_POOL];
    lv_draw_buf_t ais_aton_dbufs[AIS_ATON_POOL];
    uint8_t*   ais_aton_bufs[AIS_ATON_POOL];
    uint32_t   ais_aton_mmsis[AIS_ATON_POOL];

    /* Tap-to-inspect info card — created once during map_renderer_create.
     * Populated + shown on AIS click; hidden on chart tap. Content is
     * refreshed automatically on every refresh_ais tick while visible. */
    lv_obj_t*  ais_card;
    lv_obj_t*  ais_card_name;
    lv_obj_t*  ais_card_line2;              /* MMSI + class OR AtoN type */
    lv_obj_t*  ais_card_line3;              /* SOG COG BRG DIST (vessel) — BRG/DIST (AtoN) */
    lv_obj_t*  ais_card_line4;              /* CPA / TCPA (vessels only) */
    uint32_t   ais_selected_mmsi;
    bool       ais_selected_is_aton;        /* true → look up in atons instead */

    /* Small status pill at the top-left of the chart circle showing
     * where AIS data is coming from (N2K bus / phone / none). */
    lv_obj_t*  ais_badge;
    lv_obj_t*  ais_badge_label;

    lv_obj_t*  track_btn;
    int32_t    vp_size;
    char       tile_base[PATH_BUF_LEN];

    double     center_lat, center_lon;
    int32_t    current_zoom;
    double     center_tx, center_ty;

    double     pos_lat, pos_lon;
    float      pos_cog;
    float      pos_sog_knots;                /* own SOG for CPA/TCPA */
    bool       pos_valid;
    bool       tracking;
    /* True between PRESSING (with non-zero movement) and RELEASED /
     * PRESS_LOST — screen_manager reads it to skip refresh_ais while
     * the user is dragging the chart. Every refresh_ais snapshots the
     * store and repaints up to POOL widgets; doing that mid-drag
     * makes the drag stutter for no visible benefit (icons are
     * already being repositioned by reposition_ais_only inside
     * apply_pan_offset). */
    bool       panning;

    /* Last view explicitly set via map_renderer_set_view — used as a
     * recenter fallback by map_renderer_track when there's no GPS fix
     * yet, so the button always does something visible. */
    double     home_lat, home_lon;
    bool       home_valid;

    float      vignette_start;     /* kept for API compat, unused in widget-grid mode */
    uint16_t   vignette_color;

    char       tile_primary[PATH_BUF_LEN];
    char       tile_alt[PATH_BUF_LEN];
    bool       using_alt;
    lv_obj_t*  theme_btn;
    lv_obj_t*  zoom_in_btn;
    lv_obj_t*  zoom_out_btn;

    tile_cache_t* cache;
    uint8_t    path_id;
    uint8_t    path_id_primary;
    uint8_t    path_id_alt;
};

/* ── Tile math ── */

static void lat_lon_to_tile(double lat, double lon, int32_t zoom, double* tx, double* ty)
{
    double n = (double)(1 << zoom);
    *tx = (lon + 180.0) / 360.0 * n;
    double lat_rad = lat * M_PI / 180.0;
    *ty = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;
}

static void tile_to_lat_lon(double tx, double ty, int32_t zoom, double* lat, double* lon)
{
    double n = (double)(1 << zoom);
    *lon = tx / n * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * ty / n))) * 180.0 / M_PI;
}

static void update_tile_coords(map_renderer_t* mr)
{
    lat_lon_to_tile(mr->center_lat, mr->center_lon, mr->current_zoom, &mr->center_tx, &mr->center_ty);
}

/* ── Tile loading ── */

static uint8_t sync_tile_buf[TILE_BYTES];

static const uint8_t* load_tile_sync_raw(map_renderer_t* mr, int32_t tx, int32_t ty, int32_t zoom)
{
    char path[PATH_BUF_LEN + 48];
    snprintf(path, sizeof(path), "%s/%ld/%ld/%ld.bin", mr->tile_base, (long)zoom, (long)tx, (long)ty);

    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK) return NULL;

    uint32_t bytes_read = 0;
    lv_fs_res_t res = lv_fs_read(&f, sync_tile_buf, TILE_BYTES, &bytes_read);
    lv_fs_close(&f);
    return (res == LV_FS_RES_OK && bytes_read == TILE_BYTES) ? sync_tile_buf : NULL;
}

/* Pre-rendered "missing tile" template:
 *   - solid MISSING_COLOR background with diagonal HATCH_COLOR stripes
 *     every 8 px so even partly-loaded grids are visibly distinct from
 *     "still loading"
 *   - tt_icon_tile_missing (image-off) centered in the tile, alpha-blended
 *     onto the hatched bg
 * Rendered once into a static SPIRAM buffer and memcpy'd on every miss. */
static uint16_t* g_missing_template = NULL;

static void build_missing_template(void)
{
    if (g_missing_template) return;
    g_missing_template = (uint16_t*)PSRAM_ALLOC(TILE_BYTES);
    if (!g_missing_template) return;

    /* Background: solid + diagonal hatch (8-px stripes via (x+y)/4 parity). */
    for (int32_t y = 0; y < TILE_PX; y++) {
        for (int32_t x = 0; x < TILE_PX; x++) {
            bool stripe = (((x + y) >> 2) & 1) == 0;
            g_missing_template[y * TILE_PX + x] = stripe ? HATCH_COLOR : MISSING_COLOR;
        }
    }

    /* Center the 48-px image-off icon. Alpha-blend onto the existing bg. */
    const lv_image_dsc_t* icon = &tt_icon_tile_missing;
    int32_t icon_w = (int32_t)icon->header.w;
    int32_t icon_h = (int32_t)icon->header.h;
    int32_t ox = (TILE_PX - icon_w) / 2;
    int32_t oy = (TILE_PX - icon_h) / 2;
    const uint8_t* alpha_src = icon->data;
    uint8_t icon_r = ((ICON_COLOR >> 11) & 0x1F) << 3;
    uint8_t icon_g = ((ICON_COLOR >>  5) & 0x3F) << 2;
    uint8_t icon_b =  (ICON_COLOR        & 0x1F) << 3;
    for (int32_t y = 0; y < icon_h; y++) {
        for (int32_t x = 0; x < icon_w; x++) {
            uint8_t a = alpha_src[y * icon_w + x];
            if (a == 0) continue;
            uint16_t bg = g_missing_template[(oy + y) * TILE_PX + (ox + x)];
            if (a == 255) {
                g_missing_template[(oy + y) * TILE_PX + (ox + x)] = ICON_COLOR;
                continue;
            }
            uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
            uint8_t bg_g = ((bg >>  5) & 0x3F) << 2;
            uint8_t bg_b =  (bg        & 0x1F) << 3;
            uint16_t inv = 255 - a;
            uint8_t r = (bg_r * inv + icon_r * a) / 255;
            uint8_t g = (bg_g * inv + icon_g * a) / 255;
            uint8_t b = (bg_b * inv + icon_b * a) / 255;
            g_missing_template[(oy + y) * TILE_PX + (ox + x)] =
                ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }
}

static void fill_missing(uint8_t* buf)
{
    build_missing_template();
    if (g_missing_template) {
        memcpy(buf, g_missing_template, TILE_BYTES);
    } else {
        /* Fallback if template alloc failed — solid color. */
        uint16_t* px = (uint16_t*)buf;
        for (int32_t i = 0; i < TILE_PX * TILE_PX; i++)
            px[i] = MISSING_COLOR;
    }
}

/* Forward declarations */
static void apply_pan_offset(map_renderer_t* mr);
static void update_marker_position(map_renderer_t* mr);

/* ── Grid tile loading ── */

static void load_grid_tiles(map_renderer_t* mr)
{
    if (!mr->map_container) return;

    for (int32_t row = 0; row < GRID_ROWS; row++) {
        for (int32_t col = 0; col < GRID_COLS; col++) {
            int32_t idx = row * GRID_COLS + col;
            int32_t tx = mr->grid_origin_tx + col;
            int32_t ty = mr->grid_origin_ty + row;

            const uint8_t* td = NULL;
            if (mr->cache) {
                tile_key_t key = { mr->path_id, (uint8_t)mr->current_zoom,
                                   (uint16_t)tx, (uint16_t)ty };
                td = tile_cache_get(mr->cache, key);
                if (!td)
                    tile_cache_request(mr->cache, key, mr->tile_base);
            } else {
                td = load_tile_sync_raw(mr, tx, ty, mr->current_zoom);
            }

            if (td) {
                memcpy(mr->tile_bufs[idx], td, TILE_BYTES);
                mr->tile_loaded[idx] = true;
            } else {
                fill_missing(mr->tile_bufs[idx]);
                mr->tile_loaded[idx] = false;
            }

            lv_draw_buf_invalidate_cache(&mr->tile_dbufs[idx], NULL);
        }
    }

    /* Invalidate to trigger redraw */
    lv_obj_invalidate(mr->map_container);
}

/* ── Scroll ↔ geo coordinate mapping ── */

static void center_scroll_on_view(map_renderer_t* mr)
{
    if (!mr->map_container) return;
    apply_pan_offset(mr);
}

/* ── AIS overlay rendering ──
 *
 * Each AIS target is a small ARGB8888 widget under mr->ais_layer. The pool
 * is allocated once at init_ais_pool() — refresh_ais just (re)draws into
 * the existing buffers and moves the widgets. Stale slots get hidden.
 * The map_group's scroll position handles pan/zoom for free, same way the
 * tile widgets ride along. */

static void argb_set(uint32_t *px, int32_t stride, int32_t w, int32_t h,
                     int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    px[y * stride + x] = color;
}

static void argb_line(uint32_t *px, int32_t w, int32_t h,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      uint32_t color)
{
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        argb_set(px, w, w, h, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* COG-oriented filled triangle for a vessel. Class A red, B blue. Geometry
 * scales with AIS_VESSEL_PX so bumping the size keeps proportions right. */
static void draw_vessel(uint8_t *buf, float heading_deg, uint32_t color)
{
    memset(buf, 0, AIS_VESSEL_PX * AIS_VESSEL_PX * 4);
    uint32_t *px = (uint32_t*)buf;
    int32_t cx = AIS_VESSEL_PX / 2, cy = AIS_VESSEL_PX / 2;
    float rad = (heading_deg - 90.0f) * (float)M_PI / 180.0f;
    float cs = cosf(rad), sn = sinf(rad);
    int32_t tip_r  = AIS_VESSEL_PX * 3 / 8;    /* tip distance from centre */
    int32_t base_r = AIS_VESSEL_PX     / 4;    /* back-of-triangle to centre */
    int32_t half_w = AIS_VESSEL_PX * 3 / 16;   /* half base width */
    int32_t tip_x   = cx + (int32_t)( tip_r * cs);
    int32_t tip_y   = cy + (int32_t)( tip_r * sn);
    int32_t left_x  = cx + (int32_t)(-base_r * cs - (-half_w) * sn);
    int32_t left_y  = cy + (int32_t)(-base_r * sn + (-half_w) * cs);
    int32_t right_x = cx + (int32_t)(-base_r * cs -   half_w  * sn);
    int32_t right_y = cy + (int32_t)(-base_r * sn +   half_w  * cs);
    for (int32_t i = 0; i <= 14; i++) {
        int32_t lx = left_x + (right_x - left_x) * i / 14;
        int32_t ly = left_y + (right_y - left_y) * i / 14;
        argb_line(px, AIS_VESSEL_PX, AIS_VESSEL_PX, lx, ly, tip_x, tip_y, color);
    }
    argb_line(px, AIS_VESSEL_PX, AIS_VESSEL_PX, tip_x, tip_y,   left_x,  left_y,  AIS_COLOR_OUTL);
    argb_line(px, AIS_VESSEL_PX, AIS_VESSEL_PX, left_x, left_y, right_x, right_y, AIS_COLOR_OUTL);
    argb_line(px, AIS_VESSEL_PX, AIS_VESSEL_PX, right_x, right_y, tip_x, tip_y,   AIS_COLOR_OUTL);
}

/* Yellow filled diamond for AtoN. Radius scales with AIS_ATON_PX. */
static void draw_aton(uint8_t *buf)
{
    memset(buf, 0, AIS_ATON_PX * AIS_ATON_PX * 4);
    uint32_t *px = (uint32_t*)buf;
    int32_t cx = AIS_ATON_PX / 2, cy = AIS_ATON_PX / 2;
    int32_t r  = AIS_ATON_PX * 3 / 8;
    /* Scanline-fill the diamond |x| + |y| <= r. */
    for (int32_t y = -r; y <= r; y++) {
        int32_t span = r - abs(y);
        for (int32_t x = -span; x <= span; x++)
            argb_set(px, AIS_ATON_PX, AIS_ATON_PX, AIS_ATON_PX, cx + x, cy + y, AIS_COLOR_ATON);
    }
    /* Outline. */
    argb_line(px, AIS_ATON_PX, AIS_ATON_PX, cx,     cy - r, cx + r, cy,     AIS_COLOR_OUTL);
    argb_line(px, AIS_ATON_PX, AIS_ATON_PX, cx + r, cy,     cx,     cy + r, AIS_COLOR_OUTL);
    argb_line(px, AIS_ATON_PX, AIS_ATON_PX, cx,     cy + r, cx - r, cy,     AIS_COLOR_OUTL);
    argb_line(px, AIS_ATON_PX, AIS_ATON_PX, cx - r, cy,     cx,     cy - r, AIS_COLOR_OUTL);
}

/* Populate + show the info card for a selected vessel/AtoN. */
static void show_ais_card_vessel(map_renderer_t* mr, const ais_vessel_t *v);
static void show_ais_card_aton  (map_renderer_t* mr, const ais_aton_t   *a);
static void hide_ais_card(map_renderer_t* mr);

static void vessel_click_cb(lv_event_t *e)
{
#ifdef ESP_PLATFORM
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    lv_obj_t* target   = lv_event_get_current_target_obj(e);
    /* Which slot? Linear scan is cheap. Reuse the file-scope scratch. */
    if (!s_ais_v_scratch) return;
    ais_vessel_t *vessels = s_ais_v_scratch;
    size_t n_v = ais_store_get_vessels(vessels, AIS_VESSEL_POOL);
    for (size_t i = 0; i < AIS_VESSEL_POOL; i++) {
        if (mr->ais_vessel_imgs[i] != target) continue;
        for (size_t j = 0; j < n_v; j++) {
            if (vessels[j].mmsi == mr->ais_vessel_mmsis[i]) {
                show_ais_card_vessel(mr, &vessels[j]);
                lv_event_stop_bubbling(e);
                return;
            }
        }
        break;
    }
#else
    (void)e;
#endif
}

static void aton_click_cb(lv_event_t *e)
{
#ifdef ESP_PLATFORM
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    lv_obj_t* target   = lv_event_get_current_target_obj(e);
    if (!s_ais_a_scratch) return;
    ais_aton_t *atons = s_ais_a_scratch;
    size_t n_a = ais_store_get_atons(atons, AIS_ATON_POOL);
    for (size_t i = 0; i < AIS_ATON_POOL; i++) {
        if (mr->ais_aton_imgs[i] != target) continue;
        for (size_t j = 0; j < n_a; j++) {
            if (atons[j].mmsi == mr->ais_aton_mmsis[i]) {
                show_ais_card_aton(mr, &atons[j]);
                lv_event_stop_bubbling(e);
                return;
            }
        }
        break;
    }
#else
    (void)e;
#endif
}

/* Create a single AIS widget slot on demand. Buf goes to PSRAM via
 * malloc, dbuf inits over it, then an lv_image is added under map_group.
 * Kept lazy so boot doesn't have to build 80+ widgets before first
 * render — that stalls LVGL 9 hard on this hardware. */
static bool ensure_vessel_slot(map_renderer_t* mr, size_t i)
{
    if (mr->ais_vessel_imgs[i]) return true;
    mr->ais_vessel_bufs[i] = (uint8_t*)PSRAM_ALLOC(AIS_VESSEL_PX * AIS_VESSEL_PX * 4);
    if (!mr->ais_vessel_bufs[i]) return false;
    memset(mr->ais_vessel_bufs[i], 0, AIS_VESSEL_PX * AIS_VESSEL_PX * 4);
    lv_draw_buf_init(&mr->ais_vessel_dbufs[i], AIS_VESSEL_PX, AIS_VESSEL_PX,
        LV_COLOR_FORMAT_ARGB8888, 0,
        mr->ais_vessel_bufs[i], AIS_VESSEL_PX * AIS_VESSEL_PX * 4);
    lv_draw_buf_set_flag(&mr->ais_vessel_dbufs[i], LV_IMAGE_FLAGS_MODIFIABLE);
    mr->ais_vessel_imgs[i] = lv_image_create(mr->map_group);
    lv_image_set_src(mr->ais_vessel_imgs[i], &mr->ais_vessel_dbufs[i]);
    lv_obj_add_flag(mr->ais_vessel_imgs[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mr->ais_vessel_imgs[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(mr->ais_vessel_imgs[i], AIS_TOUCH_EXTEND);
    lv_obj_add_event_cb(mr->ais_vessel_imgs[i], vessel_click_cb, LV_EVENT_CLICKED, mr);
    return true;
}

static bool ensure_aton_slot(map_renderer_t* mr, size_t i)
{
    if (mr->ais_aton_imgs[i]) return true;
    mr->ais_aton_bufs[i] = (uint8_t*)PSRAM_ALLOC(AIS_ATON_PX * AIS_ATON_PX * 4);
    if (!mr->ais_aton_bufs[i]) return false;
    memset(mr->ais_aton_bufs[i], 0, AIS_ATON_PX * AIS_ATON_PX * 4);
    lv_draw_buf_init(&mr->ais_aton_dbufs[i], AIS_ATON_PX, AIS_ATON_PX,
        LV_COLOR_FORMAT_ARGB8888, 0,
        mr->ais_aton_bufs[i], AIS_ATON_PX * AIS_ATON_PX * 4);
    lv_draw_buf_set_flag(&mr->ais_aton_dbufs[i], LV_IMAGE_FLAGS_MODIFIABLE);
    mr->ais_aton_imgs[i] = lv_image_create(mr->map_group);
    lv_image_set_src(mr->ais_aton_imgs[i], &mr->ais_aton_dbufs[i]);
    lv_obj_add_flag(mr->ais_aton_imgs[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mr->ais_aton_imgs[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(mr->ais_aton_imgs[i], AIS_TOUCH_EXTEND);
    lv_obj_add_event_cb(mr->ais_aton_imgs[i], aton_click_cb, LV_EVENT_CLICKED, mr);
    /* AtoN diamond is orientation-invariant — draw once when the slot is born. */
    draw_aton(mr->ais_aton_bufs[i]);
    lv_draw_buf_invalidate_cache(&mr->ais_aton_dbufs[i], NULL);
    return true;
}

/* ── AIS info card ── */

/* Range (NM) from own boat to target using flat-earth approximation
 * — accurate to <1% for the scale we care about (single-digit NM). */
static double range_nm_from_us(map_renderer_t* mr, double tlat, double tlon)
{
    double olat, olon;
    if (mr->pos_valid) { olat = mr->pos_lat;  olon = mr->pos_lon; }
    else if (mr->home_valid) { olat = mr->home_lat; olon = mr->home_lon; }
    else return -1.0;
    double mid = (olat + tlat) * 0.5 * M_PI / 180.0;
    double dy_m = (tlat - olat) * 110574.0;
    double dx_m = (tlon - olon) * 111320.0 * cos(mid);
    return sqrt(dx_m * dx_m + dy_m * dy_m) / 1852.0;
}

/* Closest Point of Approach (CPA) between own boat and a moving target,
 * with the Time to CPA (TCPA). Flat-earth approximation is fine — CPA
 * is a small-scale calculation (single-digit NM, single-digit minutes).
 *
 * Output:
 *   *cpa_nm    minimum future distance in NM (or current distance if
 *              targets are diverging / stationary — TCPA <= 0 case)
 *   *tcpa_min  time until CPA, in minutes; NEGATIVE if targets are
 *              diverging (no future encounter). NAN when the input is
 *              too under-determined to answer (no own fix / no own
 *              SOG / target has neither COG nor SOG).
 *
 * Returns false when computation isn't possible (caller should hide the
 * CPA/TCPA fields on the card). */
static bool compute_cpa_tcpa(map_renderer_t* mr,
                             double tlat, double tlon,
                             float  tsog_knots, float tcog_deg,
                             float *cpa_nm, float *tcpa_min)
{
    if (!mr->pos_valid) return false;
    if (mr->pos_sog_knots < 0.1f && tsog_knots < 0.1f) return false;

    /* Local ENU (east, north) around own position. */
    double mid_rad = mr->pos_lat * M_PI / 180.0;
    double dx_m = (tlon - mr->pos_lon) * 111320.0 * cos(mid_rad);
    double dy_m = (tlat - mr->pos_lat) * 110574.0;

    /* Velocity vectors in m/s. Compass bearing → math angle:
     *   east  = sog * sin(cog),  north = sog * cos(cog) */
    double o_sog_ms = mr->pos_sog_knots * 0.51444;
    double o_rad    = mr->pos_cog * M_PI / 180.0;
    double ovx = o_sog_ms * sin(o_rad);
    double ovy = o_sog_ms * cos(o_rad);

    double t_sog_ms = tsog_knots * 0.51444;
    double t_rad    = tcog_deg * M_PI / 180.0;
    double tvx = t_sog_ms * sin(t_rad);
    double tvy = t_sog_ms * cos(t_rad);

    /* Relative velocity (target minus own). */
    double rvx = tvx - ovx, rvy = tvy - ovy;
    double v2 = rvx * rvx + rvy * rvy;
    double cur_range_m = sqrt(dx_m * dx_m + dy_m * dy_m);

    if (v2 < 1e-6) {
        /* No relative motion — CPA is the current range, TCPA irrelevant. */
        *cpa_nm   = (float)(cur_range_m / 1852.0);
        *tcpa_min = 0.0f;
        return true;
    }

    /* t* = -(r0 · rv) / |rv|²  →  seconds. */
    double t_s = -(dx_m * rvx + dy_m * rvy) / v2;

    if (t_s < 0.0) {
        /* Diverging — negative TCPA. Report current range as CPA. */
        *cpa_nm   = (float)(cur_range_m / 1852.0);
        *tcpa_min = (float)(t_s / 60.0);
        return true;
    }

    /* Closest point along the future relative track. */
    double cx = dx_m + rvx * t_s;
    double cy = dy_m + rvy * t_s;
    *cpa_nm   = (float)(sqrt(cx * cx + cy * cy) / 1852.0);
    *tcpa_min = (float)(t_s / 60.0);
    return true;
}

/* True bearing (0..360°) from own boat to target. */
static float bearing_from_us(map_renderer_t* mr, double tlat, double tlon)
{
    double olat, olon;
    if (mr->pos_valid) { olat = mr->pos_lat;  olon = mr->pos_lon; }
    else if (mr->home_valid) { olat = mr->home_lat; olon = mr->home_lon; }
    else return NAN;
    double o_rad = olat * M_PI / 180.0;
    double t_rad = tlat * M_PI / 180.0;
    double dlon  = (tlon - olon) * M_PI / 180.0;
    double y = sin(dlon) * cos(t_rad);
    double x = cos(o_rad) * sin(t_rad) - sin(o_rad) * cos(t_rad) * cos(dlon);
    double b = atan2(y, x) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    return (float)b;
}


static void card_close_click_cb(lv_event_t *e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    hide_ais_card(mr);
    lv_event_stop_bubbling(e);
}

static void build_ais_card(map_renderer_t* mr)
{
    /* Attach to the screen tile / root (parent of chart_area), NOT inside
     * chart_area. move_foreground on show then reorders us above every
     * sibling widget on this screen — RPM dial, pods, SOG bar — so the
     * card is always visible even on the crowded golden screen. */
    lv_obj_t* chart_area = lv_obj_get_parent(mr->map_container);
    lv_obj_t* root       = lv_obj_get_parent(chart_area);
    lv_obj_t* card = lv_obj_create(root);
    /* Width + vertical offset are sized so BOTH bottom corners land
     * inside the 400-px round physical bezel with margin. On the
     * 800-px full-chart tile the naive 82%-of-viewport sizing pushed
     * card corners to ~451 px from screen centre — outside the bezel,
     * so the user saw the card's bottom-left / bottom-right corners
     * clipped by the panel. On the 600-px golden inset the same
     * proportions still fit because chart_area sits inset 100 px from
     * each side. Branch on viewport size. */
    int32_t w, y_off;
    if (mr->vp_size >= 800) {
        w = 560;         /* corners at (140,600)/(660,600) → 344 px from centre */
        y_off = -200;    /* card bottom at y=600; top at y=482 */
    } else {
        w = (mr->vp_size * 82) / 100;
        y_off = -90;
    }
    lv_obj_set_size(card, w, 118);   /* +22 px for the CPA/TCPA line */
    lv_obj_align_to(card, chart_area, LV_ALIGN_BOTTOM_MID, 0, y_off);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0e1520), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    /* 2-px accent-cyan outline that reads as "AIS info", not "engine pod". */
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00d4ff), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    /* Soft outer shadow lifts it off the underlying widgets. */
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_y(card, 4, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* eat taps so map doesn't get them */
    mr->ais_card = card;

    lv_obj_t* name = lv_label_create(card);
    lv_obj_set_style_text_color(name, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_label_set_text(name, "");
    mr->ais_card_name = name;

    lv_obj_t* l2 = lv_label_create(card);
    lv_obj_set_style_text_color(l2, lv_color_hex(0x8b949e), 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 4, 32);
    lv_label_set_text(l2, "");
    mr->ais_card_line2 = l2;

    lv_obj_t* l3 = lv_label_create(card);
    lv_obj_set_style_text_color(l3, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_text_font(l3, &lv_font_montserrat_20, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 4, 56);
    lv_label_set_text(l3, "");
    mr->ais_card_line3 = l3;

    lv_obj_t* l4 = lv_label_create(card);
    lv_obj_set_style_text_color(l4, lv_color_hex(0xffcc00), 0);   /* alert-yellow */
    lv_obj_set_style_text_font(l4, &lv_font_montserrat_16, 0);
    lv_obj_align(l4, LV_ALIGN_TOP_LEFT, 4, 82);
    lv_label_set_text(l4, "");
    mr->ais_card_line4 = l4;

    /* × close button, top-right */
    lv_obj_t* close = lv_button_create(card);
    lv_obj_set_size(close, 34, 34);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x21262d), 0);
    lv_obj_add_event_cb(close, card_close_click_cb, LV_EVENT_CLICKED, mr);
    lv_obj_t* x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);
    lv_obj_center(x);
}

/* Human-friendly age string: "3 s ago" / "12 min ago" / "1.4 h ago".
 * Writes into `dst` (buffer of at least 24 chars). */
static void format_age(char *dst, size_t cap, uint32_t age_ms)
{
    if (age_ms < 60000) {
        snprintf(dst, cap, "%lu s ago", (unsigned long)(age_ms / 1000));
    } else if (age_ms < 3600000) {
        snprintf(dst, cap, "%lu min ago", (unsigned long)(age_ms / 60000));
    } else {
        snprintf(dst, cap, "%.1f h ago", age_ms / 3600000.0);
    }
}

#ifndef ESP_PLATFORM
/* Sim: no esp_timer / ais_store — stub the card populators. Called sites
 * are all inside ESP_PLATFORM guards; the stubs exist purely so the
 * forward decls at file top resolve. */
static void show_ais_card_vessel(map_renderer_t* mr, const ais_vessel_t *v) { (void)mr; (void)v; }
static void show_ais_card_aton  (map_renderer_t* mr, const ais_aton_t   *a) { (void)mr; (void)a; }
static void refresh_ais_badge(map_renderer_t* mr,
                              const ais_vessel_t *vessels, size_t n_v,
                              const ais_aton_t   *atons,   size_t n_a)
{ (void)mr; (void)vessels; (void)n_v; (void)atons; (void)n_a; }
#else
static void show_ais_card_vessel(map_renderer_t* mr, const ais_vessel_t *v)
{
    if (!mr->ais_card) return;
    mr->ais_selected_mmsi     = v->mmsi;
    mr->ais_selected_is_aton  = false;
    const char *class_str = v->is_class_a ? "Class A" : "Class B";
    lv_label_set_text(mr->ais_card_name,
                      (v->name[0] ? v->name : "(unknown)"));

    char age[24];
    format_age(age, sizeof(age),
               (uint32_t)(esp_timer_get_time() / 1000) - v->pos_update_ms);
    lv_label_set_text_fmt(mr->ais_card_line2, "MMSI %lu  •  %s  •  %s",
                          (unsigned long)v->mmsi, class_str, age);
    double rng = range_nm_from_us(mr, v->lat, v->lon);
    float  brg = bearing_from_us(mr, v->lat, v->lon);
    if (rng >= 0 && !isnan(brg)) {
        lv_label_set_text_fmt(mr->ais_card_line3,
            "SOG %.1f kn   COG %.0f°   BRG %.0f°   D %.2f NM",
            v->sog_knots, v->cog_deg, brg, rng);
    } else {
        lv_label_set_text_fmt(mr->ais_card_line3,
            "SOG %.1f kn   COG %.0f°",
            v->sog_knots, v->cog_deg);
    }

    /* If the target hasn't reported recently, prefer a staleness note over
     * CPA/TCPA — the last-known SOG/COG might be minutes old, so CPA math
     * on those values would be misleading. */
    uint32_t age_ms = (uint32_t)(esp_timer_get_time() / 1000) - v->pos_update_ms;
    if (age_ms > AIS_VESSEL_STALE_MS) {
        lv_label_set_text_fmt(mr->ais_card_line4,
            "STALE   last seen %lu min ago",
            (unsigned long)(age_ms / 60000));
    } else {
        float cpa_nm, tcpa_min;
        if (compute_cpa_tcpa(mr, v->lat, v->lon, v->sog_knots, v->cog_deg,
                             &cpa_nm, &tcpa_min)) {
            if (tcpa_min < 0.0f) {
                lv_label_set_text_fmt(mr->ais_card_line4,
                    "CPA %.2f NM   diverging", cpa_nm);
            } else if (tcpa_min < 60.0f) {
                lv_label_set_text_fmt(mr->ais_card_line4,
                    "CPA %.2f NM   TCPA %.0f min", cpa_nm, tcpa_min);
            } else {
                lv_label_set_text_fmt(mr->ais_card_line4,
                    "CPA %.2f NM   TCPA %.1f h", cpa_nm, tcpa_min / 60.0f);
            }
        } else {
            lv_label_set_text(mr->ais_card_line4, "");
        }
    }

    lv_obj_remove_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mr->ais_card);   /* above sibling pods/dial */
}

static void show_ais_card_aton(map_renderer_t* mr, const ais_aton_t *a)
{
    if (!mr->ais_card) return;
    mr->ais_selected_mmsi     = a->mmsi;
    mr->ais_selected_is_aton  = true;
    lv_label_set_text(mr->ais_card_name,
                      (a->name[0] ? a->name : "(unnamed)"));

    char age[24];
    format_age(age, sizeof(age),
               (uint32_t)(esp_timer_get_time() / 1000) - a->pos_update_ms);
    lv_label_set_text_fmt(mr->ais_card_line2, "MMSI %lu  •  AtoN%s  •  %s",
                          (unsigned long)a->mmsi,
                          a->virtual_aton ? " (virtual)" : "",
                          age);
    double rng = range_nm_from_us(mr, a->lat, a->lon);
    float  brg = bearing_from_us(mr, a->lat, a->lon);
    if (rng >= 0 && !isnan(brg))
        lv_label_set_text_fmt(mr->ais_card_line3,
            "BRG %.0f°   D %.2f NM", brg, rng);
    else
        lv_label_set_text(mr->ais_card_line3, "");
    lv_label_set_text(mr->ais_card_line4, "");    /* no CPA/TCPA for AtoN */
    lv_obj_remove_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mr->ais_card);
}
#endif  /* ESP_PLATFORM — show_ais_card_* real bodies */

static void hide_ais_card(map_renderer_t* mr)
{
    if (!mr->ais_card) return;
    lv_obj_add_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN);
    mr->ais_selected_mmsi = 0;
}

static void chart_tap_dismiss_cb(lv_event_t *e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    if (mr->ais_card && !lv_obj_has_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN))
        hide_ais_card(mr);
}

/* AIS source badge — a tiny status pill parked at the top of the chart
 * that lights up when AIS data is currently flowing (any store entry
 * updated within the last 15 s) and turns dim grey when the store goes
 * cold. Also names the source: "BLE" (phone bridge only), "N2K" (wired
 * receiver only), or "MIX" (both feeding the store) — routing decisions
 * (n2k_ais_tx skips N2K-sourced records) hinge on it, so surfacing it
 * saves the operator wondering why targets show or don't show on other
 * MFDs on the bus. */
#define AIS_BADGE_ACTIVE_WINDOW_MS   15000

static void build_ais_badge(map_renderer_t* mr)
{
    lv_obj_t* chart_area = lv_obj_get_parent(mr->map_container);
    lv_obj_t* root       = lv_obj_get_parent(chart_area);

    lv_obj_t* badge = lv_obj_create(root);
    lv_obj_set_size(badge, 100, 26);
    lv_obj_align_to(badge, chart_area, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x21262d), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_80, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x3a4150), 0);
    lv_obj_set_style_pad_all(badge, 2, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    mr->ais_badge = badge;

    lv_obj_t* lbl = lv_label_create(badge);
    lv_label_set_text(lbl, "AIS --");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8b949e), 0);
    lv_obj_center(lbl);
    mr->ais_badge_label = lbl;
}

/* Count records per source in the two snapshots. Cheap: pools cap at
 * AIS_MAX_VESSELS + AIS_MAX_ATONS = 80 items in the worst case. */
static void tally_sources(const ais_vessel_t *vessels, size_t n_v,
                          const ais_aton_t   *atons,   size_t n_a,
                          size_t *n_ble, size_t *n_n2k)
{
    size_t ble = 0, n2k = 0;
    for (size_t i = 0; i < n_v; i++) {
        if      (vessels[i].source == AIS_SOURCE_BLE) ble++;
        else if (vessels[i].source == AIS_SOURCE_N2K) n2k++;
    }
    for (size_t i = 0; i < n_a; i++) {
        if      (atons[i].source == AIS_SOURCE_BLE) ble++;
        else if (atons[i].source == AIS_SOURCE_N2K) n2k++;
    }
    *n_ble = ble; *n_n2k = n2k;
}

#ifdef ESP_PLATFORM  /* refresh_ais_badge real body — see stub above for sim */
static void refresh_ais_badge(map_renderer_t* mr,
                              const ais_vessel_t *vessels, size_t n_v,
                              const ais_aton_t   *atons,   size_t n_a)
{
    if (!mr->ais_badge_label) return;
    uint32_t now    = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last   = ais_store_last_update_ms();
    bool     active = (last != 0) && ((now - last) < AIS_BADGE_ACTIVE_WINDOW_MS);
    size_t   total  = n_v + n_a;

    if (active) {
        size_t n_ble, n_n2k;
        tally_sources(vessels, n_v, atons, n_a, &n_ble, &n_n2k);

        const char *src;
        if      (n_ble && n_n2k) src = "MIX";
        else if (n_ble)          src = "BLE";
        else if (n_n2k)          src = "N2K";
        else                     src = "?";
        lv_label_set_text_fmt(mr->ais_badge_label, "AIS %s %zu", src, total);
        lv_obj_set_style_text_color(mr->ais_badge_label, lv_color_hex(0x00d4ff), 0);
        lv_obj_set_style_border_color(mr->ais_badge, lv_color_hex(0x00d4ff), 0);
    } else {
        lv_label_set_text(mr->ais_badge_label, "AIS --");
        lv_obj_set_style_text_color(mr->ais_badge_label, lv_color_hex(0x8b949e), 0);
        lv_obj_set_style_border_color(mr->ais_badge, lv_color_hex(0x3a4150), 0);
    }
}
#endif  /* ESP_PLATFORM — refresh_ais_badge real body */

static void init_ais_pool(map_renderer_t* mr)
{
    /* Zero widgets at boot. Slots are created lazily by refresh_ais when
     * real targets arrive from ais_store. Cheap boot, no LVGL churn. */
    for (int i = 0; i < AIS_VESSEL_POOL; i++) {
        mr->ais_vessel_imgs[i] = NULL;
        mr->ais_vessel_bufs[i] = NULL;
        mr->ais_vessel_mmsis[i] = 0;
    }
    for (int i = 0; i < AIS_ATON_POOL; i++) {
        mr->ais_aton_imgs[i] = NULL;
        mr->ais_aton_bufs[i] = NULL;
        mr->ais_aton_mmsis[i] = 0;
    }
    mr->ais_layer = NULL;   /* unused with lazy scheme */
    mr->ais_enabled = true;
    mr->ais_card = NULL;
    mr->ais_badge = NULL;
    mr->ais_badge_label = NULL;
    mr->ais_selected_mmsi = 0;
    mr->ais_selected_is_aton = false;
    build_ais_card(mr);
    build_ais_badge(mr);
}

/* Reposition existing AIS widgets only. Safe to call from apply_pan_offset
 * during a drag — no LVGL widget creation, so no risk of tree corruption
 * while an event handler is iterating children. Walks by SLOT (matching
 * the MMSI stored in mr->ais_vessel_mmsis[s]) so the same widget stays
 * with the same vessel across pans and refreshes. Earlier this iterated
 * by packed snapshot index, which undid refresh_ais's MMSI keying and
 * caused visible blinking when the store re-packed. */
static void reposition_ais_only(map_renderer_t* mr)
{
#ifndef ESP_PLATFORM
    (void)mr;
#else
    if (!mr || !mr->ais_enabled || !mr->map_group) return;
    if (!s_ais_v_scratch || !s_ais_a_scratch) return;
    ais_vessel_t *vessels = s_ais_v_scratch;
    ais_aton_t   *atons   = s_ais_a_scratch;
    size_t n_v = ais_store_get_vessels(vessels, AIS_VESSEL_POOL);
    size_t n_a = ais_store_get_atons  (atons,   AIS_ATON_POOL);

    for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
        if (!mr->ais_vessel_imgs[s]) continue;
        uint32_t mmsi = mr->ais_vessel_mmsis[s];
        if (!mmsi) continue;
        for (size_t i = 0; i < n_v; i++) {
            if (vessels[i].mmsi != mmsi) continue;
            /* Skip if static-only (no position yet) — the widget was
             * showing this vessel at its last real position; leave it
             * put until refresh_ais gets a proper coord. */
            if (isnan(vessels[i].lat) || isnan(vessels[i].lon)) break;
            double vtx, vty;
            lat_lon_to_tile(vessels[i].lat, vessels[i].lon,
                            mr->current_zoom, &vtx, &vty);
            int32_t gx = mr->vp_size / 2 + (int32_t)((vtx - mr->center_tx) * TILE_PX);
            int32_t gy = mr->vp_size / 2 + (int32_t)((vty - mr->center_ty) * TILE_PX);
            lv_obj_set_pos(mr->ais_vessel_imgs[s],
                           gx - AIS_VESSEL_PX / 2, gy - AIS_VESSEL_PX / 2);
            break;
        }
    }
    for (int32_t s = 0; s < AIS_ATON_POOL; s++) {
        if (!mr->ais_aton_imgs[s]) continue;
        uint32_t mmsi = mr->ais_aton_mmsis[s];
        if (!mmsi) continue;
        for (size_t i = 0; i < n_a; i++) {
            if (atons[i].mmsi != mmsi) continue;
            double atx, aty;
            lat_lon_to_tile(atons[i].lat, atons[i].lon,
                            mr->current_zoom, &atx, &aty);
            int32_t gx = mr->vp_size / 2 + (int32_t)((atx - mr->center_tx) * TILE_PX);
            int32_t gy = mr->vp_size / 2 + (int32_t)((aty - mr->center_ty) * TILE_PX);
            lv_obj_set_pos(mr->ais_aton_imgs[s],
                           gx - AIS_ATON_PX / 2, gy - AIS_ATON_PX / 2);
            break;
        }
    }
#endif
}

void map_renderer_refresh_ais(map_renderer_t* mr)
{
#ifndef ESP_PLATFORM
    /* PC simulator has no ais_store — refresh is a no-op there. */
    (void)mr;
#else
    if (!mr || !mr->ais_enabled || !mr->map_group) return;

    /* Snapshot the store. Pool is the upper bound; extra targets are
     * silently dropped — they'll appear next refresh if pool space frees. */
    /* static: 128 × sizeof(ais_vessel_t) = ~15 KB per array, way too
     * much for the 32 KB LVGL task stack when multiple render paths
     * (refresh_ais, reposition_ais_only, click callbacks) can nest via
     * event dispatch. Only ever read from the LVGL task, so a shared
     * static is safe. */
    ais_vessel_t *vessels = s_ais_v_scratch;
    ais_aton_t   *atons   = s_ais_a_scratch;
    size_t n_v = ais_store_get_vessels(vessels, AIS_VESSEL_POOL);
    size_t n_a = ais_store_get_atons  (atons,   AIS_ATON_POOL);

    uint32_t now_ms_local = (uint32_t)(esp_timer_get_time() / 1000);

    /* Vessels: renderer slots are keyed by MMSI, NOT by the packed snapshot
     * index. If we blindly used slot i = vessels[i], any prune / add in the
     * store shifted the packed order and each renderer slot re-drew a
     * different vessel every refresh — user-visible as groups of icons
     * blinking every ~2 s. Keying by MMSI keeps a vessel at the SAME
     * widget across refreshes, so LVGL only invalidates its actual
     * position change; nothing blinks. */
    bool slot_touched[AIS_VESSEL_POOL] = {false};
#if AIS_BLINK_DEBUG
    /* Save the pre-refresh state so we can log transitions after the loop. */
    uint32_t pre_mmsis[AIS_VESSEL_POOL];
    bool     pre_hidden[AIS_VESSEL_POOL];
    for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
        pre_mmsis[s]  = mr->ais_vessel_mmsis[s];
        pre_hidden[s] = mr->ais_vessel_imgs[s]
                      ? lv_obj_has_flag(mr->ais_vessel_imgs[s], LV_OBJ_FLAG_HIDDEN)
                      : true;
    }
    int dbg_nan_skipped = 0, dbg_new_slot = 0, dbg_mmsi_moved = 0, dbg_hidden = 0;
#endif
    for (size_t v_idx = 0; v_idx < n_v; v_idx++) {
        const ais_vessel_t *v = &vessels[v_idx];

        /* Skip static-info-only entries (name / call-sign arrived from a
         * PGN 129809/129810 before any position). alloc_vessel seeds
         * lat/lon = NAN for these; without this guard we'd project NaN
         * through lat_lon_to_tile → undefined int cast → widget placed at
         * garbage screen coords, and the next refresh (when the position
         * finally arrives) would relocate it — the user-visible blink.
         * Slot stays HELD by this MMSI: if the widget was previously
         * showing this vessel at a real position, mark the slot touched
         * so cleanup doesn't hide it. */
        if (isnan(v->lat) || isnan(v->lon)) {
            for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
                if (mr->ais_vessel_mmsis[s] == v->mmsi) {
                    slot_touched[s] = true;
                    break;
                }
            }
#if AIS_BLINK_DEBUG
            dbg_nan_skipped++;
#endif
            continue;
        }

        /* Find the slot already holding this MMSI. */
        int32_t slot = -1;
        for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
            if (mr->ais_vessel_mmsis[s] == v->mmsi) { slot = s; break; }
        }
        /* First time we see this MMSI — take an empty (mmsi=0) slot. */
        if (slot < 0) {
            for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
                if (mr->ais_vessel_mmsis[s] == 0 && !slot_touched[s]) {
                    slot = s; break;
                }
            }
        }
        if (slot < 0) continue;                     /* pool full — drop */
        if (!ensure_vessel_slot(mr, (size_t)slot)) continue;

        slot_touched[slot] = true;
        mr->ais_vessel_mmsis[slot] = v->mmsi;

        float heading = !isnan(v->heading_deg) ? v->heading_deg : v->cog_deg;
        uint32_t color = v->is_class_a ? AIS_COLOR_A : AIS_COLOR_B;
        draw_vessel(mr->ais_vessel_bufs[slot], heading, color);

        /* Dim the marker if we haven't heard from this target recently.
         * The store still prunes at AIS_STALE_AFTER_MS; this is the
         * "aging" state between fresh and gone. */
        bool stale = (now_ms_local - v->pos_update_ms) > AIS_VESSEL_STALE_MS;
        lv_obj_set_style_opa(mr->ais_vessel_imgs[slot],
                             stale ? LV_OPA_40 : LV_OPA_COVER, 0);

        double vtx, vty;
        lat_lon_to_tile(v->lat, v->lon, mr->current_zoom, &vtx, &vty);
        int32_t gx = mr->vp_size / 2 + (int32_t)((vtx - mr->center_tx) * TILE_PX);
        int32_t gy = mr->vp_size / 2 + (int32_t)((vty - mr->center_ty) * TILE_PX);
        lv_obj_set_pos(mr->ais_vessel_imgs[slot],
                       gx - AIS_VESSEL_PX / 2, gy - AIS_VESSEL_PX / 2);
        lv_draw_buf_invalidate_cache(&mr->ais_vessel_dbufs[slot], NULL);
        lv_obj_remove_flag(mr->ais_vessel_imgs[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(mr->ais_vessel_imgs[slot]);
    }
    /* Any slot that had a widget last cycle but wasn't touched this cycle
     * belongs to a vessel that dropped out of the snapshot (pruned as
     * stale, or bumped from a full store). Hide the widget and clear the
     * MMSI so the slot is reusable for a future arrival. */
    for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
        if (!slot_touched[s] && mr->ais_vessel_imgs[s]) {
            lv_obj_add_flag(mr->ais_vessel_imgs[s], LV_OBJ_FLAG_HIDDEN);
            mr->ais_vessel_mmsis[s] = 0;
#if AIS_BLINK_DEBUG
            dbg_hidden++;
#endif
        }
    }
#if AIS_BLINK_DEBUG
    /* Post-refresh diff: what actually changed slot-by-slot? Only log
     * transitions (something changed) — a steady state where nothing
     * moved should be silent. Cheap: max 64 comparisons. */
    for (int32_t s = 0; s < AIS_VESSEL_POOL; s++) {
        uint32_t post_mmsi = mr->ais_vessel_mmsis[s];
        bool post_hidden = mr->ais_vessel_imgs[s]
                         ? lv_obj_has_flag(mr->ais_vessel_imgs[s], LV_OBJ_FLAG_HIDDEN)
                         : true;
        if (post_mmsi != pre_mmsis[s]) {
            if (pre_mmsis[s] == 0) dbg_new_slot++;
            else                   dbg_mmsi_moved++;
            ESP_LOGI(AIS_DBG_TAG, "slot %ld  mmsi %lu -> %lu  (hidden %d -> %d)",
                     (long)s, (unsigned long)pre_mmsis[s], (unsigned long)post_mmsi,
                     (int)pre_hidden[s], (int)post_hidden);
        } else if (post_hidden != pre_hidden[s]) {
            ESP_LOGI(AIS_DBG_TAG, "slot %ld  mmsi %lu  hidden %d -> %d",
                     (long)s, (unsigned long)post_mmsi,
                     (int)pre_hidden[s], (int)post_hidden);
        }
    }
    /* Summary per refresh so we can see cadence + volume at a glance. */
    ESP_LOGI(AIS_DBG_TAG, "refresh: n_v=%zu n_a=%zu nan_skipped=%d "
                          "new_slot=%d mmsi_moved=%d hidden=%d vp=%d",
             n_v, n_a, dbg_nan_skipped, dbg_new_slot, dbg_mmsi_moved, dbg_hidden,
             (int)mr->vp_size);
#endif

    /* Live-refresh the open info card so name/COG/SOG/BRG/D stay current
     * without needing the user to close and re-tap. Reuses the snapshot
     * we already fetched above — no extra ais_store call. */
    if (mr->ais_card && !lv_obj_has_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN)
        && mr->ais_selected_mmsi && !mr->ais_selected_is_aton) {
        for (size_t i = 0; i < n_v; i++) {
            if (vessels[i].mmsi == mr->ais_selected_mmsi) {
                show_ais_card_vessel(mr, &vessels[i]);
                break;
            }
        }
    }

    /* AtoN: keyed by MMSI, same reason as the vessel loop above. */
    bool aton_touched[AIS_ATON_POOL] = {false};
    for (size_t a_idx = 0; a_idx < n_a; a_idx++) {
        const ais_aton_t *a = &atons[a_idx];

        int32_t slot = -1;
        for (int32_t s = 0; s < AIS_ATON_POOL; s++) {
            if (mr->ais_aton_mmsis[s] == a->mmsi) { slot = s; break; }
        }
        if (slot < 0) {
            for (int32_t s = 0; s < AIS_ATON_POOL; s++) {
                if (mr->ais_aton_mmsis[s] == 0 && !aton_touched[s]) {
                    slot = s; break;
                }
            }
        }
        if (slot < 0) continue;
        if (!ensure_aton_slot(mr, (size_t)slot)) continue;

        aton_touched[slot] = true;
        mr->ais_aton_mmsis[slot] = a->mmsi;

        double atx, aty;
        lat_lon_to_tile(a->lat, a->lon, mr->current_zoom, &atx, &aty);
        int32_t gx = mr->vp_size / 2 + (int32_t)((atx - mr->center_tx) * TILE_PX);
        int32_t gy = mr->vp_size / 2 + (int32_t)((aty - mr->center_ty) * TILE_PX);
        lv_obj_set_pos(mr->ais_aton_imgs[slot],
                       gx - AIS_ATON_PX / 2, gy - AIS_ATON_PX / 2);
        lv_obj_remove_flag(mr->ais_aton_imgs[slot], LV_OBJ_FLAG_HIDDEN);
    }
    for (int32_t s = 0; s < AIS_ATON_POOL; s++) {
        if (!aton_touched[s] && mr->ais_aton_imgs[s]) {
            lv_obj_add_flag(mr->ais_aton_imgs[s], LV_OBJ_FLAG_HIDDEN);
            mr->ais_aton_mmsis[s] = 0;
        }
    }

    /* AtoN branch of live-refresh: same idea, using the AtoN snapshot. */
    if (mr->ais_card && !lv_obj_has_flag(mr->ais_card, LV_OBJ_FLAG_HIDDEN)
        && mr->ais_selected_mmsi && mr->ais_selected_is_aton) {
        for (size_t i = 0; i < n_a; i++) {
            if (atons[i].mmsi == mr->ais_selected_mmsi) {
                show_ais_card_aton(mr, &atons[i]);
                break;
            }
        }
    }

    refresh_ais_badge(mr, vessels, n_v, atons, n_a);
#endif /* ESP_PLATFORM */
}

/* ── Grid boundary detection and shifting ── */

/* Ensure the grid covers the current centre and every tile widget is
 * positioned correctly. If the centre has drifted past the safe band
 * inside the 5×5 grid, shift grid_origin (integer tile step) and
 * reload the 25 tile buffers. Always ends with a single
 * apply_pan_offset — this is important, callers must NOT call
 * apply_pan_offset (or center_scroll_on_view) separately for the same
 * centre change: doing so left an intermediate state visible to the
 * parallel LV draw thread, and the user reported tiles occasionally
 * appearing vertically shifted after a recentre. */
static void check_grid_boundary(map_renderer_t* mr)
{
    if (!mr->map_container) return;

    double offset_x = mr->center_tx - mr->grid_origin_tx;
    double offset_y = mr->center_ty - mr->grid_origin_ty;

    if (offset_x < 1.5 || offset_x > GRID_COLS - 1.5 ||
        offset_y < 1.5 || offset_y > GRID_ROWS - 1.5) {
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        load_grid_tiles(mr);
    }
    apply_pan_offset(mr);
}

/* ── Position marker ── */

static void update_marker_position(map_renderer_t* mr)
{
    if (!mr->marker_img) return;

    /* Decide what we have to anchor the marker on:
     *   - pos_valid  -> live fix, draw a yellow COG-oriented triangle
     *   - home_valid -> no fix yet, draw a faint grey "no-fix" ring at the
     *                   initial home center so the user can at least see
     *                   "you are here (approximately)"
     *   - neither    -> hide the marker entirely */
    double lat, lon;
    bool have_fix = mr->pos_valid;
    if (have_fix) {
        lat = mr->pos_lat;
        lon = mr->pos_lon;
    } else if (mr->home_valid) {
        lat = mr->home_lat;
        lon = mr->home_lon;
    } else {
        lv_obj_add_flag(mr->marker_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(mr->marker_img, LV_OBJ_FLAG_HIDDEN);

    double ptx, pty;
    lat_lon_to_tile(lat, lon, mr->current_zoom, &ptx, &pty);

    /* Screen position, matching apply_pan_offset(): map content at tile-coord T
     * is drawn at vp/2 + (T - center)*TILE_PX. So the vessel sits at viewport
     * centre when centred, and drifts from centre within the deadband. */
    int32_t mx = mr->vp_size / 2 + (int32_t)((ptx - mr->center_tx) * TILE_PX);
    int32_t my = mr->vp_size / 2 + (int32_t)((pty - mr->center_ty) * TILE_PX);
    lv_obj_set_pos(mr->marker_img, mx - MARKER_SIZE / 2, my - MARKER_SIZE / 2);

    /* Clear marker canvas to transparent */
    memset(mr->marker_buf, 0, MARKER_SIZE * MARKER_SIZE * 4);
    uint32_t* pixels = (uint32_t*)mr->marker_buf;

    if (!have_fix) {
        /* No-fix marker: hollow grey ring, no orientation. */
        int32_t cx = MARKER_SIZE / 2, cy = MARKER_SIZE / 2;
        int32_t r_out = 9, r_in = 6;
        uint32_t grey = 0xCC9098A4;   /* ARGB: ~80% opaque cool grey */
        for (int32_t y = -r_out; y <= r_out; y++) {
            for (int32_t x = -r_out; x <= r_out; x++) {
                int32_t d2 = x * x + y * y;
                if (d2 > r_out * r_out || d2 < r_in * r_in) continue;
                int32_t px_x = cx + x, px_y = cy + y;
                if (px_x >= 0 && px_x < MARKER_SIZE
                    && px_y >= 0 && px_y < MARKER_SIZE)
                    pixels[px_y * MARKER_SIZE + px_x] = grey;
            }
        }
        lv_draw_buf_invalidate_cache(&mr->marker_dbuf, NULL);
        lv_obj_invalidate(mr->marker_img);
        return;
    }

    /* Draw yellow triangle with COG heading into the ARGB8888 buffer */
    float rad = (mr->pos_cog - 90.0f) * (float)M_PI / 180.0f;
    float cs = cosf(rad), sn = sinf(rad);
    int32_t cx = MARKER_SIZE / 2, cy = MARKER_SIZE / 2;

    int32_t tip_x  = cx + (int32_t)(10 * cs);
    int32_t tip_y  = cy + (int32_t)(10 * sn);
    int32_t left_x = cx + (int32_t)(-6 * cs - (-5) * sn);
    int32_t left_y = cy + (int32_t)(-6 * sn + (-5) * cs);
    int32_t right_x = cx + (int32_t)(-6 * cs - 5 * sn);
    int32_t right_y = cy + (int32_t)(-6 * sn + 5 * cs);

    uint32_t yellow = 0xFFFFFF00;  /* ARGB: fully opaque yellow */
    uint32_t black  = 0xFF000000;  /* ARGB: fully opaque black */

    #define DRAW_LINE_ARGB(x0,y0,x1,y1,c) do { \
        int32_t _dx=abs((x1)-(x0)),_sx=(x0)<(x1)?1:-1; \
        int32_t _dy=-abs((y1)-(y0)),_sy=(y0)<(y1)?1:-1; \
        int32_t _err=_dx+_dy,_e2; \
        int32_t _cx=(x0),_cy=(y0); \
        for(;;){ \
            if(_cx>=0&&_cx<MARKER_SIZE&&_cy>=0&&_cy<MARKER_SIZE) \
                pixels[_cy*MARKER_SIZE+_cx]=(c); \
            if(_cx==(x1)&&_cy==(y1))break; \
            _e2=2*_err; \
            if(_e2>=_dy){_err+=_dy;_cx+=_sx;} \
            if(_e2<=_dx){_err+=_dx;_cy+=_sy;} \
        } \
    } while(0)

    /* Outline */
    DRAW_LINE_ARGB(tip_x, tip_y, left_x, left_y, black);
    DRAW_LINE_ARGB(left_x, left_y, right_x, right_y, black);
    DRAW_LINE_ARGB(right_x, right_y, tip_x, tip_y, black);
    /* Fill */
    for (int32_t i = 0; i <= 10; i++) {
        int32_t lx = left_x + (right_x - left_x) * i / 10;
        int32_t ly = left_y + (right_y - left_y) * i / 10;
        DRAW_LINE_ARGB(lx, ly, tip_x, tip_y, yellow);
    }
    #undef DRAW_LINE_ARGB

    lv_draw_buf_invalidate_cache(&mr->marker_dbuf, NULL);
    lv_obj_invalidate(mr->marker_img);
}

/* ── Prefetch ── */

static void prefetch_if_cached(map_renderer_t* mr)
{
    if (mr->cache) {
        int32_t ctx = (int32_t)floor(mr->center_tx);
        int32_t cty = (int32_t)floor(mr->center_ty);
        tile_cache_prefetch(mr->cache, mr->path_id,
            ctx, cty, mr->current_zoom, mr->tile_base);
    }
}

/* ── Event handlers ── */

static void show_track_btn(map_renderer_t* mr, bool show)
{
    if (mr->track_btn) {
        if (show) lv_obj_remove_flag(mr->track_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(mr->track_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Pan offset: how many pixels the grid is shifted from its origin */
static void apply_pan_offset(map_renderer_t* mr)
{
    int32_t base_x = -(int32_t)((mr->center_tx - mr->grid_origin_tx) * TILE_PX) + mr->vp_size / 2;
    int32_t base_y = -(int32_t)((mr->center_ty - mr->grid_origin_ty) * TILE_PX) + mr->vp_size / 2;

    for (int32_t i = 0; i < GRID_TILES; i++) {
        if (!mr->tile_widgets[i]) continue;
        int32_t col = i % GRID_COLS;
        int32_t row = i / GRID_COLS;
        int32_t px = base_x + col * TILE_PX;
        int32_t py = base_y + row * TILE_PX;

        /* Skip repositioning tiles that are fully off-screen */
        bool visible = (px + TILE_PX > 0 && px < mr->vp_size &&
                        py + TILE_PX > 0 && py < mr->vp_size);

        if (visible) {
            lv_obj_set_pos(mr->tile_widgets[i], px, py);
            lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Hide off-screen tiles so LVGL skips them entirely during render */
            lv_obj_add_flag(mr->tile_widgets[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (mr->marker_img) update_marker_position(mr);
    /* Keep AIS glued to the chart when panning / boundary shifts. Position
     * only — widget creation from inside a drag event can corrupt LVGL's
     * child iterator, so we skip ensure_*_slot() here and let the periodic
     * data-tick refresh handle new arrivals. */
    reposition_ais_only(mr);
}

static void drag_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        mr->panning = false;
        check_grid_boundary(mr);
        prefetch_if_cached(mr);
#ifdef ESP_PLATFORM
        /* User just finished dragging this map — it's the currently-
         * visible one, so its bbox is what determines what AIS the
         * user should be seeing. Push it to the store; incoming
         * updates outside the bbox get dropped, and vessels already
         * out of view get pruned. Stops the LRU churn when the AIS
         * source (BLE / N2K) is feeding a much wider area. */
        map_viewport_bbox_t bb;
        map_renderer_get_viewport_bbox(mr, &bb);
        ais_store_set_viewport(bb.lat_min, bb.lat_max,
                               bb.lon_min, bb.lon_max);
#endif
        return;
    }
    if (code != LV_EVENT_PRESSING) return;

    lv_indev_t* indev = lv_indev_active();
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.x == 0 && vect.y == 0) return;

    mr->panning = true;
    mr->tracking = false;
    show_track_btn(mr, true);

    /* Update center coords by pixel delta */
    double scale = 1.0 / TILE_PX;
    mr->center_tx -= vect.x * scale;
    mr->center_ty -= vect.y * scale;
    tile_to_lat_lon(mr->center_tx, mr->center_ty, mr->current_zoom, &mr->center_lat, &mr->center_lon);

    /* Just reposition widgets — no tile reloading */
    apply_pan_offset(mr);
}

static void refresh_zoom_btn_states(map_renderer_t* mr);

static void track_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    map_renderer_track(mr);
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

static void zoom_in_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    map_renderer_zoom_in(mr);
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

static void zoom_out_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    map_renderer_zoom_out(mr);
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

static void theme_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    if (mr->tile_alt[0] == '\0') return;

    mr->using_alt = !mr->using_alt;
    strncpy(mr->tile_base,
        mr->using_alt ? mr->tile_alt : mr->tile_primary,
        PATH_BUF_LEN - 1);
    mr->path_id = mr->using_alt ? mr->path_id_alt : mr->path_id_primary;
    load_grid_tiles(mr);
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

/* ── Public API ── */

map_renderer_t* map_renderer_create(lv_obj_t* parent, const char* tile_base, int32_t viewport_size)
{
#ifdef ESP_PLATFORM
    /* Once-only PSRAM allocation for the shared AIS snapshot scratch —
     * we call it here (rather than lazy per-refresh) so the memory is
     * definitely available before any refresh_ais / reposition path
     * can run. If PSRAM alloc fails (highly unlikely), refresh_ais
     * and friends silently no-op via the null guard at each site. */
    ensure_ais_scratch();
#endif
    map_renderer_t* mr = (map_renderer_t*)lv_malloc(sizeof(map_renderer_t));
    if (!mr) return NULL;
    memset(mr, 0, sizeof(*mr));

    strncpy(mr->tile_base, tile_base, PATH_BUF_LEN - 1);
    strncpy(mr->tile_primary, tile_base, PATH_BUF_LEN - 1);
    mr->tile_primary[PATH_BUF_LEN - 1] = '\0';
    mr->vp_size = viewport_size;
    mr->tracking = true;
    mr->current_zoom = 13;
    mr->parent = parent;

    /* ── Step 1: scrollable container ── */
    mr->map_container = lv_obj_create(parent);
    lv_obj_set_size(mr->map_container, viewport_size, viewport_size);
    lv_obj_center(mr->map_container);
    lv_obj_set_style_bg_color(mr->map_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(mr->map_container, 0, 0);
    lv_obj_set_style_radius(mr->map_container, 0, 0);  /* CRITICAL: radius=0 prevents layer alloc */
    lv_obj_set_style_pad_all(mr->map_container, 0, 0);
    lv_obj_set_scrollbar_mode(mr->map_container, LV_SCROLLBAR_MODE_OFF);
    /* Custom drag handler drives panning (see drag_cb) */
    lv_obj_add_flag(mr->map_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    mr->map_group = mr->map_container;

    mr->marker_img = NULL;
    mr->marker_buf = NULL;

    for (int32_t i = 0; i < GRID_TILES; i++) {
        mr->tile_bufs[i] = (uint8_t*)PSRAM_ALLOC(TILE_BYTES);
        if (!mr->tile_bufs[i]) {
            for (int32_t j = 0; j < i; j++) free(mr->tile_bufs[j]);
            lv_free(mr);
            return NULL;
        }
        fill_missing(mr->tile_bufs[i]);
        mr->tile_loaded[i] = false;

        lv_draw_buf_init(&mr->tile_dbufs[i], TILE_PX, TILE_PX,
            LV_COLOR_FORMAT_RGB565, 0,
            mr->tile_bufs[i], TILE_BYTES);
        lv_draw_buf_set_flag(&mr->tile_dbufs[i], LV_IMAGE_FLAGS_MODIFIABLE);

        int32_t col = i % GRID_COLS;
        int32_t row = i / GRID_COLS;
        mr->tile_widgets[i] = lv_image_create(mr->map_group);
        lv_obj_set_pos(mr->tile_widgets[i], col * TILE_PX, row * TILE_PX);
        lv_image_set_src(mr->tile_widgets[i], &mr->tile_dbufs[i]);
        lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Position marker (vessel arrow), created after the tiles so it draws on
     * top. Small ARGB8888 image (MARKER_SIZE px) so the per-update blend is
     * cheap. update_marker_position() draws the COG-oriented triangle. */
    mr->marker_buf = (uint8_t*)PSRAM_ALLOC(MARKER_SIZE * MARKER_SIZE * 4);
    if (mr->marker_buf) {
        memset(mr->marker_buf, 0, MARKER_SIZE * MARKER_SIZE * 4);
        lv_draw_buf_init(&mr->marker_dbuf, MARKER_SIZE, MARKER_SIZE,
            LV_COLOR_FORMAT_ARGB8888, 0, mr->marker_buf, MARKER_SIZE * MARKER_SIZE * 4);
        lv_draw_buf_set_flag(&mr->marker_dbuf, LV_IMAGE_FLAGS_MODIFIABLE);
        mr->marker_img = lv_image_create(mr->map_group);
        lv_image_set_src(mr->marker_img, &mr->marker_dbuf);
        lv_obj_remove_flag(mr->marker_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(mr->marker_img, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* AIS overlay layer — sits between tiles and marker (own boat on top). */
    init_ais_pool(mr);

    /* Touch handling for the custom drag pan */
    #define PAN_EDGE_PX 100
    lv_obj_t* touch_target;
    if (viewport_size >= 800) {
        /* Full-screen map: narrower touch zone leaves edges for tileview swipe */
        lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_CLICKABLE);
        touch_target = lv_obj_create(parent);
        lv_obj_set_size(touch_target, viewport_size - 2 * PAN_EDGE_PX, viewport_size);
        lv_obj_center(touch_target);
        lv_obj_set_style_bg_opa(touch_target, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(touch_target, 0, 0);
        lv_obj_set_style_pad_all(touch_target, 0, 0);
        lv_obj_set_scrollbar_mode(touch_target, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(touch_target, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(touch_target, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(touch_target, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_remove_flag(touch_target, LV_OBJ_FLAG_GESTURE_BUBBLE);
    } else {
        /* Smaller map: container handles touch directly */
        touch_target = mr->map_container;
    }
    lv_obj_add_event_cb(touch_target, drag_cb, LV_EVENT_PRESSING, mr);
    lv_obj_add_event_cb(touch_target, drag_cb, LV_EVENT_RELEASED, mr);
    lv_obj_add_event_cb(touch_target, drag_cb, LV_EVENT_PRESS_LOST, mr);
    /* Tap on empty water (short click that wasn't consumed by an AIS
     * widget's own handler) dismisses the info card. */
    lv_obj_add_event_cb(touch_target, chart_tap_dismiss_cb,
                        LV_EVENT_SHORT_CLICKED, mr);

    /* Move the transparent touch_target to the BACK of chart_area so
     * LVGL's hit-testing checks map_container's AIS-icon children FIRST.
     * Without this the touch_target (created last → topmost sibling)
     * intercepts every tap and vessel_click_cb never fires on the full
     * chart. AIS icons are children of map_container which is
     * non-clickable but has clickable descendants — LVGL descends into
     * it, hits the icon, dispatches vessel_click_cb. If the tap misses
     * every icon, LVGL falls back to the sibling behind it
     * (touch_target) which then handles drag / tap-dismiss. */
    if (touch_target != mr->map_container) {
        lv_obj_move_background(touch_target);
    }

    mr->track_btn = NULL;
    return mr;
}

void map_renderer_set_view(map_renderer_t* mr, double lat, double lon, int32_t zoom)
{
    mr->center_lat = lat;
    mr->center_lon = lon;
    mr->home_lat   = lat;
    mr->home_lon   = lon;
    mr->home_valid = true;
    mr->current_zoom = zoom;
    if (mr->current_zoom < ZOOM_MIN) mr->current_zoom = ZOOM_MIN;
    if (mr->current_zoom > ZOOM_MAX) mr->current_zoom = ZOOM_MAX;
    update_tile_coords(mr);

    /* Position grid centered on the view */
    mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
    mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;

#ifndef ESP_PLATFORM
    /* Simulator has no background tile loader, so load the visible grid
     * synchronously here. (On target this is async — see below.) */
    if (mr->cache) {
        for (int32_t row = 0; row < GRID_ROWS; row++)
            for (int32_t col = 0; col < GRID_COLS; col++) {
                tile_key_t key = { mr->path_id, (uint8_t)mr->current_zoom,
                                   (uint16_t)(mr->grid_origin_tx + col),
                                   (uint16_t)(mr->grid_origin_ty + row) };
                tile_cache_load_sync(mr->cache, key, mr->tile_base);
            }
    }
#endif
    /* On target, load_grid_tiles() requests cache misses via the background
     * loader (async) and shows placeholders until they arrive — no blocking SD
     * reads during boot. */
    load_grid_tiles(mr);
    center_scroll_on_view(mr);
    update_marker_position(mr);
    prefetch_if_cached(mr);
}

/* FIXME: violates the "check_grid_boundary is the single settle" invariant
 * the rest of this file now upholds. Today no live caller passes a
 * (dx, dy) large enough to push the offset outside the safe band, so
 * grid_origin never needs to shift and the missing check_grid_boundary
 * call is invisible. Any future caller that pans by more than a couple
 * of tile widths in one step would see stale tiles until the next drag
 * or tracking recentre. Route this through check_grid_boundary too. */
void map_renderer_pan(map_renderer_t* mr, int32_t dx, int32_t dy)
{
    double scale = 1.0 / TILE_PX;
    mr->center_tx += dx * scale;
    mr->center_ty += dy * scale;
    tile_to_lat_lon(mr->center_tx, mr->center_ty, mr->current_zoom, &mr->center_lat, &mr->center_lon);
    apply_pan_offset(mr);
}

void map_renderer_zoom_in(map_renderer_t* mr)
{
    if (mr->current_zoom < ZOOM_MAX) {
        mr->current_zoom++;
        update_tile_coords(mr);
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        load_grid_tiles(mr);
        center_scroll_on_view(mr);
        update_marker_position(mr);
        prefetch_if_cached(mr);
        refresh_zoom_btn_states(mr);
    }
}

void map_renderer_zoom_out(map_renderer_t* mr)
{
    if (mr->current_zoom > ZOOM_MIN) {
        mr->current_zoom--;
        update_tile_coords(mr);
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        load_grid_tiles(mr);
        center_scroll_on_view(mr);
        update_marker_position(mr);
        prefetch_if_cached(mr);
        refresh_zoom_btn_states(mr);
    }
}

void map_renderer_set_own_sog(map_renderer_t* mr, float sog_knots)
{
    if (mr) mr->pos_sog_knots = sog_knots;
}

void map_renderer_mirror_pos(map_renderer_t* dst, map_renderer_t* src)
{
    if (!dst || !src || dst == src || !src->pos_valid) return;
    /* Skip if already caught up — avoids a needless marker redraw. */
    if (dst->pos_valid &&
        dst->pos_lat == src->pos_lat &&
        dst->pos_lon == src->pos_lon &&
        dst->pos_cog == src->pos_cog &&
        dst->pos_sog_knots == src->pos_sog_knots) return;
    map_renderer_set_position(dst, src->pos_lat, src->pos_lon, src->pos_cog);
    map_renderer_set_own_sog(dst, src->pos_sog_knots);
}

void map_renderer_set_position(map_renderer_t* mr, double lat, double lon, float cog_deg)
{
    mr->pos_lat = lat;
    mr->pos_lon = lon;
    /* NaN COG can arrive on GPS reacquisition even when SOG is above
     * the anchor gate. Casting sin/cos(NaN) → NaN → int32_t downstream
     * in update_marker_position is undefined behaviour and the
     * deadband check on NaN drift silently never fires, freezing
     * tracking until COG recovers. Keep the last-known heading
     * whenever the incoming value is not finite. */
    if (isfinite(cog_deg)) mr->pos_cog = cog_deg;
    mr->pos_valid = true;

    if (mr->tracking) {
        double ptx, pty;
        lat_lon_to_tile(lat, lon, mr->current_zoom, &ptx, &pty);

        /* Where the tile-coord centre SHOULD sit for the vessel to land
         * on the course-up anchor. Bias vector in tile units:
         * translate the pixel-space "opposite of COG" unit vector into
         * tile deltas. Screen y grows southward; north-up map means
         * tile y grows southward too. So:
         *   COG   0° (N)  → bias = ( 0,-1)   centre goes NORTH of boat
         *   COG  90° (E)  → bias = (+1, 0)   centre goes EAST  of boat
         *   COG 180° (S)  → bias = ( 0,+1)   centre goes SOUTH of boat
         *   COG 270° (W)  → bias = (-1, 0)   centre goes WEST  of boat
         * i.e. bias = (sin(cog), -cos(cog)).
         * Falls back to zero bias (dead-centre) when there's no
         * meaningful heading available. */
        /* SOG threshold gates the COG bias — pos_cog is zeroed at
         * struct init, so a bare "cog!=0" check would treat a
         * genuine due-north course as "no heading". Trust the COG
         * only when the vessel is actually making way. */
        double bias_tx = 0.0, bias_ty = 0.0;
        if (mr->pos_sog_knots >= MARKER_ANCHOR_MIN_SOG_KNOTS) {
            double cog_rad = (double)mr->pos_cog * M_PI / 180.0;
            double offset_tiles = (double)MARKER_ANCHOR_OFFSET_PX / (double)TILE_PX;
            bias_tx =  sin(cog_rad) * offset_tiles;
            bias_ty = -cos(cog_rad) * offset_tiles;
        }
        double anchor_tx = ptx + bias_tx;
        double anchor_ty = pty + bias_ty;

        /* Deadband on the DRIFT from the ideal course-up centre. If we
         * were to recentre right now, the vessel would land exactly on
         * the anchor point on screen. Once accumulated drift exceeds
         * 30 % of the viewport in any axis, recentre. */
        double drift_x = (anchor_tx - mr->center_tx) * TILE_PX;
        double drift_y = (anchor_ty - mr->center_ty) * TILE_PX;
        double deadband = mr->vp_size * 0.30;
        if (fabs(drift_x) > deadband || fabs(drift_y) > deadband) {
            mr->center_tx = anchor_tx;
            mr->center_ty = anchor_ty;
            tile_to_lat_lon(mr->center_tx, mr->center_ty, mr->current_zoom,
                            &mr->center_lat, &mr->center_lon);
            /* Single settle call — no earlier apply_pan_offset. See the
             * comment on check_grid_boundary. */
            check_grid_boundary(mr);
            return;
        }
    }

    update_marker_position(mr);
}

void map_renderer_track(map_renderer_t* mr)
{
    mr->tracking = true;
    show_track_btn(mr, false);

    double target_lat, target_lon;
    if (mr->pos_valid) {
        target_lat = mr->pos_lat;
        target_lon = mr->pos_lon;
    } else if (mr->home_valid) {
        target_lat = mr->home_lat;
        target_lon = mr->home_lon;
    } else {
        return;
    }
    mr->center_lat = target_lat;
    mr->center_lon = target_lon;
    update_tile_coords(mr);
    /* Single settle path — check_grid_boundary owns the apply_pan_offset
     * call. Previously we also called center_scroll_on_view here, which
     * races the parallel draw thread: writer #1 (center_scroll_on_view)
     * placed the tile widgets for the OLD grid_origin, writer #2
     * (check_grid_boundary) shifted grid_origin, memcpy'd 25×128 KiB
     * of tile buffers, and re-placed widgets. The drawer could sample
     * writer #1's widget positions while writer #2 was still overwriting
     * the pixel source, giving the vertical-tile-shift flash seen after
     * a track-button tap. */
    check_grid_boundary(mr);
    update_marker_position(mr);
}

void map_renderer_create_track_btn(map_renderer_t* mr, lv_obj_t* btn_parent,
    int32_t x_ofs, int32_t y_ofs)
{
    mr->track_btn = lv_button_create(btn_parent);
    lv_obj_set_size(mr->track_btn, 40, 40);
    lv_obj_align(mr->track_btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_radius(mr->track_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mr->track_btn, lv_color_hex(0x21262d), 0);
    lv_obj_set_style_bg_opa(mr->track_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mr->track_btn, 0, 0);
    lv_obj_add_event_cb(mr->track_btn, track_btn_cb, LV_EVENT_CLICKED, mr);
    lv_obj_remove_flag(mr->track_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(mr->track_btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(mr->track_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl = lv_label_create(mr->track_btn);
    lv_label_set_text(lbl, LV_SYMBOL_GPS);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
}

void map_renderer_set_alt_tiles(map_renderer_t* mr, const char* alt_tile_base,
    lv_obj_t* btn_parent, int32_t x_ofs, int32_t y_ofs)
{
    strncpy(mr->tile_alt, alt_tile_base, PATH_BUF_LEN - 1);
    mr->tile_alt[PATH_BUF_LEN - 1] = '\0';
    mr->using_alt = false;

    mr->theme_btn = lv_button_create(btn_parent);
    lv_obj_set_size(mr->theme_btn, 40, 40);
    lv_obj_align(mr->theme_btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_radius(mr->theme_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mr->theme_btn, lv_color_hex(0x21262d), 0);
    lv_obj_set_style_bg_opa(mr->theme_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mr->theme_btn, 0, 0);
    lv_obj_add_event_cb(mr->theme_btn, theme_btn_cb, LV_EVENT_CLICKED, mr);
    lv_obj_remove_flag(mr->theme_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(mr->theme_btn, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t* lbl = lv_label_create(mr->theme_btn);
    lv_label_set_text(lbl, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
}

/* Helper: one circular semi-transparent button matching the track/theme look,
 * with a 32-px telltale icon centered on it. */
static lv_obj_t* make_overlay_btn(lv_obj_t* parent, int32_t x_ofs, int32_t y_ofs,
                                  const lv_image_dsc_t* icon,
                                  lv_event_cb_t cb, void* user_data)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x21262d), 0);
    /* Semi-transparent so the map underneath still shows through — user
     * ask, matches the badge's blend and the AIS icons' opacity. Set
     * both here and in set_btn_enabled's enabled branch since that path
     * repeats the assignment when zoom-in/out cross ZOOM_MIN/MAX. */
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    return btn;
}

static void set_btn_enabled(lv_obj_t* btn, bool enabled)
{
    if (!btn) return;
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    }
    /* The button has exactly one child — the lv_image we created. Dim its
     * recolor when disabled so the icon stops looking interactive. */
    lv_obj_t* img = lv_obj_get_child(btn, 0);
    if (img) {
        lv_obj_set_style_image_recolor(img,
            enabled ? lv_color_white() : lv_color_hex(0x666666), 0);
    }
}

static void refresh_zoom_btn_states(map_renderer_t* mr)
{
    set_btn_enabled(mr->zoom_in_btn,  mr->current_zoom < ZOOM_MAX);
    set_btn_enabled(mr->zoom_out_btn, mr->current_zoom > ZOOM_MIN);
}

void map_renderer_create_zoom_btns(map_renderer_t* mr, lv_obj_t* btn_parent,
    int32_t x_in,  int32_t y_in,
    int32_t x_out, int32_t y_out)
{
    mr->zoom_in_btn  = make_overlay_btn(btn_parent, x_in,  y_in,
                                        &tt_icon_zoom_in_32,  zoom_in_btn_cb,  mr);
    mr->zoom_out_btn = make_overlay_btn(btn_parent, x_out, y_out,
                                        &tt_icon_zoom_out_32, zoom_out_btn_cb, mr);
    refresh_zoom_btn_states(mr);
}

void map_renderer_set_vignette(map_renderer_t* mr, float fade_start_pct, lv_color_t color)
{
    /* Vignette is not supported in widget-grid mode (no canvas to draw into).
     * Kept for API compatibility. */
    mr->vignette_start = fade_start_pct;
    mr->vignette_color = ((color.red >> 3) << 11) |
                         ((color.green >> 2) << 5) |
                          (color.blue >> 3);
}

void map_renderer_set_cache(map_renderer_t* mr, tile_cache_t* cache)
{
    if (!mr || !cache) return;
    mr->cache = cache;
    mr->path_id_primary = tile_cache_register_path(cache, mr->tile_primary);
    mr->path_id = mr->path_id_primary;
    if (mr->tile_alt[0] != '\0') {
        mr->path_id_alt = tile_cache_register_path(cache, mr->tile_alt);
        if (mr->using_alt) mr->path_id = mr->path_id_alt;
    }
    /* No blocking prepopulate — the caller's map_renderer_set_view() requests
     * the visible tiles asynchronously via the background loader. */
}

void map_renderer_render(map_renderer_t* mr)
{
    if (!mr || !mr->map_container) return;

    /* Fill in any tiles that were cache misses and have now loaded. */
    for (int32_t i = 0; i < GRID_TILES; i++) {
        if (mr->tile_loaded[i]) continue;

        int32_t col = i % GRID_COLS;
        int32_t row = i / GRID_COLS;
        int32_t tx = mr->grid_origin_tx + col;
        int32_t ty = mr->grid_origin_ty + row;

        tile_key_t key = { mr->path_id, (uint8_t)mr->current_zoom,
                           (uint16_t)tx, (uint16_t)ty };
        const uint8_t* td = mr->cache ? tile_cache_get(mr->cache, key) : NULL;
        if (td) {
            memcpy(mr->tile_bufs[i], td, TILE_BYTES);
            mr->tile_loaded[i] = true;
            lv_draw_buf_invalidate_cache(&mr->tile_dbufs[i], NULL);
            /* Only un-hide if the widget's current position is actually
             * on-screen. apply_pan_offset hides off-screen tiles as a
             * render-time optimisation; blindly un-hiding here would
             * make LVGL redraw off-screen widgets (fps regressed from
             * ~27 to ~5 flushes/5s with the unconditional un-hide).
             * Position is the source of truth — read it back. */
            lv_area_t coords;
            lv_obj_get_coords(mr->tile_widgets[i], &coords);
            int32_t rx0 = coords.x1 - lv_obj_get_x(mr->map_group);
            int32_t ry0 = coords.y1 - lv_obj_get_y(mr->map_group);
            bool on_screen = (rx0 + TILE_PX > 0 && rx0 < mr->vp_size &&
                              ry0 + TILE_PX > 0 && ry0 < mr->vp_size);
            if (on_screen) {
                lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_invalidate(mr->tile_widgets[i]);
            }
        }
    }
}

double map_renderer_get_lat(map_renderer_t* mr) { return mr->center_lat; }
double map_renderer_get_lon(map_renderer_t* mr) { return mr->center_lon; }
int32_t map_renderer_get_zoom(map_renderer_t* mr) { return mr->current_zoom; }
bool map_renderer_is_tracking(map_renderer_t* mr) { return mr && mr->tracking; }
bool map_renderer_is_panning(map_renderer_t* mr)  { return mr && mr->panning; }

void map_renderer_get_viewport_bbox(map_renderer_t* mr, map_viewport_bbox_t* out)
{
    if (!mr || !out) return;
    /* Half-viewport in tile units, plus 20% margin so vessels appear at the
     * edges *before* they physically enter the visible circle. */
    double half_tx = ((double)mr->vp_size * 0.5 * 1.2) / (double)TILE_PX;
    double tx_lo = mr->center_tx - half_tx;
    double tx_hi = mr->center_tx + half_tx;
    double ty_lo = mr->center_ty - half_tx;  /* square viewport, same scale */
    double ty_hi = mr->center_ty + half_tx;

    double lat_a, lat_b, lon_a, lon_b;
    tile_to_lat_lon(tx_lo, ty_lo, mr->current_zoom, &lat_a, &lon_a);  /* top-left */
    tile_to_lat_lon(tx_hi, ty_hi, mr->current_zoom, &lat_b, &lon_b);  /* bot-right */

    /* In Web-Mercator, ty_lo (smaller Y) maps to HIGHER latitude. */
    out->lat_max = lat_a;
    out->lat_min = lat_b;
    out->lon_min = lon_a;
    out->lon_max = lon_b;
    out->zoom    = mr->current_zoom;
}

lv_obj_t* map_renderer_get_ais_badge(map_renderer_t* mr)
{
    return mr ? mr->ais_badge : NULL;
}

bool map_renderer_focus_mmsi(map_renderer_t* mr, uint32_t mmsi)
{
#ifdef ESP_PLATFORM
    if (!mr || !mmsi) return false;
    if (!s_ais_v_scratch || !s_ais_a_scratch) return false;
    ais_vessel_t *vessels = s_ais_v_scratch;
    size_t n_v = ais_store_get_vessels(vessels, AIS_MAX_VESSELS);
    for (size_t i = 0; i < n_v; i++) {
        if (vessels[i].mmsi != mmsi) continue;
        /* Skip targets whose position is unknown — centering on NAN would
         * blank the map. Static-info-only entries fall in this bucket. */
        if (isnan(vessels[i].lat) || isnan(vessels[i].lon)) return false;
        mr->tracking = false;
        map_renderer_set_view(mr, vessels[i].lat, vessels[i].lon, mr->current_zoom);
        map_renderer_refresh_ais(mr);        /* place icons at the new center now */
        show_ais_card_vessel(mr, &vessels[i]);
        map_renderer_render(mr);
        return true;
    }
    ais_aton_t *atons = s_ais_a_scratch;
    size_t n_a = ais_store_get_atons(atons, AIS_MAX_ATONS);
    for (size_t i = 0; i < n_a; i++) {
        if (atons[i].mmsi != mmsi) continue;
        if (isnan(atons[i].lat) || isnan(atons[i].lon)) return false;
        mr->tracking = false;
        map_renderer_set_view(mr, atons[i].lat, atons[i].lon, mr->current_zoom);
        map_renderer_refresh_ais(mr);
        show_ais_card_aton(mr, &atons[i]);
        map_renderer_render(mr);
        return true;
    }
    return false;
#else
    (void)mr; (void)mmsi;
    return false;
#endif
}
