#include "golden_screen.h"
#include "map_renderer.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Define COMET_DISABLE to replace the expensive canvas comet with a
 * lightweight LVGL arc needle — useful for performance profiling. */
#define COMET_DISABLE  /* ring-band needle — better FPS than comet canvas */

/* ── Layout constants ── */
#define DISP_SIZE       800
#define RPM_SCALE_SIZE  780
#define RPM_MAX         6000
#define RPM_YELLOW      4800
#define RPM_REDLINE     5200

/* Comet tail gauge constants */
#define COMET_SIZE         780
#define COMET_NEEDLE_WIDTH 5      /* core needle thickness */
#define COMET_GLOW_WIDTH   14     /* glow halo thickness (each side of needle) */
#define COMET_RADIUS_OUTER 378    /* outer end (near scale ticks) */
#define COMET_RADIUS_INNER 310    /* inner end (near map edge) */
#define COMET_SWEEP_DEG    70.0f

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
#ifdef COMET_DISABLE
static lv_obj_t* needle_line;       /* standalone lv_line in the ring band */
static lv_point_precise_t needle_pts[2];  /* persistent — lv_line stores the pointer */
#else
static lv_obj_t* comet_canvas;
static uint16_t* comet_buf;
static int32_t   comet_last_rpm = -1;
static lv_area_t comet_dirty;       /* bounding box of last drawn comet */
static lv_draw_buf_t* scale_overlay; /* pre-rendered scale ticks/labels (ARGB8888) */
#endif

/* Comet colors */
#define COL_COMET_BLUE   lv_color_hex(0x4488ff)
#define COL_COMET_WHITE  lv_color_hex(0xe0e8ff)
#define COL_COMET_AMBER  lv_color_hex(0xffaa00)

static lv_obj_t* sog_label;
static lv_obj_t* sog_unit_label;
static lv_obj_t* depth_label;
static lv_obj_t* nav_sep;
static lv_obj_t* watertemp_label;
/* Pod widgets: each pod has a status dot, value label, and bar */
typedef struct {
    lv_obj_t* pod;    /* container */
    lv_obj_t* dot;    /* status indicator (unused) */
    lv_obj_t* val;    /* value label */
    lv_obj_t* unit;   /* unit label (smaller, dimmer) */
    lv_obj_t* bar;    /* thin range bar */
    int8_t    alert;  /* last applied state: 0 uninit, 1 normal, 2 yellow, 3 red */
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
    lv_style_set_text_color(&style_section_yellow_indicator, COL_YELLOW);

    lv_style_init(&style_section_yellow_items);
    lv_style_set_line_color(&style_section_yellow_items, COL_YELLOW);

    /* Red section: 5200-6000 RPM */
    lv_style_init(&style_section_red_main);
    lv_style_set_arc_color(&style_section_red_main, COL_RED);

    lv_style_init(&style_section_red_indicator);
    lv_style_set_line_color(&style_section_red_indicator, COL_RED);
    lv_style_set_text_color(&style_section_red_indicator, COL_RED);

    lv_style_init(&style_section_red_items);
    lv_style_set_line_color(&style_section_red_items, COL_RED);
}

#ifndef COMET_DISABLE
/* ── Comet tail helpers ── */

static uint16_t color_to_565(lv_color_t c)
{
    return ((c.red >> 3) << 11) | ((c.green >> 2) << 5) | (c.blue >> 3);
}

static uint16_t bg_565;  /* pre-computed COL_BG as RGB565 */

/* Convert RPM to angle in degrees (0=3 o'clock, CW) */
static float rpm_to_angle(float rpm)
{
    if (rpm < 0) rpm = 0;
    if (rpm > RPM_MAX) rpm = RPM_MAX;
    return 135.0f + (rpm / (float)RPM_MAX) * 270.0f;
}

/* Stateful comet color based on RPM — HSV for blue→white, RGB lerp for the rest */
static lv_color_t comet_color_for_rpm(float rpm)
{
    if (rpm < 1000.0f) {
        /* Blue → Cyan → White via HSV-like transition */
        float t = rpm / 1000.0f;
        /* Saturation drops 1.0→0.0, value stays high, hue 220→200 */
        uint8_t r = (uint8_t)(0x44 + (0xe0 - 0x44) * t);
        uint8_t g = (uint8_t)(0x88 + (0xe8 - 0x88) * t);
        uint8_t b = (uint8_t)(0xff + (0xff - 0xff) * t);
        return lv_color_make(r, g, b);
    } else if (rpm < 4000.0f) {
        return lv_color_hex(0xe0e8ff); /* white-blue cruise */
    } else if (rpm < 4800.0f) {
        float t = (rpm - 4000.0f) / 800.0f;
        return lv_color_mix(lv_color_hex(0xffaa00), lv_color_hex(0xe0e8ff), (uint8_t)(t * 255));
    } else if (rpm < 5200.0f) {
        float t = (rpm - 4800.0f) / 400.0f;
        return lv_color_mix(lv_color_hex(0xffd600), lv_color_hex(0xffaa00), (uint8_t)(t * 255));
    } else {
        float t = (rpm - 5200.0f) / 800.0f;
        if (t > 1.0f) t = 1.0f;
        return lv_color_mix(lv_color_hex(0xff1744), lv_color_hex(0xffd600), (uint8_t)(t * 255));
    }
}

