#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "lvgl/lvgl.h"
#include "gauge_data.h"
#include "warn_thresholds.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the tile base path for the map renderer (call before screen_manager_create).
 */
void screen_manager_set_tile_path(const char* path);
void screen_manager_set_alt_tile_path(const char* path);

/**
 * Create the screen manager with all screens inside a tileview.
 * Sets up the circular mask, tileview, and all three screens.
 */
void screen_manager_create(void);

/**
 * Show or hide the "DEMO" badge that marks simulated (non-live) data.
 */
void screen_manager_set_demo(bool demo);

/**
 * Platform hooks the settings screen calls to apply + persist changes.
 * The shared UI stays hardware-agnostic; the firmware backs these with the
 * backlight / data source / NVS, the simulator with stubs.
 * Call before screen_manager_create() so the controls start at init values.
 */
typedef struct {
    void (*set_brightness)(uint8_t pct);          /* backlight %, 1-100 */
    void (*set_demo)(bool demo);                  /* select demo vs live data */
    void (*set_threshold)(int id, uint16_t val);  /* persist a warning threshold (thr_id_t) */
    void (*set_ais_range_nm)(uint8_t nm);         /* AIS request radius, NM (5..40) */
    void (*set_ap_enabled)(bool enabled);         /* SoftAP on/off (persisted + applied) */
} screen_hooks_t;
void screen_manager_set_hooks(const screen_hooks_t* hooks, uint8_t init_brightness, bool init_demo);

/* Initial SoftAP switch state for the settings editor. Call before
 * screen_manager_create(). Independent from set_hooks so main can pull
 * the current value from NVS/ota_wifi without duplicating that plumbing. */
void screen_manager_set_ap_enabled(bool enabled);

/* Provide the initial AIS request range (NM) for the settings editor. Call
 * before screen_manager_create(). */
void screen_manager_set_ais_range(uint8_t nm);

/* Provide the initial warning thresholds for the settings editor (applies to
 * the warning overlay too). Call before screen_manager_create(). */
void screen_manager_set_thresholds(const warn_thresholds_t* t);

/**
 * Update all screens with current gauge data.
 */
void screen_manager_update(const gauge_data_t* data);

/**
 * Navigate to the next/previous screen.
 */
void screen_manager_next(void);
void screen_manager_prev(void);

/* Get the map renderer for the currently active screen (or NULL) */
struct map_renderer* screen_manager_get_active_map(void);

/* Get the map renderer for the full-screen chart tile, regardless of
 * what's currently visible. Used by the AIS list tile when the user
 * taps a row so we can center the chart on that target before switching
 * to it. NULL if there's no SD/tile-path and the chart tile was built
 * without a map. */
struct map_renderer* screen_manager_get_chart_map(void);

/* Programmatically switch to the full-screen chart tile. Wraps the
 * tileview transition so callers don't have to know its internal index. */
void screen_manager_show_chart(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_MANAGER_H */
