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

/**
 * Create the screen manager with all screens inside a tileview.
 * Sets up the circular mask, tileview, and all three screens.
 */
void screen_manager_create(void);

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
