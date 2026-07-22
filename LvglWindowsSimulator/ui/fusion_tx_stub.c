/*
 * Windows LVGL simulator stub for the Fusion TX side.
 *
 * The real fusion_tx.cpp on the ESP target hands the wire bytes to the
 * NMEA2000 driver. In the sim there's no CAN bus and the point of the
 * media tile is to see the layout and interactions — the button
 * callbacks flow through fusion_cmd_* into these stubs, which just
 * print. Volume/mute/power state gets locally reflected via the demo
 * publisher so the UI still updates after a tap.
 */
#include "fusion_state.h"
#include "fusion_state_internal.h"

#include "lvgl.h"     /* lv_tick_get for the user-touch grace stamp */

#include <stdio.h>
#include <stdbool.h>

/* Stamp the field the user just poked so fusion_state_publish_demo
 * (running 20 Hz off the sim timer) doesn't stomp it in the next frame.
 * See fusion_state.c's touch-grace block. */
static inline uint32_t now_ms(void) { return (uint32_t)lv_tick_get(); }

void fusion_tx_transport(uint8_t source_id, media_source_t stype, uint8_t cmd)
{
    (void)source_id; (void)stype;
    printf("[fusion_tx_stub] transport cmd=%u\n", (unsigned)cmd);
    /* Reflect play/pause locally so the tile animates on tap. */
    if (cmd == 1)      fusion_state_set_play(MP_PLAYING);
    else if (cmd == 2) fusion_state_set_play(MP_PAUSED);
    fusion_state_note_user_touch(FUSION_TOUCH_PLAY, now_ms());
}

void fusion_tx_all_zone_volume(uint8_t vol)
{
    printf("[fusion_tx_stub] volume=%u\n", (unsigned)vol);
    fusion_state_set_volume(vol);
    fusion_state_note_user_touch(FUSION_TOUCH_VOLUME, now_ms());
}

void fusion_tx_mute(bool mute)
{
    printf("[fusion_tx_stub] mute=%d\n", (int)mute);
    fusion_state_set_mute(mute);
    fusion_state_note_user_touch(FUSION_TOUCH_MUTE, now_ms());
}

void fusion_tx_power(bool on)
{
    printf("[fusion_tx_stub] power=%d\n", (int)on);
    fusion_state_set_power(on);
    fusion_state_note_user_touch(FUSION_TOUCH_POWER, now_ms());
}

void fusion_tx_set_source(uint8_t source_id)
{
    printf("[fusion_tx_stub] set_source=%u\n", (unsigned)source_id);
    /* No local reflection — we don't know the type/name mapping here;
     * the note still holds off the demo cycle so if a source picker is
     * wired up later it won't be immediately overwritten. */
    fusion_state_note_user_touch(FUSION_TOUCH_SOURCE, now_ms());
}

void fusion_tx_request_status(void)
{
    printf("[fusion_tx_stub] request_status\n");
}