static void clear_last_comet(void)
{
    if (comet_dirty.x2 <= comet_dirty.x1) return;
    for (int32_t y = comet_dirty.y1; y <= comet_dirty.y2; y++) {
        for (int32_t x = comet_dirty.x1; x <= comet_dirty.x2; x++) {
            comet_buf[y * COMET_SIZE + x] = bg_565;
        }
    }
}

/* Overlay pre-rendered scale ticks/labels from ARGB8888 snapshot onto
 * the comet canvas.  Called after comet drawing to restore scale pixels
 * that were cleared or overwritten by the comet gradient. */
static void overlay_scale(lv_area_t* area)
{
    if (!scale_overlay || !scale_overlay->data) return;
    const uint8_t* src = scale_overlay->data;
    uint32_t stride = scale_overlay->header.stride;
    int32_t snap_w = scale_overlay->header.w;
    int32_t snap_h = scale_overlay->header.h;

    /* Center the snapshot within the canvas (may differ by padding) */
    int32_t off_x = (COMET_SIZE - snap_w) / 2;
    int32_t off_y = (COMET_SIZE - snap_h) / 2;

    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= COMET_SIZE ? COMET_SIZE - 1 : area->x2;
    int32_t y2 = area->y2 >= COMET_SIZE ? COMET_SIZE - 1 : area->y2;

    for (int32_t y = y1; y <= y2; y++) {
        int32_t sy = y - off_y;
        if (sy < 0 || sy >= snap_h) continue;
        const uint8_t* row = src + sy * stride;
        for (int32_t x = x1; x <= x2; x++) {
            int32_t sx = x - off_x;
            if (sx < 0 || sx >= snap_w) continue;
            const uint8_t* px = row + sx * 4;  /* BGRA in memory */
            uint8_t alpha = px[3];
            if (alpha == 0) continue;

            if (alpha >= 250) {
                /* Opaque: direct conversion RGB888 → RGB565 */
                comet_buf[y * COMET_SIZE + x] =
                    ((px[2] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[0] >> 3);
            } else {
                /* Semi-transparent: alpha blend onto canvas pixel */
                uint16_t dst = comet_buf[y * COMET_SIZE + x];
                uint32_t inv = 255 - alpha;
                uint32_t r = (px[2] * alpha + (((dst >> 11) & 0x1F) << 3) * inv) / 255;
                uint32_t g = (px[1] * alpha + (((dst >> 5) & 0x3F) << 2) * inv) / 255;
                uint32_t b = (px[0] * alpha + ((dst & 0x1F) << 3) * inv) / 255;
                comet_buf[y * COMET_SIZE + x] =
                    ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
}

static void comet_redraw(float rpm)
{
    /* Save the old dirty rect before clearing — we need to invalidate it too */
    lv_area_t old_dirty = comet_dirty;

    /* Clear previous frame's dirty area */
    clear_last_comet();

    /* Reset dirty rect to impossible values */
    comet_dirty.x1 = COMET_SIZE;
    comet_dirty.y1 = COMET_SIZE;
    comet_dirty.x2 = 0;
    comet_dirty.y2 = 0;

    if (rpm < 50.0f) {
        /* Invalidate the old (now cleared) area, restore scale pixels */
        if (old_dirty.x2 > old_dirty.x1) {
            overlay_scale(&old_dirty);
            lv_area_t screen_area;
            int32_t ox = lv_obj_get_x(comet_canvas);
            int32_t oy = lv_obj_get_y(comet_canvas);
            screen_area.x1 = old_dirty.x1 + ox;
            screen_area.y1 = old_dirty.y1 + oy;
            screen_area.x2 = old_dirty.x2 + ox;
            screen_area.y2 = old_dirty.y2 + oy;
            lv_obj_invalidate_area(comet_canvas, &screen_area);
        }
        return;
    }

    float head_angle = rpm_to_angle(rpm);
    lv_color_t head_color = comet_color_for_rpm(rpm);
    uint16_t head_565 = color_to_565(head_color);

    /* Dark blue tail color */
    lv_color_t tail_color = lv_color_hex(0x112244);
    /* Single continuous comet arc with per-pixel angular gradient.
     * Instead of discrete segments, draw one arc from tail to head
     * and compute opacity per pixel based on angular distance from head. */
    {
        float tail_angle = head_angle - COMET_SWEEP_DEG;
        if (tail_angle < 135.0f) tail_angle = 135.0f;  /* clamp to 0 RPM position */

        int32_t cx = COMET_SIZE / 2;
        int32_t cy = COMET_SIZE / 2;
        int32_t r2_outer = COMET_RADIUS_OUTER * COMET_RADIUS_OUTER;
        int32_t r2_inner = COMET_RADIUS_INNER * COMET_RADIUS_INNER;

        /* Pre-compute boundary vectors for the (clamped) sweep */
        float a_tail = tail_angle * (float)M_PI / 180.0f;
        float a_head = head_angle * (float)M_PI / 180.0f;
        float cos_tail = cosf(a_tail), sin_tail = sinf(a_tail);
        float cos_head = cosf(a_head), sin_head = sinf(a_head);

        /* Pre-convert tail color */
        uint16_t tail_565 = color_to_565(tail_color);
        uint32_t tail_r = (tail_565 >> 11) & 0x1F;
        uint32_t tail_g = (tail_565 >> 5) & 0x3F;
        uint32_t tail_b = tail_565 & 0x1F;
        uint32_t head_r = (head_565 >> 11) & 0x1F;
        uint32_t head_g = (head_565 >> 5) & 0x3F;
        uint32_t head_b = head_565 & 0x1F;
        uint32_t bg_r = (bg_565 >> 11) & 0x1F;
        uint32_t bg_g = (bg_565 >> 5) & 0x3F;
        uint32_t bg_b = bg_565 & 0x1F;

        /* Use cross-product magnitude ratio to approximate angular position
         * within the sweep, avoiding atan2f entirely.
         *
         * For a pixel P, cross_tail = |Va x P| tells us "how far past the tail
         * boundary" the pixel is. Similarly cross_from_head = |Vh x P| tells us
         * how far before the head. The ratio cross_tail / (cross_tail + cross_from_head)
         * gives a smooth 0→1 interpolant across the sweep. */

        /* Compute tight bounding box from arc geometry */
        float a_mid = (a_tail + a_head) * 0.5f;
        float a_quarter1 = (a_tail + a_mid) * 0.5f;
        float a_quarter3 = (a_mid + a_head) * 0.5f;
        float sample_angles[] = { a_tail, a_quarter1, a_mid, a_quarter3, a_head };
        int32_t bb_x1 = cx, bb_x2 = cx, bb_y1 = cy, bb_y2 = cy;
        for (int i = 0; i < 5; i++) {
            int32_t sx = cx + (int32_t)(COMET_RADIUS_OUTER * cosf(sample_angles[i]));
            int32_t sy = cy + (int32_t)(COMET_RADIUS_OUTER * sinf(sample_angles[i]));
            if (sx < bb_x1) { bb_x1 = sx; } if (sx > bb_x2) { bb_x2 = sx; }
            if (sy < bb_y1) { bb_y1 = sy; } if (sy > bb_y2) { bb_y2 = sy; }
            sx = cx + (int32_t)(COMET_RADIUS_INNER * cosf(sample_angles[i]));
            sy = cy + (int32_t)(COMET_RADIUS_INNER * sinf(sample_angles[i]));
            if (sx < bb_x1) { bb_x1 = sx; } if (sx > bb_x2) { bb_x2 = sx; }
            if (sy < bb_y1) { bb_y1 = sy; } if (sy > bb_y2) { bb_y2 = sy; }
        }
        /* Add margin for the ring width */
        bb_x1 -= 2; bb_y1 -= 2; bb_x2 += 2; bb_y2 += 2;
        if (bb_x1 < 0) bb_x1 = 0;
        if (bb_y1 < 0) bb_y1 = 0;
        if (bb_x2 >= COMET_SIZE) bb_x2 = COMET_SIZE - 1;
        if (bb_y2 >= COMET_SIZE) bb_y2 = COMET_SIZE - 1;

        /* Scanline ring iteration — compute exact ring x-ranges per row
         * to skip all pixels inside the inner circle.  Cross products use
         * incremental adds (ct -= sin_tail, ch += sin_head) instead of
         * per-pixel multiplies.  Dirty rect tracked per row, not per pixel. */
        for (int32_t y = bb_y1; y <= bb_y2; y++) {
            int32_t dy = y - cy;
            int32_t dy2 = dy * dy;
            if (dy2 > r2_outer) continue;

            /* Ring x-extent for this scanline */
            int32_t rem_o = r2_outer - dy2;
            int32_t x_outer = (int32_t)sqrtf((float)rem_o);
            if (x_outer * x_outer > rem_o) x_outer--;
            else if ((x_outer + 1) * (x_outer + 1) <= rem_o) x_outer++;

            int32_t seg_x1[2], seg_x2[2];
            int nseg;
            if (dy2 < r2_inner) {
                /* Row crosses inner hole — two ring segments */
                int32_t rem_i = r2_inner - dy2;
                int32_t x_inner = (int32_t)sqrtf((float)rem_i);
                if (x_inner * x_inner < rem_i) x_inner++;
                seg_x1[0] = cx - x_outer; seg_x2[0] = cx - x_inner;
                seg_x1[1] = cx + x_inner; seg_x2[1] = cx + x_outer;
                nseg = 2;
            } else {
                /* Row at or past inner edge — one continuous span */
                seg_x1[0] = cx - x_outer; seg_x2[0] = cx + x_outer;
                nseg = 1;
            }

            /* Per-row cross-product base values */
            float ct_row = cos_tail * (float)dy;
            float ch_row = -(float)dy * cos_head;
            int32_t row_x_min = COMET_SIZE, row_x_max = 0;
            uint16_t* row_ptr = &comet_buf[y * COMET_SIZE];

            for (int s = 0; s < nseg; s++) {
                int32_t x1 = seg_x1[s] < bb_x1 ? bb_x1 : seg_x1[s];
                int32_t x2 = seg_x2[s] > bb_x2 ? bb_x2 : seg_x2[s];
                if (x1 > x2) continue;

                /* Seed cross products at x1, then increment per step */
                float dx0 = (float)(x1 - cx);
                float ct = ct_row - sin_tail * dx0;
                float ch = ch_row + sin_head * dx0;

                for (int32_t x = x1; x <= x2; x++) {
                    if (ct >= 0.0f && ch >= 0.0f) {
                        float t = ct / (ct + ch);
                        uint32_t alpha = (uint32_t)(255.0f * t * t);
                        uint32_t t256 = (uint32_t)(t * 256);
                        uint32_t it256 = 256 - t256;
                        uint32_t cr = (tail_r * it256 + head_r * t256) >> 8;
                        uint32_t cg = (tail_g * it256 + head_g * t256) >> 8;
                        uint32_t cb = (tail_b * it256 + head_b * t256) >> 8;
                        uint32_t inv = 255 - alpha;
                        uint32_t r = (cr * alpha + bg_r * inv) >> 8;
                        uint32_t g = (cg * alpha + bg_g * inv) >> 8;
                        uint32_t b = (cb * alpha + bg_b * inv) >> 8;
                        row_ptr[x] = (uint16_t)((r << 11) | (g << 5) | b);

                        if (x < row_x_min) row_x_min = x;
                        if (x > row_x_max) row_x_max = x;
                    }
                    ct -= sin_tail;
                    ch += sin_head;
                }
            }

            /* Track dirty rect at row granularity */
            if (row_x_max >= row_x_min) {
                if (row_x_min < comet_dirty.x1) comet_dirty.x1 = row_x_min;
                if (row_x_max > comet_dirty.x2) comet_dirty.x2 = row_x_max;
                if (y < comet_dirty.y1) comet_dirty.y1 = y;
                if (y > comet_dirty.y2) comet_dirty.y2 = y;
            }
        }
    }

    /* Invalidate the union of old (cleared) + new (drawn) dirty rects */
    int32_t ox = lv_obj_get_x(comet_canvas);
    int32_t oy = lv_obj_get_y(comet_canvas);

    /* Merge old and new dirty rects into one invalidation area */
    lv_area_t merged;
    merged.x1 = comet_dirty.x1;
    merged.y1 = comet_dirty.y1;
    merged.x2 = comet_dirty.x2;
    merged.y2 = comet_dirty.y2;
    if (old_dirty.x2 > old_dirty.x1) {
        if (old_dirty.x1 < merged.x1) merged.x1 = old_dirty.x1;
        if (old_dirty.y1 < merged.y1) merged.y1 = old_dirty.y1;
        if (old_dirty.x2 > merged.x2) merged.x2 = old_dirty.x2;
        if (old_dirty.y2 > merged.y2) merged.y2 = old_dirty.y2;
    }

    if (merged.x2 > merged.x1) {
        overlay_scale(&merged);
        lv_area_t screen_area;
        screen_area.x1 = merged.x1 + ox;
        screen_area.y1 = merged.y1 + oy;
        screen_area.x2 = merged.x2 + ox;
        screen_area.y2 = merged.y2 + oy;
        lv_obj_invalidate_area(comet_canvas, &screen_area);
    }
}
#endif /* !COMET_DISABLE */

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
    const char* label_text, const char* init_val, const char* unit_text,
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
    lv_obj_align(val, LV_ALIGN_TOP_MID, -8, 6);
    out->val = val;

    /* Unit — smaller, dimmer, positioned after the value */
    lv_obj_t* unt = lv_label_create(pod);
    lv_label_set_text(unt, unit_text);
    lv_obj_set_style_text_color(unt, lv_color_hex(0x6e7681), 0);
    lv_obj_set_style_text_font(unt, &lv_font_montserrat_14, 0);
    lv_obj_align_to(unt, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -2);
    out->unit = unt;

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

/* Update pod alert state: changes border, value text, bar, and dot color.
 * Skips re-applying the same state (avoids needless invalidation/redraw). */
static void pod_set_alert(gauge_pod_t* p, lv_color_t color)
{
    int8_t lvl = lv_color_eq(color, COL_RED) ? 3 : 2;   /* red=3, yellow=2 */
    if (p->alert == lvl) return;
    p->alert = lvl;
    lv_obj_set_style_border_color(p->pod, color, 0);
    lv_obj_set_style_text_color(p->val, color, 0);
    lv_obj_set_style_bg_color(p->bar, color, LV_PART_INDICATOR);
}

/* Update pod value and realign unit label (skips when text is unchanged). */
static void pod_update_val(gauge_pod_t* p, const char* text)
{
    const char* cur = lv_label_get_text(p->val);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(p->val, text);
    if (p->unit) lv_obj_align_to(p->unit, p->val, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -2);
}

static void pod_set_normal(gauge_pod_t* p)
{
    if (p->alert == 1) return;
    p->alert = 1;
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
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

/* Pod showing stale data: dash placeholder, cleared bar, normal styling. */
static void pod_show_dash(gauge_pod_t* p, const char* dash)
{
    lv_bar_set_value(p->bar, 0, LV_ANIM_OFF);
    pod_update_val(p, dash);
    pod_set_normal(p);
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

#ifndef COMET_DISABLE
    /* ════════════════════════════════════════════
     *  COMET CANVAS — z-index 0, RGB565, comet tail in outer ring
     * ════════════════════════════════════════════ */
    bg_565 = color_to_565(COL_BG);
    comet_buf = (uint16_t*)lv_malloc(COMET_SIZE * COMET_SIZE * 2);
    if (comet_buf) {
        /* Fill with background color */
        for (int32_t i = 0; i < COMET_SIZE * COMET_SIZE; i++)
            comet_buf[i] = bg_565;

        comet_canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(comet_canvas, comet_buf, COMET_SIZE, COMET_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(comet_canvas);
        /* Make comet canvas completely invisible to input system */
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(comet_canvas, LV_OBJ_FLAG_SCROLL_CHAIN);

        comet_dirty.x1 = 0; comet_dirty.y1 = 0;
        comet_dirty.x2 = 0; comet_dirty.y2 = 0;
        comet_last_rpm = -1;
    }
#endif

    /* ════════════════════════════════════════════
     *  Chart — z-index 1, on top of comet canvas inner area
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

    if (tile_path) {
        gs_map = map_renderer_create(chart_area, tile_path, 600);
        if (gs_map) {
            /* View is set later by screen_manager once the cache exists, so tile
             * loading is async (no blocking SD reads during boot). */
            /* Vignette disabled — pixel-by-pixel alpha blend is too expensive on ESP32 */
            /* map_renderer_set_vignette(gs_map, 0.65f, COL_BG); */
            map_renderer_create_track_btn(gs_map, root, 220, 60);
            if (alt_tile_path) {
                map_renderer_set_alt_tiles(gs_map, alt_tile_path, root, -220, 60);
            }
        }
    }

    /* ════════════════════════════════════════════
     *  RPM SCALE — z-index 2, tick marks and labels on top of comet
     * ════════════════════════════════════════════ */
    init_section_styles();

    rpm_scale = lv_scale_create(root);
    lv_obj_set_size(rpm_scale, RPM_SCALE_SIZE, RPM_SCALE_SIZE);
    lv_obj_center(rpm_scale);
    lv_scale_set_mode(rpm_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(rpm_scale, 0, RPM_MAX);
    lv_scale_set_angle_range(rpm_scale, 270);
    lv_scale_set_rotation(rpm_scale, 135);

    lv_scale_set_total_tick_count(rpm_scale, 31);
    lv_scale_set_major_tick_every(rpm_scale, 5);
    lv_scale_set_label_show(rpm_scale, true);

    static const char* rpm_labels[] = {"0", "1", "2", "3", "4", "5", "6", NULL};
    lv_scale_set_text_src(rpm_scale, rpm_labels);

    lv_obj_set_style_line_color(rpm_scale, COL_TEXT_DIM, LV_PART_ITEMS);
    lv_obj_set_style_line_width(rpm_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(rpm_scale, 16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(rpm_scale, COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(rpm_scale, &lv_font_montserrat_40, LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(rpm_scale, 10, LV_PART_INDICATOR);  /* gap between tick and label */
    lv_obj_set_style_line_color(rpm_scale, COL_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(rpm_scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(rpm_scale, 28, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(rpm_scale, COL_ARC_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(rpm_scale, 7, LV_PART_MAIN);

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

    lv_scale_set_post_draw(rpm_scale, true);
    lv_obj_remove_flag(rpm_scale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(rpm_scale, LV_OBJ_FLAG_SCROLLABLE);

    /* Snapshot the scale to ARGB8888.  The comet path merges scale pixels
     * directly into the comet canvas (no separate LVGL image layer).
     * The needle path keeps a separate lv_image widget. */
    lv_obj_update_layout(rpm_scale);
    lv_draw_buf_t* scale_snap = lv_snapshot_take(rpm_scale, LV_COLOR_FORMAT_ARGB8888);
    if (scale_snap) {
        lv_obj_delete(rpm_scale);
        rpm_scale = NULL;
    }

#ifdef COMET_DISABLE
    /* Needle path: show scale as a separate static image */
    if (scale_snap) {
        lv_obj_t* scale_img = lv_image_create(root);
        lv_image_set_src(scale_img, scale_snap);
        lv_obj_set_size(scale_img, RPM_SCALE_SIZE, RPM_SCALE_SIZE);
        lv_obj_center(scale_img);
        lv_obj_remove_flag(scale_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(scale_img, LV_OBJ_FLAG_SCROLLABLE);
    }
#else
    /* Comet path: scale will be overlaid into the canvas buffer directly */
    scale_overlay = scale_snap;
    /* Draw initial scale onto canvas so it's visible before first comet_redraw */
    if (scale_overlay && comet_buf) {
        lv_area_t full = { 0, 0, COMET_SIZE - 1, COMET_SIZE - 1 };
        overlay_scale(&full);
    }
#endif

#ifdef COMET_DISABLE
    /* Standalone line needle in the ring band (radius 310→378) */
    needle_line = lv_line_create(root);
    lv_obj_set_style_line_color(needle_line, lv_color_white(), 0);
    lv_obj_set_style_line_width(needle_line, 4, 0);
    lv_obj_set_style_line_rounded(needle_line, true, 0);
    lv_obj_remove_flag(needle_line, LV_OBJ_FLAG_CLICKABLE);
    /* Initial position at 0 RPM (135°) */
    float init_rad = 135.0f * (float)M_PI / 180.0f;
    needle_pts[0].x = DISP_SIZE / 2 + (int32_t)(COMET_RADIUS_INNER * cosf(init_rad));
    needle_pts[0].y = DISP_SIZE / 2 + (int32_t)(COMET_RADIUS_INNER * sinf(init_rad));
    needle_pts[1].x = DISP_SIZE / 2 + (int32_t)(COMET_RADIUS_OUTER * cosf(init_rad));
    needle_pts[1].y = DISP_SIZE / 2 + (int32_t)(COMET_RADIUS_OUTER * sinf(init_rad));
    lv_line_set_points(needle_line, needle_pts, 2);
#endif

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
    lv_obj_align(sog_label, LV_ALIGN_CENTER, -15, 0);

    sog_unit_label = lv_label_create(sog_panel);
    lv_label_set_text(sog_unit_label, "kn");
    lv_obj_set_style_text_color(sog_unit_label, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sog_unit_label, &lv_font_montserrat_20, 0);
    lv_obj_align_to(sog_unit_label, sog_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);

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

    nav_sep = lv_label_create(nav_panel);
    lv_obj_t* sep = nav_sep;
    lv_label_set_text(sep, "|");
    lv_obj_set_style_text_color(sep, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sep, &lv_font_montserrat_24, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 4);

    depth_label = lv_label_create(nav_panel);
    lv_label_set_text(depth_label, "--- m");
    lv_obj_set_style_text_color(depth_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(depth_label, &lv_font_montserrat_24, 0);
    lv_obj_align_to(depth_label, sep, LV_ALIGN_OUT_LEFT_MID, -15, 0);

    watertemp_label = lv_label_create(nav_panel);
    lv_label_set_text(watertemp_label, "--- °C");
    lv_obj_set_style_text_color(watertemp_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(watertemp_label, &lv_font_montserrat_24, 0);
    lv_obj_align_to(watertemp_label, sep, LV_ALIGN_OUT_RIGHT_MID, 15, 0);

    /* ════════════════════════════════════════════
     *  Gauge pods — Row 1: 3 pods (OilT, OilP, CltT)
     *  145×58px each, 10px gaps, centered in 460px
     * ════════════════════════════════════════════ */
    int32_t pod_w1 = 145, pod_h1 = 72, pod_gap = 10;
    int32_t row1_total = pod_w1 * 3 + pod_gap * 2;
    int32_t row1_x = (DISP_SIZE - row1_total) / 2;
    int32_t row1_y = 545;

    create_pod(root, row1_x,                      row1_y, pod_w1, pod_h1,
        "OIL",   "92", "°C",  40, 150, 92,  &pod_oilt);
    create_pod(root, row1_x + pod_w1 + pod_gap,   row1_y, pod_w1, pod_h1,
        "PRESS", "4.2", "bar", 0, 600, 420, &pod_oilp);
    create_pod(root, row1_x + (pod_w1 + pod_gap)*2, row1_y, pod_w1, pod_h1,
        "CLT",   "72", "°C",  40, 110, 77,  &pod_clt);

    /* ════════════════════════════════════════════
     *  Gauge pods — Row 2: 3 pods (Lambda, CltP, Batt)
     *  130×55px each, narrower to follow circle curve
     * ════════════════════════════════════════════ */
    int32_t pod_w2 = 130, pod_h2 = 68;
    int32_t row2_total = pod_w2 * 3 + pod_gap * 2;
    int32_t row2_x = (DISP_SIZE - row2_total) / 2;
    int32_t row2_y = row1_y + pod_h1 + pod_gap;

    create_pod(root, row2_x,                      row2_y, pod_w2, pod_h2,
        "LAM",  "1.00", "",    70, 130, 100, &pod_lam);
    create_pod(root, row2_x + pod_w2 + pod_gap,   row2_y, pod_w2, pod_h2,
        "CP",   "55", "kPa",  0,  100, 55,  &pod_cltp);
    create_pod(root, row2_x + (pod_w2 + pod_gap)*2, row2_y, pod_w2, pod_h2,
        "BAT",  "14.1", "V",  110, 150, 141, &pod_batt);

    /* ════════════════════════════════════════════
     *  Bottom digital readouts
     *  Row 1: L1, L2, IAT, MAP (4 items)
     *  Row 2: FP, FC (2 items, centered lower in the circle)
     * ════════════════════════════════════════════ */
    int32_t rdout1_y = row2_y + pod_h2 + 17;
    int32_t rdout2_y = rdout1_y + 32;
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

#ifdef COMET_DISABLE
    /* Ring-band needle (relative coords keep invalidation tight; data is fed by
     * a Core-0 task so it stays smooth). Redraw only when the tip actually moves
     * >= ~1px — at this radius ~3 RPM — so it's smooth without sub-pixel churn. */
    {
        static float needle_last_rpm = -1000.0f;
        /* Hide the needle entirely when the RPM PDU (127488) has timed out. */
        if (!d->valid.engine_rapid) {
            lv_obj_add_flag(needle_line, LV_OBJ_FLAG_HIDDEN);
            needle_last_rpm = -1000.0f;
        } else if (lv_obj_has_flag(needle_line, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(needle_line, LV_OBJ_FLAG_HIDDEN);
            needle_last_rpm = -1000.0f;  /* force a redraw now that it is visible again */
        }
        if (d->valid.engine_rapid && fabsf(d->rpm - needle_last_rpm) >= 3.0f) {
            needle_last_rpm = d->rpm;

            float angle_deg = 135.0f + (d->rpm / (float)RPM_MAX) * 270.0f;
            float angle_rad = angle_deg * (float)M_PI / 180.0f;
            float cs = cosf(angle_rad), sn = sinf(angle_rad);
            int32_t cx = DISP_SIZE / 2, cy = DISP_SIZE / 2;
            int32_t x1 = cx + (int32_t)(COMET_RADIUS_INNER * cs);
            int32_t y1 = cy + (int32_t)(COMET_RADIUS_INNER * sn);
            int32_t x2 = cx + (int32_t)(COMET_RADIUS_OUTER * cs);
            int32_t y2 = cy + (int32_t)(COMET_RADIUS_OUTER * sn);

            /* Position line at bounding box origin, use relative points
             * so lv_line auto-size is only ~68×68 — not 400×400 */
            int32_t min_x = x1 < x2 ? x1 : x2;
            int32_t min_y = y1 < y2 ? y1 : y2;
            lv_obj_set_pos(needle_line, min_x, min_y);
            needle_pts[0].x = x1 - min_x;
            needle_pts[0].y = y1 - min_y;
            needle_pts[1].x = x2 - min_x;
            needle_pts[1].y = y2 - min_y;
            lv_line_set_points(needle_line, needle_pts, 2);

            if (d->rpm >= RPM_REDLINE)
                lv_obj_set_style_line_color(needle_line, COL_RED, 0);
            else if (d->rpm >= RPM_YELLOW)
                lv_obj_set_style_line_color(needle_line, COL_YELLOW, 0);
            else
                lv_obj_set_style_line_color(needle_line, lv_color_white(), 0);
        }
    }
#else
    /* Comet tail RPM gauge — throttled to reduce canvas redraw load */
    static uint32_t comet_frame = 0;
    int32_t rpm_int = (int32_t)d->rpm;
    if (rpm_int != comet_last_rpm && comet_buf && ++comet_frame >= 3) {
        comet_frame = 0;
        comet_redraw(d->rpm);
        comet_last_rpm = rpm_int;
    }
#endif

    /* Update map position — throttled to ~0.5Hz to reduce SD read + canvas redraw load */
    static uint32_t map_frame = 0;
    if (++map_frame >= 40) {
        map_frame = 0;
        if (gs_map && d->latitude != 0.0 && d->longitude != 0.0) {
            map_renderer_set_position(gs_map, d->latitude, d->longitude, d->cog_degrees);
        }
    }

    /* Stagger slow-changing value updates across frames to avoid invalidation spikes.
     * Each slot runs at ~4 Hz (every 5th call at 20 Hz input rate). */
    static uint32_t slow_slot = 0;
    slow_slot++;
    uint32_t phase = slow_slot % 5;

    if (phase == 0) {
        /* Nav values (each per its own PDU) */
        set_val(sog_label, d->valid.cogsog, "%.1f", d->sog_knots, "-.-");
        lv_obj_align_to(sog_unit_label, sog_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
        set_val(depth_label, d->valid.depth, "%.1f m", d->depth_m, "-.- m");
        lv_obj_align_to(depth_label, nav_sep, LV_ALIGN_OUT_LEFT_MID, -15, 0);
        set_val(watertemp_label, d->valid.water_temp, "%.0f °C", d->water_temp_c, "--- °C");
    } else if (phase == 1) {
        /* Oil temp + oil pressure pods (PGN 127489) */
        if (d->valid.engine_dyn) {
            lv_bar_set_value(pod_oilt.bar, (int32_t)d->oil_temp_c, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.0f", d->oil_temp_c);
            pod_update_val(&pod_oilt, buf);
            if (d->oil_temp_c > 125)      pod_set_alert(&pod_oilt, COL_RED);
            else if (d->oil_temp_c > 110)  pod_set_alert(&pod_oilt, COL_YELLOW);
            else                           pod_set_normal(&pod_oilt);

            lv_bar_set_value(pod_oilp.bar, (int32_t)d->oil_pressure_kpa, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.1f", d->oil_pressure_kpa / 100.0f);
            pod_update_val(&pod_oilp, buf);
            if (d->oil_pressure_kpa < 150)       pod_set_alert(&pod_oilp, COL_RED);
            else if (d->oil_pressure_kpa < 250)  pod_set_alert(&pod_oilp, COL_YELLOW);
            else                                 pod_set_normal(&pod_oilp);
        } else {
            pod_show_dash(&pod_oilt, "---");
            pod_show_dash(&pod_oilp, "-.-");
        }
    } else if (phase == 2) {
        /* Coolant temp (127489) + lambda (raw CAN) pods */
        if (d->valid.engine_dyn) {
            lv_bar_set_value(pod_clt.bar, (int32_t)d->coolant_temp_c, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.0f", d->coolant_temp_c);
            pod_update_val(&pod_clt, buf);
            if (d->coolant_temp_c > 77)       pod_set_alert(&pod_clt, COL_RED);
            else if (d->coolant_temp_c > 70)  pod_set_alert(&pod_clt, COL_YELLOW);
            else                              pod_set_normal(&pod_clt);
        } else {
            pod_show_dash(&pod_clt, "---");
        }

        if (d->valid.lambda1 || d->valid.lambda2) {
            float worst_lambda = d->lambda1 > d->lambda2 ? d->lambda1 : d->lambda2;
            lv_bar_set_value(pod_lam.bar, (int32_t)(worst_lambda * 100), LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.2f", worst_lambda);
            pod_update_val(&pod_lam, buf);
            if (worst_lambda < 0.85f || worst_lambda > 1.15f)       pod_set_alert(&pod_lam, COL_RED);
            else if (worst_lambda < 0.90f || worst_lambda > 1.10f)  pod_set_alert(&pod_lam, COL_YELLOW);
            else                                                    pod_set_normal(&pod_lam);
        } else {
            pod_show_dash(&pod_lam, "-.--");
        }
    } else if (phase == 3) {
        /* Coolant pressure + battery pods (PGN 127489) */
        if (d->valid.engine_dyn) {
            lv_bar_set_value(pod_cltp.bar, (int32_t)d->coolant_pressure_kpa, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.0f", d->coolant_pressure_kpa);
            pod_update_val(&pod_cltp, buf);
            if (d->coolant_pressure_kpa < 15)       pod_set_alert(&pod_cltp, COL_RED);
            else if (d->coolant_pressure_kpa < 25)  pod_set_alert(&pod_cltp, COL_YELLOW);
            else                                    pod_set_normal(&pod_cltp);

            lv_bar_set_value(pod_batt.bar, (int32_t)(d->battery_voltage * 10), LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%.1f", d->battery_voltage);
            pod_update_val(&pod_batt, buf);
            if (d->battery_voltage < 12.0f)       pod_set_alert(&pod_batt, COL_RED);
            else if (d->battery_voltage < 12.8f)  pod_set_alert(&pod_batt, COL_YELLOW);
            else                                  pod_set_normal(&pod_batt);
        } else {
            pod_show_dash(&pod_cltp, "---");
            pod_show_dash(&pod_batt, "-.-");
        }
    } else {
        /* Bottom readouts (each per its own PDU) */
        set_val(lbl_lambda1, d->valid.lambda1, "L1 %.2f", d->lambda1, "L1 -.--");
        set_val(lbl_lambda2, d->valid.lambda2, "L2 %.2f", d->lambda2, "L2 -.--");
        set_val(lbl_iat, d->valid.iat, "IAT %.0f°C", d->iat_c, "IAT ---°C");
        set_val(lbl_map, d->valid.engine_rapid, "MAP %.0f kPa", d->map_kpa, "MAP --- kPa");
        set_val(lbl_fp, d->valid.engine_dyn, "FP %.0f kPa", d->fuel_pressure_kpa, "FP --- kPa");
        set_val(lbl_fuel, d->valid.engine_dyn, "FC %.1f l/h", d->fuel_rate_lph, "FC -.- l/h");
    }

}

/* Debug: frame tick counter visible on screen to distinguish data stall from render lag */
static lv_obj_t* dbg_tick_label = NULL;

void golden_screen_show_tick_counter(lv_obj_t* parent)
{
    dbg_tick_label = lv_label_create(parent);
    lv_obj_set_style_text_color(dbg_tick_label, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(dbg_tick_label, &lv_font_montserrat_14, 0);
    lv_obj_align(dbg_tick_label, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(dbg_tick_label, "0");
}

void golden_screen_update_tick(void)
{
    if (!dbg_tick_label) return;
    static uint32_t tick = 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(++tick));
    lv_label_set_text(dbg_tick_label, buf);
}

map_renderer_t* golden_screen_get_map(void) { return gs_map; }
