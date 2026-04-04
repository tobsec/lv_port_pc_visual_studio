#include "map_renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TILE_PX      256
#define TILE_BYTES   (TILE_PX * TILE_PX * 2)
#define ZOOM_MIN     10
#define ZOOM_MAX     15
#define PATH_BUF_LEN 256

struct map_renderer {
    lv_obj_t*  canvas_obj;
    uint8_t*   canvas_buf;
    lv_obj_t*  parent;
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

    float      vignette_start;  /* 0.0 = disabled, else fraction from top where fade begins */
    uint16_t   vignette_color;  /* RGB565 color to fade toward (usually background) */

    uint8_t    tile_buf[TILE_BYTES];
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

static bool load_tile(map_renderer_t* mr, int32_t tx, int32_t ty, int32_t zoom)
{
    char path[PATH_BUF_LEN];
    snprintf(path, sizeof(path), "%s/%d/%d/%d.bin", mr->tile_base, zoom, tx, ty);

    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK) return false;

    uint32_t bytes_read = 0;
    lv_fs_res_t res = lv_fs_read(&f, mr->tile_buf, TILE_BYTES, &bytes_read);
    lv_fs_close(&f);
    return (res == LV_FS_RES_OK && bytes_read == TILE_BYTES);
}

static void fill_rect(map_renderer_t* mr, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t col)
{
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = (x + w) > mr->vp_size ? mr->vp_size : (x + w);
    int32_t y1 = (y + h) > mr->vp_size ? mr->vp_size : (y + h);
    uint16_t* px = (uint16_t*)mr->canvas_buf;
    for (int32_t r = y0; r < y1; r++)
        for (int32_t c = x0; c < x1; c++)
            px[r * mr->vp_size + c] = col;
}

static void blit_tile(map_renderer_t* mr, int32_t src_x, int32_t src_y,
    int32_t dst_x, int32_t dst_y, int32_t w, int32_t h)
{
    if (dst_x < 0) { src_x -= dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y -= dst_y; h += dst_y; dst_y = 0; }
    if (dst_x + w > mr->vp_size) w = mr->vp_size - dst_x;
    if (dst_y + h > mr->vp_size) h = mr->vp_size - dst_y;
    if (w <= 0 || h <= 0) return;

    const uint16_t* src = (const uint16_t*)mr->tile_buf;
    uint16_t* dst = (uint16_t*)mr->canvas_buf;
    for (int32_t row = 0; row < h; row++)
        memcpy(&dst[(dst_y + row) * mr->vp_size + dst_x],
               &src[(src_y + row) * TILE_PX + src_x], w * 2);
}

/* ── Render ── */

#define MISSING_COLOR 0x0883

static void draw_marker(map_renderer_t* mr)
{
    if (!mr->pos_valid) return;

    double ptx, pty;
    lat_lon_to_tile(mr->pos_lat, mr->pos_lon, mr->current_zoom, &ptx, &pty);

    double frac_x = mr->center_tx - floor(mr->center_tx);
    double frac_y = mr->center_ty - floor(mr->center_ty);
    int32_t half = mr->vp_size / 2;
    int32_t origin_x = half - (int32_t)(frac_x * TILE_PX);
    int32_t origin_y = half - (int32_t)(frac_y * TILE_PX);
    int32_t ctx = (int32_t)floor(mr->center_tx);
    int32_t cty = (int32_t)floor(mr->center_ty);

    int32_t mx = origin_x + (int32_t)((ptx - ctx) * TILE_PX);
    int32_t my = origin_y + (int32_t)((pty - cty) * TILE_PX);

    float rad = (mr->pos_cog - 90.0f) * (float)M_PI / 180.0f;
    float cs = cosf(rad), sn = sinf(rad);

    int32_t tip_x  = mx + (int32_t)(10 * cs);
    int32_t tip_y  = my + (int32_t)(10 * sn);
    int32_t left_x = mx + (int32_t)(-6 * cs - (-5) * sn);
    int32_t left_y = my + (int32_t)(-6 * sn + (-5) * cs);
    int32_t right_x = mx + (int32_t)(-6 * cs - 5 * sn);
    int32_t right_y = my + (int32_t)(-6 * sn + 5 * cs);

    uint16_t* pixels = (uint16_t*)mr->canvas_buf;
    uint16_t col = 0xFFE0; /* yellow */

    #define DRAW_LINE(x0,y0,x1,y1,c) do { \
        int32_t _dx=abs((x1)-(x0)),_sx=(x0)<(x1)?1:-1; \
        int32_t _dy=-abs((y1)-(y0)),_sy=(y0)<(y1)?1:-1; \
        int32_t _err=_dx+_dy,_e2; \
        int32_t _cx=(x0),_cy=(y0); \
        for(;;){ \
            if(_cx>=0&&_cx<mr->vp_size&&_cy>=0&&_cy<mr->vp_size) \
                pixels[_cy*mr->vp_size+_cx]=(c); \
            if(_cx==(x1)&&_cy==(y1))break; \
            _e2=2*_err; \
            if(_e2>=_dy){_err+=_dy;_cx+=_sx;} \
            if(_e2<=_dx){_err+=_dx;_cy+=_sy;} \
        } \
    } while(0)

    DRAW_LINE(tip_x, tip_y, left_x, left_y, 0x0000);
    DRAW_LINE(left_x, left_y, right_x, right_y, 0x0000);
    DRAW_LINE(right_x, right_y, tip_x, tip_y, 0x0000);
    for (int32_t i = 0; i <= 10; i++) {
        int32_t lx = left_x + (right_x - left_x) * i / 10;
        int32_t ly = left_y + (right_y - left_y) * i / 10;
        DRAW_LINE(lx, ly, tip_x, tip_y, col);
    }
    #undef DRAW_LINE
}

