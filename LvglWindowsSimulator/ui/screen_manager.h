#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "lvgl/lvgl.h"
#include "gauge_data.h"

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
    void (*set_brightness)(uint8_t pct);   /* backlight %, 1-100 */
    void (*set_demo)(bool demo);           /* select demo vs live data */
} screen_hooks_t;
void screen_manager_set_hooks(const screen_hooks_t* hooks, uint8_t init_brightness, bool init_demo);

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

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_MANAGER_H */
