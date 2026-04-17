#include "tile_cache.h"
#include "lvgl/lvgl.h"
#include <string.h>
#include <stdio.h>

/* ── Platform abstraction ── */

#ifdef ESP_PLATFORM
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  #include "freertos/task.h"
  #include "freertos/queue.h"
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  #define TC_ALLOC(sz)    heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
  #define TC_FREE(p)      heap_caps_free(p)
  #define TC_LOCK(tc)     xSemaphoreTake((tc)->mutex, portMAX_DELAY)
  #define TC_UNLOCK(tc)   xSemaphoreGive((tc)->mutex)
  static const char* TAG = "tile_cache";
#else
  #include <stdlib.h>
  #define TC_ALLOC(sz)    malloc(sz)
  #define TC_FREE(p)      free(p)
  #define TC_LOCK(tc)     ((void)0)
  #define TC_UNLOCK(tc)   ((void)0)
#endif

#define MAX_PATHS       4
#define PATH_BUF_LEN    300
#define LOAD_QUEUE_LEN  32

/* ── Internal types ── */

typedef struct {
    tile_key_t key;
    uint8_t*   data;        /* TC_TILE_BYTES in PSRAM, pre-allocated */
    uint32_t   lru_stamp;
    bool       occupied;
} tile_slot_t;

/* Load request for the background task */
typedef struct {
    tile_key_t key;
    char       path[PATH_BUF_LEN + 48];
} tile_load_req_t;

struct tile_cache {
    tile_slot_t* slots;
    int32_t      max_tiles;
    uint32_t     lru_counter;

    char         paths[MAX_PATHS][PATH_BUF_LEN];
    int          path_count;

#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex;
    QueueHandle_t     load_queue;
    TaskHandle_t      loader_task;
    uint8_t*          loader_buf;   /* 128 KB read buffer for the loader */
#endif
    volatile bool     tiles_ready;
};

/* ── Helpers ── */

static bool key_eq(tile_key_t a, tile_key_t b)
{
    return a.path_id == b.path_id && a.zoom == b.zoom && a.x == b.x && a.y == b.y;
}

/* Find a slot for the given key.  Returns the slot index, or -1 on miss. */
static int32_t find_slot(tile_cache_t* tc, tile_key_t key)
{
    for (int32_t i = 0; i < tc->max_tiles; i++) {
        if (tc->slots[i].occupied && key_eq(tc->slots[i].key, key))
            return i;
    }
    return -1;
}

/* Find the LRU slot (smallest lru_stamp) or the first empty slot. */
static int32_t evict_slot(tile_cache_t* tc)
{
    int32_t best = 0;
    uint32_t best_stamp = UINT32_MAX;
    for (int32_t i = 0; i < tc->max_tiles; i++) {
        if (!tc->slots[i].occupied) return i;
        if (tc->slots[i].lru_stamp < best_stamp) {
            best_stamp = tc->slots[i].lru_stamp;
            best = i;
        }
    }
    return best;
}

/* Build the file path for a tile. */
static void build_path(char* buf, size_t buf_len, const char* tile_base, tile_key_t key)
{
    snprintf(buf, buf_len, "%s/%d/%u/%u.bin", tile_base, key.zoom, key.x, key.y);
}

/* Load a tile from disk into a buffer.  Returns true on success. */
static bool load_from_disk(const char* path, uint8_t* dst)
{
    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return false;

    uint32_t bytes_read = 0;
    lv_fs_res_t res = lv_fs_read(&f, dst, TC_TILE_BYTES, &bytes_read);
    lv_fs_close(&f);
    return (res == LV_FS_RES_OK && bytes_read == TC_TILE_BYTES);
}

/* Insert tile data into a cache slot (caller must hold lock on ESP32). */
static void insert_into_slot(tile_cache_t* tc, tile_key_t key, const uint8_t* src)
{
    int32_t idx = evict_slot(tc);
    tile_slot_t* s = &tc->slots[idx];
    s->key = key;
    memcpy(s->data, src, TC_TILE_BYTES);
    s->lru_stamp = ++tc->lru_counter;
    s->occupied = true;
}

/* ── Public API ── */

