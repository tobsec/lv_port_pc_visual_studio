#ifndef MAP_RENDERER_H
#define MAP_RENDERER_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for a map renderer instance */
typedef struct map_renderer map_renderer_t;

/**
 * Create a new map renderer instance.
 * @param parent       Parent object (the chart_area circle)
 * @param tile_base    Base path for tiles, e.g. "C:/Data/marine-gauge/tools/MAP_BIN"
 * @param viewport_size  Diameter of the circular viewport in pixels
 * @return  Handle, or NULL on failure
 */
map_renderer_t* map_renderer_create(lv_obj_t* parent, const char* tile_base, int32_t viewport_size);

/**
 * Set the map view center and zoom, triggers a redraw.
 */
void map_renderer_set_view(map_renderer_t* mr, double lat, double lon, int32_t zoom);

/**
 * Pan the map by pixel delta.
 */
void map_renderer_pan(map_renderer_t* mr, int32_t dx, int32_t dy);

/**
 * Zoom in or out by one level.
 */
void map_renderer_zoom_in(map_renderer_t* mr);
void map_renderer_zoom_out(map_renderer_t* mr);

/**
 * Update the position marker on the map.
 * If tracking is enabled, the map auto-centers on the position.
 */
void map_renderer_set_position(map_renderer_t* mr, double lat, double lon, float cog_deg);

/**
 * Re-enable tracking (auto-center on vessel).
 */
void map_renderer_track(map_renderer_t* mr);

/**
 * Enable a vignette fade: radial edge fade + vertical bottom fade.
 * @param fade_start_pct  Where vertical fade begins (0.0=top, 1.0=bottom).
 * @param color           Color to fade toward (should match background).
 */
void map_renderer_set_vignette(map_renderer_t* mr, float fade_start_pct, lv_color_t color);

/**
 * Create the re-center button on a parent that sits ABOVE the chart area.
 * Must be called after map_renderer_create, passing the root/tile container.
 */
void map_renderer_create_track_btn(map_renderer_t* mr, lv_obj_t* btn_parent,
    int32_t x_ofs, int32_t y_ofs);

/**
 * Set an alternative tile path and create a toggle button.
 * Clicking the button switches between the primary and alt tile sets.
 */
void map_renderer_set_alt_tiles(map_renderer_t* mr, const char* alt_tile_base,
    lv_obj_t* btn_parent, int32_t x_ofs, int32_t y_ofs);

/**
 * Get current view state.
 */
double map_renderer_get_lat(map_renderer_t* mr);
double map_renderer_get_lon(map_renderer_t* mr);
int32_t map_renderer_get_zoom(map_renderer_t* mr);

#ifdef __cplusplus
}
#endif

#endif /* MAP_RENDERER_H */