static void render(map_renderer_t* mr)
{
    double frac_x = mr->center_tx - floor(mr->center_tx);
    double frac_y = mr->center_ty - floor(mr->center_ty);
    int32_t ctx = (int32_t)floor(mr->center_tx);
    int32_t cty = (int32_t)floor(mr->center_ty);
    int32_t half = mr->vp_size / 2;
    int32_t origin_x = half - (int32_t)(frac_x * TILE_PX);
    int32_t origin_y = half - (int32_t)(frac_y * TILE_PX);

    int32_t tiles_left  = (origin_x / TILE_PX) + 1;
    int32_t tiles_right = ((mr->vp_size - origin_x) / TILE_PX) + 1;
    int32_t tiles_up    = (origin_y / TILE_PX) + 1;
    int32_t tiles_down  = ((mr->vp_size - origin_y) / TILE_PX) + 1;

    for (int32_t dy = -tiles_up; dy <= tiles_down; dy++) {
        for (int32_t dx = -tiles_left; dx <= tiles_right; dx++) {
            int32_t px = origin_x + dx * TILE_PX;
            int32_t py = origin_y + dy * TILE_PX;
            if (px + TILE_PX <= 0 || px >= mr->vp_size ||
                py + TILE_PX <= 0 || py >= mr->vp_size) continue;

            if (load_tile(mr, ctx + dx, cty + dy, mr->current_zoom))
                blit_tile(mr, 0, 0, px, py, TILE_PX, TILE_PX);
            else
                fill_rect(mr, px, py, TILE_PX, TILE_PX, MISSING_COLOR);
        }
    }

    draw_marker(mr);

    /* Apply vignette: radial edge fade + vertical bottom fade */
    if (mr->vignette_start > 0.0f) {
        int32_t half = mr->vp_size / 2;
        int32_t fade_y = (int32_t)(mr->vp_size * mr->vignette_start);
        int32_t fade_len = mr->vp_size - fade_y;
        /* Radial fade: fully visible inside 88% of radius, fade to black at edge */
        int32_t r_inner = half * 88 / 100;
        int32_t r_outer = half;
        int32_t r_range = r_outer - r_inner;

        uint16_t* pixels = (uint16_t*)mr->canvas_buf;
        for (int32_t y = 0; y < mr->vp_size; y++) {
            for (int32_t x = 0; x < mr->vp_size; x++) {
                int32_t dx = x - half;
                int32_t dy = y - half;

                /* Radial alpha (edge fade) */
                /* Use integer sqrt approximation: dist² compared to r² thresholds */
                int32_t dist_sq = dx * dx + dy * dy;
                int32_t radial_alpha = 255;
                if (dist_sq >= r_outer * r_outer) {
                    radial_alpha = 0;
                } else if (dist_sq > r_inner * r_inner) {
                    /* Linear fade between r_inner and r_outer */
                    /* Approximate: use dist_sq vs r_sq for smoother curve */
                    int32_t inner_sq = r_inner * r_inner;
                    int32_t outer_sq = r_outer * r_outer;
                    radial_alpha = 255 - (dist_sq - inner_sq) * 255 / (outer_sq - inner_sq);
                }

                /* Vertical alpha (bottom fade) */
                int32_t vert_alpha = 255;
                if (fade_len > 0 && y > fade_y) {
                    vert_alpha = 255 - (y - fade_y) * 255 / fade_len;
                    if (vert_alpha < 0) vert_alpha = 0;
                }

                /* Combined alpha */
                int32_t alpha = radial_alpha * vert_alpha / 255;
                if (alpha >= 255) continue;

                uint16_t px = pixels[y * mr->vp_size + x];
                /* Lerp: result = src * alpha + target * (255 - alpha) */
                int32_t inv = 255 - alpha;
                int32_t tr = (mr->vignette_color >> 11) & 0x1F;
                int32_t tg = (mr->vignette_color >> 5) & 0x3F;
                int32_t tb = mr->vignette_color & 0x1F;
                int32_t r = (((px >> 11) & 0x1F) * alpha + tr * inv) / 255;
                int32_t g = (((px >> 5) & 0x3F) * alpha + tg * inv) / 255;
                int32_t b = ((px & 0x1F) * alpha + tb * inv) / 255;
                pixels[y * mr->vp_size + x] = (r << 11) | (g << 5) | b;
            }
        }
    }

    lv_obj_invalidate(mr->canvas_obj);
}