tile_cache_t* tile_cache_create(int32_t max_tiles)
{
    tile_cache_t* tc = (tile_cache_t*)calloc(1, sizeof(*tc));
    if (!tc) return NULL;

    tc->max_tiles = max_tiles;
    tc->slots = (tile_slot_t*)calloc(max_tiles, sizeof(tile_slot_t));
    if (!tc->slots) { free(tc); return NULL; }

    /* Pre-allocate tile data buffers in PSRAM */
    for (int32_t i = 0; i < max_tiles; i++) {
        tc->slots[i].data = (uint8_t*)TC_ALLOC(TC_TILE_BYTES);
        if (!tc->slots[i].data) {
            /* Partial allocation — shrink max_tiles to what we got */
            tc->max_tiles = i;
            break;
        }
    }

#ifdef ESP_PLATFORM
    tc->mutex = xSemaphoreCreateMutex();
    tc->load_queue = xQueueCreate(LOAD_QUEUE_LEN, sizeof(tile_load_req_t));
    tc->loader_buf = (uint8_t*)TC_ALLOC(TC_TILE_BYTES);
    ESP_LOGI(TAG, "Created tile cache: %ld slots (%.1f MB PSRAM)",
        (long)tc->max_tiles, tc->max_tiles * TC_TILE_BYTES / (1024.0f * 1024.0f));
#endif

    return tc;
}

void tile_cache_destroy(tile_cache_t* tc)
{
    if (!tc) return;
    tile_cache_stop_loader(tc);
    for (int32_t i = 0; i < tc->max_tiles; i++) {
        if (tc->slots[i].data) TC_FREE(tc->slots[i].data);
    }
#ifdef ESP_PLATFORM
    if (tc->loader_buf) TC_FREE(tc->loader_buf);
    if (tc->mutex) vSemaphoreDelete(tc->mutex);
    if (tc->load_queue) vQueueDelete(tc->load_queue);
#endif
    free(tc->slots);
    free(tc);
}

uint8_t tile_cache_register_path(tile_cache_t* tc, const char* tile_base)
{
    if (!tc || !tile_base) return 0xFF;

    /* Check if already registered */
    for (int i = 0; i < tc->path_count; i++) {
        if (strcmp(tc->paths[i], tile_base) == 0)
            return (uint8_t)i;
    }
    if (tc->path_count >= MAX_PATHS) return 0xFF;

    int id = tc->path_count++;
    strncpy(tc->paths[id], tile_base, PATH_BUF_LEN - 1);
    tc->paths[id][PATH_BUF_LEN - 1] = '\0';
    return (uint8_t)id;
}

const uint8_t* tile_cache_get(tile_cache_t* tc, tile_key_t key)
{
    if (!tc) return NULL;
    TC_LOCK(tc);
    int32_t idx = find_slot(tc, key);
    if (idx >= 0) {
        tc->slots[idx].lru_stamp = ++tc->lru_counter;
        TC_UNLOCK(tc);
        return tc->slots[idx].data;
    }
    TC_UNLOCK(tc);
    return NULL;
}

void tile_cache_request(tile_cache_t* tc, tile_key_t key, const char* tile_base)
{
    if (!tc || !tile_base) return;

    /* Already cached? */
    TC_LOCK(tc);
    int32_t idx = find_slot(tc, key);
    TC_UNLOCK(tc);
    if (idx >= 0) return;

#ifdef ESP_PLATFORM
    /* Enqueue for background loader */
    tile_load_req_t req;
    req.key = key;
    build_path(req.path, sizeof(req.path), tile_base, key);
    xQueueSend(tc->load_queue, &req, 0);  /* non-blocking, drop if full */
#else
    /* Simulator: load synchronously into cache */
    char path[PATH_BUF_LEN + 48];
    build_path(path, sizeof(path), tile_base, key);
    /* Use a temporary stack buffer for reading */
    uint8_t* tmp = (uint8_t*)malloc(TC_TILE_BYTES);
    if (!tmp) return;
    if (load_from_disk(path, tmp)) {
        TC_LOCK(tc);
        insert_into_slot(tc, key, tmp);
        TC_UNLOCK(tc);
    }
    free(tmp);
#endif
}

