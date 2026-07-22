#ifndef AIS_SCREEN_H
#define AIS_SCREEN_H

#include "lvgl/lvgl.h"
#include "gauge_data.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the AIS-targets tile inside `tile`. Called once from screen_manager
 * at startup. The tile shows a scrollable list of currently-tracked vessels
 * and AtoNs, each row: name/MMSI, source (BLE/N2K), age since last update,
 * and range from own position.
 *
 * Range needs own position — pass it in via ais_screen_update() on every
 * frame the tile is visible. */
void ais_screen_create(lv_obj_t* tile);

/* Refresh the list from ais_store. `now_ms` is the local xTaskGetTickCount
 * value (or equivalent) used to compute "age" — same base as
 * ais_vessel_t::pos_update_ms. Own lat/lon/SOG/COG come from `d` so the
 * per-row range column and the tap-detail modal's BRG/CPA/TCPA can be
 * computed without a second setter call. Cheap to call every frame;
 * internally self-throttled and only re-labels when values actually
 * change. Pass a d with valid.position==false when own position is
 * unknown and the range column will show "--". */
void ais_screen_update(const gauge_data_t* d, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* AIS_SCREEN_H */
