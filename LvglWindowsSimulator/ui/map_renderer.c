#include "map_renderer.h"
#include "tile_cache.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TILE_PX      256
#define TILE_BYTES   (TILE_PX * TILE_PX * 2)  /* RGB565 */
#define ZOOM_MIN     10
#define ZOOM_MAX     15
#define PATH_BUF_LEN 300
#define GRID_COLS    5
#define GRID_ROWS    5
#define GRID_TILES   (GRID_COLS * GRID_ROWS)
#define GRID_PX_W    (GRID_COLS * TILE_PX)
#define GRID_PX_H    (GRID_ROWS * TILE_PX)
#define MISSING_COLOR 0x0883
#define MARKER_SIZE  30

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

    lv_obj_t*  track_btn;
    int32_t    vp_size;
    char       tile_base[PATH_BUF_LEN];

    double     center_lat, center_lon;
    int32_t    current_zoom;
    double     center_tx, center_ty;

    double     pos_lat, pos_lon;
    float      pos_cog;
    bool       pos_valid;
    bool       tracking;

    float      vignette_start;     /* kept for API compat, unused in widget-grid mode */
    uint16_t   vignette_color;

    char       tile_primary[PATH_BUF_LEN];
    char       tile_alt[PATH_BUF_LEN];
    bool       using_alt;
    lv_obj_t*  theme_btn;

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

/* Fill a tile buffer with MISSING_COLOR */
static void fill_missing(uint8_t* buf)
{
    uint16_t* px = (uint16_t*)buf;
    for (int32_t i = 0; i < TILE_PX * TILE_PX; i++)
        px[i] = MISSING_COLOR;
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

static void update_center_from_scroll(map_renderer_t* mr)
{
    /* center_tx/ty are already maintained by the drag handler */
    (void)mr;
}

/* ── Grid boundary detection and shifting ── */

static void check_grid_boundary(map_renderer_t* mr)
{
    if (!mr->map_container) return;

    /* Check if center has moved too close to grid edge */
    double offset_x = mr->center_tx - mr->grid_origin_tx;
    double offset_y = mr->center_ty - mr->grid_origin_ty;
    bool need_reload = false;

    /* Re-center grid when within 1 tile of any edge */
    if (offset_x < 1.5 || offset_x > GRID_COLS - 1.5 ||
        offset_y < 1.5 || offset_y > GRID_ROWS - 1.5) {
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        need_reload = true;
    }

    if (need_reload) {
        load_grid_tiles(mr);
        apply_pan_offset(mr);
    }
}

/* ── Position marker ── */

static void update_marker_position(map_renderer_t* mr)
{
    if (!mr->pos_valid || !mr->marker_img) return;

    double ptx, pty;
    lat_lon_to_tile(mr->pos_lat, mr->pos_lon, mr->current_zoom, &ptx, &pty);

    /* Position in map_group pixel coordinates */
    int32_t mx = (int32_t)((ptx - mr->grid_origin_tx) * TILE_PX);
    int32_t my = (int32_t)((pty - mr->grid_origin_ty) * TILE_PX);

    /* Position the marker centered on this point */
    lv_obj_set_pos(mr->marker_img, mx - MARKER_SIZE / 2, my - MARKER_SIZE / 2);

    /* Clear marker canvas to transparent */
    memset(mr->marker_buf, 0, MARKER_SIZE * MARKER_SIZE * 4);

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

    uint32_t* pixels = (uint32_t*)mr->marker_buf;
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
    /* Reposition all tile widgets based on grid_origin + pan offset */
    int32_t base_x = -(int32_t)((mr->center_tx - mr->grid_origin_tx) * TILE_PX) + mr->vp_size / 2;
    int32_t base_y = -(int32_t)((mr->center_ty - mr->grid_origin_ty) * TILE_PX) + mr->vp_size / 2;

    for (int32_t i = 0; i < GRID_TILES; i++) {
        if (!mr->tile_widgets[i]) continue;
        int32_t col = i % GRID_COLS;
        int32_t row = i / GRID_COLS;
        lv_obj_set_pos(mr->tile_widgets[i], base_x + col * TILE_PX, base_y + row * TILE_PX);
    }
    if (mr->marker_img) update_marker_position(mr);
}

static void drag_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        check_grid_boundary(mr);
        prefetch_if_cached(mr);
        return;
    }
    if (code != LV_EVENT_PRESSING) return;

    lv_indev_t* indev = lv_indev_active();
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.x == 0 && vect.y == 0) return;

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