void tile_cache_prefetch(tile_cache_t* tc, uint8_t path_id,
    int32_t center_tx, int32_t center_ty, int32_t zoom,
    const char* tile_base)
{
    if (!tc || !tile_base || path_id == 0xFF) return;

    /* 1. Visible + adjacent tiles at current zoom (5×5 grid around center) */
    for (int32_t dy = -2; dy <= 2; dy++) {
        for (int32_t dx = -2; dx <= 2; dx++) {
            tile_key_t key = { path_id, (uint8_t)zoom,
                               (uint16_t)(center_tx + dx),
                               (uint16_t)(center_ty + dy) };
            tile_cache_request(tc, key, tile_base);
        }
    }

    /* 2. Zoom-1 tiles (parent level, 3×3 grid) */
    if (zoom > 10) {
        int32_t pz = zoom - 1;
        int32_t ptx = center_tx >> 1;
        int32_t pty = center_ty >> 1;
        for (int32_t dy = -1; dy <= 1; dy++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                tile_key_t key = { path_id, (uint8_t)pz,
                                   (uint16_t)(ptx + dx),
                                   (uint16_t)(pty + dy) };
                tile_cache_request(tc, key, tile_base);
            }
        }
    }

    /* 3. Zoom+1 tiles (child level, 4×4 grid) */
    if (zoom < 15) {
        int32_t cz = zoom + 1;
        int32_t ctx = center_tx << 1;
        int32_t cty_c = center_ty << 1;
        for (int32_t dy = -1; dy <= 2; dy++) {
            for (int32_t dx = -1; dx <= 2; dx++) {
                tile_key_t key = { path_id, (uint8_t)cz,
                                   (uint16_t)(ctx + dx),
                                   (uint16_t)(cty_c + dy) };
                tile_cache_request(tc, key, tile_base);
            }
        }
    }
}

const uint8_t* tile_cache_load_sync(tile_cache_t* tc, tile_key_t key, const char* tile_base)
{
    if (!tc || !tile_base) return NULL;

    /* Already cached? */
    TC_LOCK(tc);
    int32_t idx = find_slot(tc, key);
    if (idx >= 0) {
        tc->slots[idx].lru_stamp = ++tc->lru_counter;
        TC_UNLOCK(tc);
        return tc->slots[idx].data;
    }

    /* Sync load directly into an evicted cache slot */
    int32_t slot_idx = evict_slot(tc);
    tile_slot_t* s = &tc->slots[slot_idx];
    TC_UNLOCK(tc);

    char path[PATH_BUF_LEN + 48];
    build_path(path, sizeof(path), tile_base, key);
    if (!load_from_disk(path, s->data))
        return NULL;

    TC_LOCK(tc);
    s->key = key;
    s->lru_stamp = ++tc->lru_counter;
    s->occupied = true;
    TC_UNLOCK(tc);
    return s->data;
}

bool tile_cache_check_ready(tile_cache_t* tc)
{
    if (!tc || !tc->tiles_ready) return false;
    tc->tiles_ready = false;
    return true;
}

/* ── Background loader (ESP32 only) ── */

#ifdef ESP_PLATFORM

static void loader_task_fn(void* arg)
{
    tile_cache_t* tc = (tile_cache_t*)arg;
    tile_load_req_t req;

    while (xQueueReceive(tc->load_queue, &req, portMAX_DELAY) == pdTRUE) {
        /* Skip if already cached */
        TC_LOCK(tc);
        int32_t idx = find_slot(tc, req.key);
        TC_UNLOCK(tc);
        if (idx >= 0) continue;

        /* Read tile from SD card into loader buffer */
        if (!load_from_disk(req.path, tc->loader_buf))
            continue;

        /* Insert into cache */
        TC_LOCK(tc);
        insert_into_slot(tc, req.key, tc->loader_buf);
        TC_UNLOCK(tc);

        tc->tiles_ready = true;
    }
}

void tile_cache_start_loader(tile_cache_t* tc)
{
    if (!tc || tc->loader_task) return;
    xTaskCreatePinnedToCore(loader_task_fn, "tile_loader", 8192, tc, 5,
        &tc->loader_task, 0);
    ESP_LOGI(TAG, "Background tile loader started on Core 0");
}

void tile_cache_stop_loader(tile_cache_t* tc)
{
    if (!tc || !tc->loader_task) return;
    vTaskDelete(tc->loader_task);
    tc->loader_task = NULL;
}

#else /* Simulator stubs */

void tile_cache_start_loader(tile_cache_t* tc) { (void)tc; }
void tile_cache_stop_loader(tile_cache_t* tc) { (void)tc; }

#endif /* ESP_PLATFORM */
