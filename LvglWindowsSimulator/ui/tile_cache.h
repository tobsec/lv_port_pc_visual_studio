#ifndef TILE_CACHE_H
#define TILE_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tile dimensions — must match map_renderer */
#define TC_TILE_PX     256
#define TC_TILE_BYTES  (TC_TILE_PX * TC_TILE_PX * 2)  /* 131,072 bytes RGB565 */

/* Cache key: uniquely identifies a tile across all tile sets and zoom levels */
typedef struct {
    uint8_t  path_id;   /* registered tile-base index (0=primary, 1=alt, ...) */
    uint8_t  zoom;      /* slippy map zoom level */
    uint16_t x, y;      /* slippy map tile coordinates */
} tile_key_t;

/* Opaque handle */
typedef struct tile_cache tile_cache_t;

/**
 * Create a tile cache with pre-allocated PSRAM buffers.
 * @param max_tiles  Number of tile slots (each 128 KB).  64 = 8 MB.
 * @return Handle, or NULL on allocation failure.
 */
tile_cache_t* tile_cache_create(int32_t max_tiles);

/** Destroy the cache and free all buffers. */
void tile_cache_destroy(tile_cache_t* tc);

/**
 * Register a tile-base path (e.g. "S:/MAP_BIN") and get a path_id for cache keys.
 * Up to 4 paths may be registered.
 * @return path_id (0, 1, ...) or 0xFF on overflow.
 */
uint8_t tile_cache_register_path(tile_cache_t* tc, const char* tile_base);

/**
 * Look up a tile in the cache.
 * Thread-safe (acquires mutex on ESP32).
 * @return Pointer to tile data (131,072 bytes), or NULL on cache miss.
 *         The pointer is valid until the next cache eviction — use immediately.
 */
const uint8_t* tile_cache_get(tile_cache_t* tc, tile_key_t key);

/**
 * Request a single tile to be loaded into the cache.
 * On ESP32: enqueues to the background loader task.
 * On simulator: loads synchronously into cache immediately.
 * No-op if the tile is already cached.
 */
void tile_cache_request(tile_cache_t* tc, tile_key_t key, const char* tile_base);

/**
 * Prefetch tiles around a center position: visible area + 1-tile border
 * + adjacent zoom levels.  Skips tiles already in cache.
 */
void tile_cache_prefetch(tile_cache_t* tc, uint8_t path_id,
    int32_t center_tx, int32_t center_ty, int32_t zoom,
    const char* tile_base);

/**
 * Load a tile synchronously and insert into cache.
 * Returns cached data pointer, or NULL if the file doesn't exist.
 * Used for visible tiles that must be displayed immediately.
 */
const uint8_t* tile_cache_load_sync(tile_cache_t* tc, tile_key_t key, const char* tile_base);

/**
 * Check if background loader has finished any new tiles since the last check.
 * Returns true and clears the flag if new tiles are available.
 * Always returns false on the simulator (loads are synchronous).
 */
bool tile_cache_check_ready(tile_cache_t* tc);

/** Start the background loader task (ESP32 only, no-op on simulator). */
void tile_cache_start_loader(tile_cache_t* tc);

/** Stop the background loader task (ESP32 only, no-op on simulator). */
void tile_cache_stop_loader(tile_cache_t* tc);

#ifdef __cplusplus
}
#endif

#endif /* TILE_CACHE_H */