static void track_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    map_renderer_track(mr);
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
    lv_obj_set_style_bg_opa(mr->map_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mr->map_container, 0, 0);
    lv_obj_set_style_pad_all(mr->map_container, 0, 0);
    lv_obj_set_scrollbar_mode(mr->map_container, LV_SCROLLBAR_MODE_OFF);
    /* Don't use LVGL native scroll — it locks to one axis.
     * Instead use custom PRESSING handler for free diagonal pan. */
    lv_obj_add_flag(mr->map_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(mr->map_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* ── Tile images directly inside map_container (no intermediate group) ── */
    mr->map_group = mr->map_container;  /* alias — tiles are direct children */

    mr->marker_img = NULL;
    mr->marker_buf = NULL;

    for (int32_t i = 0; i < GRID_TILES; i++) {
        mr->tile_bufs[i] = (uint8_t*)malloc(TILE_BYTES);
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
        mr->tile_widgets[i] = lv_image_create(mr->map_container);
        lv_obj_set_pos(mr->tile_widgets[i], col * TILE_PX, row * TILE_PX);
        lv_image_set_src(mr->tile_widgets[i], &mr->tile_dbufs[i]);
        lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(mr->tile_widgets[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── Touch handling for drag ── */
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

    mr->track_btn = NULL;
    return mr;
}

void map_renderer_set_view(map_renderer_t* mr, double lat, double lon, int32_t zoom)
{
    mr->center_lat = lat;
    mr->center_lon = lon;
    mr->current_zoom = zoom;
    if (mr->current_zoom < ZOOM_MIN) mr->current_zoom = ZOOM_MIN;
    if (mr->current_zoom > ZOOM_MAX) mr->current_zoom = ZOOM_MAX;
    update_tile_coords(mr);

    /* Position grid centered on the view */
    mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
    mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;

    /* Sync-load visible tiles into cache */
    if (mr->cache) {
        for (int32_t row = 0; row < GRID_ROWS; row++)
            for (int32_t col = 0; col < GRID_COLS; col++) {
                tile_key_t key = { mr->path_id, (uint8_t)mr->current_zoom,
                                   (uint16_t)(mr->grid_origin_tx + col),
                                   (uint16_t)(mr->grid_origin_ty + row) };
                tile_cache_load_sync(mr->cache, key, mr->tile_base);
            }
    }

    load_grid_tiles(mr);
    center_scroll_on_view(mr);
    update_marker_position(mr);
    prefetch_if_cached(mr);
}

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
        update_center_from_scroll(mr);
        mr->current_zoom++;
        update_tile_coords(mr);
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        load_grid_tiles(mr);
        center_scroll_on_view(mr);
        update_marker_position(mr);
        prefetch_if_cached(mr);
    }
}

void map_renderer_zoom_out(map_renderer_t* mr)
{
    if (mr->current_zoom > ZOOM_MIN) {
        update_center_from_scroll(mr);
        mr->current_zoom--;
        update_tile_coords(mr);
        mr->grid_origin_tx = (int32_t)floor(mr->center_tx) - GRID_COLS / 2;
        mr->grid_origin_ty = (int32_t)floor(mr->center_ty) - GRID_ROWS / 2;
        load_grid_tiles(mr);
        center_scroll_on_view(mr);
        update_marker_position(mr);
        prefetch_if_cached(mr);
    }
}

void map_renderer_set_position(map_renderer_t* mr, double lat, double lon, float cog_deg)
{
    mr->pos_lat = lat;
    mr->pos_lon = lon;
    mr->pos_cog = cog_deg;
    mr->pos_valid = true;

    if (mr->tracking) {
        mr->center_lat = lat;
        mr->center_lon = lon;
        update_tile_coords(mr);
        center_scroll_on_view(mr);
        check_grid_boundary(mr);
    }

    update_marker_position(mr);
}

void map_renderer_track(map_renderer_t* mr)
{
    mr->tracking = true;
    show_track_btn(mr, false);
    if (mr->pos_valid) {
        mr->center_lat = mr->pos_lat;
        mr->center_lon = mr->pos_lon;
        update_tile_coords(mr);
        center_scroll_on_view(mr);
        check_grid_boundary(mr);
        update_marker_position(mr);
    }
}

void map_renderer_create_track_btn(map_renderer_t* mr, lv_obj_t* btn_parent,
    int32_t x_ofs, int32_t y_ofs)
{
    mr->track_btn = lv_button_create(btn_parent);
    lv_obj_set_size(mr->track_btn, 40, 40);
    lv_obj_align(mr->track_btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_radius(mr->track_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mr->track_btn, lv_color_hex(0x21262d), 0);
    lv_obj_set_style_bg_opa(mr->track_btn, LV_OPA_80, 0);
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
    lv_obj_set_style_bg_opa(mr->theme_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(mr->theme_btn, 0, 0);
    lv_obj_add_event_cb(mr->theme_btn, theme_btn_cb, LV_EVENT_CLICKED, mr);
    lv_obj_remove_flag(mr->theme_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(mr->theme_btn, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t* lbl = lv_label_create(mr->theme_btn);
    lv_label_set_text(lbl, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
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

    /* Pre-populate cache with grid tiles */
    for (int32_t row = 0; row < GRID_ROWS; row++)
        for (int32_t col = 0; col < GRID_COLS; col++) {
            tile_key_t key = { mr->path_id, (uint8_t)mr->current_zoom,
                               (uint16_t)(mr->grid_origin_tx + col),
                               (uint16_t)(mr->grid_origin_ty + row) };
            tile_cache_load_sync(mr->cache, key, mr->tile_base);
        }

    /* Reload grid now that cache is populated */
    load_grid_tiles(mr);
}

void map_renderer_render(map_renderer_t* mr)
{
    if (!mr || !mr->map_container) return;

    /* Fill in any tiles that were cache misses and have now loaded */
    bool any_updated = false;
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
            lv_obj_invalidate(mr->tile_widgets[i]);
            any_updated = true;
        }
    }
}

double map_renderer_get_lat(map_renderer_t* mr) { return mr->center_lat; }
double map_renderer_get_lon(map_renderer_t* mr) { return mr->center_lon; }
int32_t map_renderer_get_zoom(map_renderer_t* mr) { return mr->current_zoom; }