/* ── Input handling ── */

static void show_track_btn(map_renderer_t* mr, bool show)
{
    if (mr->track_btn) {
        if (show) lv_obj_remove_flag(mr->track_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(mr->track_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void chart_event_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;

    lv_indev_t* indev = lv_indev_active();
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.x != 0 || vect.y != 0) {
        mr->tracking = false;
        show_track_btn(mr, true);
        map_renderer_pan(mr, -vect.x, -vect.y);
    }
}

static void track_btn_cb(lv_event_t* e)
{
    map_renderer_t* mr = (map_renderer_t*)lv_event_get_user_data(e);
    map_renderer_track(mr);
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
    mr->vp_size = viewport_size;
    mr->tracking = true;
    mr->current_zoom = 13;
    mr->parent = parent;

    mr->canvas_buf = (uint8_t*)lv_malloc(viewport_size * viewport_size * 2);
    if (!mr->canvas_buf) { lv_free(mr); return NULL; }

    mr->canvas_obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(mr->canvas_obj, mr->canvas_buf, viewport_size, viewport_size, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(mr->canvas_obj);
    lv_obj_remove_flag(mr->canvas_obj, LV_OBJ_FLAG_CLICKABLE);

    /* Drag to pan */
    lv_obj_add_event_cb(parent, chart_event_cb, LV_EVENT_PRESSING, mr);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_CHAIN);

    /* Track button is not created here — call map_renderer_create_track_btn
     * on the parent's parent (root/tile) so it sits above the chart_area */
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
    render(mr);
}

void map_renderer_pan(map_renderer_t* mr, int32_t dx, int32_t dy)
{
    double scale = 1.0 / TILE_PX;
    mr->center_tx += dx * scale;
    mr->center_ty += dy * scale;
    tile_to_lat_lon(mr->center_tx, mr->center_ty, mr->current_zoom, &mr->center_lat, &mr->center_lon);
    render(mr);
}

void map_renderer_zoom_in(map_renderer_t* mr)
{
    if (mr->current_zoom < ZOOM_MAX) {
        mr->current_zoom++;
        update_tile_coords(mr);
        render(mr);
    }
}

void map_renderer_zoom_out(map_renderer_t* mr)
{
    if (mr->current_zoom > ZOOM_MIN) {
        mr->current_zoom--;
        update_tile_coords(mr);
        render(mr);
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
    }
    render(mr);
}

void map_renderer_track(map_renderer_t* mr)
{
    mr->tracking = true;
    show_track_btn(mr, false);
    if (mr->pos_valid) {
        mr->center_lat = mr->pos_lat;
        mr->center_lon = mr->pos_lon;
        update_tile_coords(mr);
        render(mr);
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

void map_renderer_set_vignette(map_renderer_t* mr, float fade_start_pct, lv_color_t color)
{
    mr->vignette_start = fade_start_pct;
    /* Convert lv_color_t to RGB565 */
    mr->vignette_color = ((color.red >> 3) << 11) |
                         ((color.green >> 2) << 5) |
                          (color.blue >> 3);
}

double map_renderer_get_lat(map_renderer_t* mr) { return mr->center_lat; }
double map_renderer_get_lon(map_renderer_t* mr) { return mr->center_lon; }
int32_t map_renderer_get_zoom(map_renderer_t* mr) { return mr->current_zoom; }
