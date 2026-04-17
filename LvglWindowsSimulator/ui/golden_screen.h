#ifndef GOLDEN_SCREEN_H
#define GOLDEN_SCREEN_H

#include "lvgl/lvgl.h"
#include "gauge_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set the tile base path before creating the screen.
 * Simulator: "C:/Data/marine-gauge/tools/MAP_BIN"
 * ESP32:     "S:/MAP_BIN" (SD card)
 * Tiles at: {base}/{z}/{x}/{y}.bin (raw RGB565, 256x256) */
void golden_screen_set_tile_path(const char* path);
void golden_screen_set_alt_tile_path(const char* path);

/* Create the golden main screen.
 * @param parent  Container to build in (tileview tile or screen root).
 *                If NULL, uses lv_screen_active() with its own circular mask. */
void golden_screen_create(lv_obj_t* parent);

/* Update all widgets from current gauge data */
void golden_screen_update(const gauge_data_t* data);

/* Get the map renderer instance (for zoom control from simulator) */
struct map_renderer* golden_screen_get_map(void);

/* Debug: show a frame tick counter to distinguish data stall from render lag */
void golden_screen_show_tick_counter(lv_obj_t* parent);
void golden_screen_update_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* GOLDEN_SCREEN_H */
