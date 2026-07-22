#ifndef MAP_RENDERER_H
#define MAP_RENDERER_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for a map renderer instance */
typedef struct map_renderer map_renderer_t;
typedef struct tile_cache tile_cache_t;

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
 * Update own-ship SOG (knots), used for CPA/TCPA computation against
 * AIS targets on the tap-to-inspect card. Cheap; call alongside
 * map_renderer_set_position from the data tick.
 */
void map_renderer_set_own_sog(map_renderer_t* mr, float sog_knots);

/**
 * Copy own-vessel position + heading + SOG state (pos_valid / lat / lon
 * / COG / SOG_knots) from `src` to `dst`. Useful on screen swipe so the
 * newly-visible map immediately shows the yellow COG marker without
 * waiting for the next data tick — otherwise the destination map's
 * pos_valid stays false until set_position runs under its own guard.
 */
void map_renderer_mirror_pos(map_renderer_t* dst, map_renderer_t* src);

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
 * Create a pair of zoom-in / zoom-out buttons at the given offsets on
 * `btn_parent` (typically the tile container). Each button's position
 * is independent so the pair can be stacked vertically OR laid out
 * side-by-side, e.g. (-30, 200) / (30, 200) to sit between the
 * theme and track buttons across the bottom of the map.
 */
void map_renderer_create_zoom_btns(map_renderer_t* mr, lv_obj_t* btn_parent,
    int32_t x_in,  int32_t y_in,
    int32_t x_out, int32_t y_out);

/**
 * Attach a shared tile cache.  Enables async tile loading and LRU caching.
 * Call after create, before the first render.
 */
void map_renderer_set_cache(map_renderer_t* mr, tile_cache_t* cache);

/**
 * Force a re-render (e.g. after background tile loads complete).
 */
void map_renderer_render(map_renderer_t* mr);

/**
 * Pull the latest AIS targets from the global ais_store and redraw them on
 * the chart overlay. Cheap — caller can invoke at the same cadence as
 * map_renderer_set_position. Hidden slots reuse pool widgets.
 */
void map_renderer_refresh_ais(map_renderer_t* mr);

/**
 * Get current view state.
 */
double map_renderer_get_lat(map_renderer_t* mr);
double map_renderer_get_lon(map_renderer_t* mr);
int32_t map_renderer_get_zoom(map_renderer_t* mr);

/**
 * True when the map auto-centers on own position (default after
 * map_renderer_track). Set to false as soon as the user pans; re-set by
 * map_renderer_track. The viewport broadcaster uses this to decide
 * whether to publish an override or fall back to own-pos + range.
 */
bool map_renderer_is_tracking(map_renderer_t* mr);

/**
 * True while the user's finger is dragging the chart (PRESSING with
 * cumulative non-zero movement, cleared on RELEASED / PRESS_LOST).
 * screen_manager uses this to skip refresh_ais during a drag —
 * repainting all icons on every 2-s tick while the user is scrubbing
 * the map is expensive and unnecessary; reposition_ais_only inside
 * apply_pan_offset already keeps the icons glued to the chart.
 */
bool map_renderer_is_panning(map_renderer_t* mr);

/**
 * Current visible viewport as a lat/lon bounding box with 20% margin.
 * lat_max > lat_min, lon_max > lon_min (crossing anti-meridian is not
 * handled — irrelevant for Adriatic use).
 */
typedef struct {
    double  lat_min, lat_max;
    double  lon_min, lon_max;
    int32_t zoom;
} map_viewport_bbox_t;

void map_renderer_get_viewport_bbox(map_renderer_t* mr, map_viewport_bbox_t* out);

/**
 * Center on an AIS target by MMSI and pop the target-detail card.
 * Turns tracking off (the user is now looking at that target, not own
 * ship). If `mmsi` is not in the store (either as a vessel or an AtoN)
 * this is a no-op. Called from the AIS list tile when the user taps a
 * row so the chart jumps to the vessel with its info card open. Returns
 * true when the target was found and the view was updated.
 */
bool map_renderer_focus_mmsi(map_renderer_t* mr, uint32_t mmsi);

/**
 * Get the AIS status badge widget so a parent screen can reposition it
 * when the default (aligned to chart_area TOP_MID + 18) collides with
 * other UI. Returns NULL if the badge hasn't been built yet.
 */
struct _lv_obj_t* map_renderer_get_ais_badge(map_renderer_t* mr);

#ifdef __cplusplus
}
#endif

#endif /* MAP_RENDERER_H */
