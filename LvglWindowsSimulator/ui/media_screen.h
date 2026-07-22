#ifndef MEDIA_SCREEN_H
#define MEDIA_SCREEN_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the Fusion media-control tile under `tile`. Called once from
 * screen_manager. Widgets are all positioned inside the 400-px round
 * clip mask (design agent verified). */
void media_screen_create(lv_obj_t* tile);

/* Refresh the tile from fusion_state — cheap when the state's rev
 * counter hasn't advanced since last call (early-out). */
void media_screen_update(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_SCREEN_H */
