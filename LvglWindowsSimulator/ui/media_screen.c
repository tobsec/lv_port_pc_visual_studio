/*
 * Fusion media control tile.
 *
 * Layout, palette, and interaction discipline follow the design brief
 * from the LVGL research pass — verified inside the round 400-px clip
 * mask, montserrat 14/16/18/20/24/32/40/48 available. State comes from
 * fusion_state_snapshot(); commands go through fusion_cmd_*.
 *
 * Cheap update: media_screen_update reads state.rev, early-exits if
 * unchanged, and even when it does run it only touches labels whose
 * text actually changed (set_if_changed helper). Same discipline as
 * ais_screen.
 */
#include "media_screen.h"
#include "fusion_state.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
  #include "esp_timer.h"
#else
  /* Sim: LVGL tick is a portable ms clock — no <time.h>/clock_gettime
   * gymnastics needed. */
  static inline uint32_t now_ms(void) { return (uint32_t)lv_tick_get(); }
#endif

#define COL_BG          lv_color_hex(0x0d1117)
#define COL_TEXT        lv_color_hex(0xe6edf3)
#define COL_TEXT_DIM    lv_color_hex(0x8b949e)
#define COL_ACCENT      lv_color_hex(0x00d4ff)
#define COL_BTN         lv_color_hex(0x21262d)
#define COL_BTN_ACC     lv_color_hex(0x00b8d9)
#define COL_BAR_BG      lv_color_hex(0x21262d)
#define COL_BAR_FILL    lv_color_hex(0x00d4ff)
#define COL_MUTE_ON     lv_color_hex(0xd94a4a)

/* ── Widget handles ─────────────────────────────────────────────────── */

static lv_obj_t *s_content = NULL;         /* wrapper for online-state widgets */
static lv_obj_t *s_empty = NULL;           /* "NO RADIO" overlay */

static lv_obj_t *s_source_badge = NULL;
static lv_obj_t *s_mute_btn = NULL;
static lv_obj_t *s_mute_lbl = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_artist_lbl = NULL;
static lv_obj_t *s_elapsed_lbl = NULL;
static lv_obj_t *s_duration_lbl = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_prev_btn = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_play_lbl = NULL;
static lv_obj_t *s_next_btn = NULL;
static lv_obj_t *s_vol_down_btn = NULL;
static lv_obj_t *s_vol_up_btn = NULL;
static lv_obj_t *s_vol_bar = NULL;
static lv_obj_t *s_vol_readout = NULL;

/* Track last-rendered rev so update can early-out cheaply. */
static uint32_t s_last_rev = UINT32_MAX;

/* Volume repeat timer state — press-and-hold on ± sends at 8 Hz after
 * a 400 ms grace, matching the design's discipline. */
static lv_timer_t *s_vol_repeat_timer = NULL;
static int8_t     s_vol_repeat_delta = 0;

/* ── Small helpers ──────────────────────────────────────────────────── */

static void set_if_changed(lv_obj_t *lbl, const char *text)
{
    const char *cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(lbl, text);
}

static void fmt_mmss(char *buf, size_t cap, uint32_t s)
{
    uint32_t m = s / 60;
    uint32_t r = s % 60;
    /* Cap at 99:59 to keep the label narrow. */
    if (m > 99) { snprintf(buf, cap, "--:--"); return; }
    snprintf(buf, cap, "%02u:%02u", (unsigned)m, (unsigned)r);
}

static const char *source_glyph(media_source_t t)
{
    /* Simple text-badge glyphs. Real icons would need a bigger font
     * ROM — the labels are already unambiguous. */
    switch (t) {
        case MS_BT:     return "BLUETOOTH";
        case MS_FM:     return "FM";
        case MS_AM:     return "AM";
        case MS_USB:    return "USB";
        case MS_AUX:    return "AUX";
        case MS_DAB:    return "DAB";
        case MS_SIRIUS: return "SIRIUS";
        case MS_IPOD:   return "iPod";
        default:        return "-";
    }
}

/* ── Event handlers ─────────────────────────────────────────────────── */

static void play_click_cb(lv_event_t *e)   { (void)e; fusion_cmd_play_pause(); }
static void next_click_cb(lv_event_t *e)   { (void)e; fusion_cmd_next(); }
static void prev_click_cb(lv_event_t *e)   { (void)e; fusion_cmd_prev(); }

static void mute_click_cb(lv_event_t *e)
{
    (void)e;
    media_state_t st;
    fusion_state_snapshot(&st);
    fusion_cmd_mute(!st.muted);
}

/* Two-phase volume repeat: 400 ms grace after the initial tap, then
 * 8 Hz repeat until RELEASED. Same feel as a car radio's volume rocker.
 *
 *   PRESSED  → send one step immediately; start a 400 ms timer.
 *   400 ms   → send + retune the timer to 125 ms (8 Hz).
 *   125 ms × → send each tick.
 *   RELEASED → delete the timer.
 */
static bool s_vol_repeat_in_grace = false;

static void vol_repeat_timer_cb(lv_timer_t *t)
{
    if (s_vol_repeat_delta == 0) return;
    fusion_cmd_volume_delta(s_vol_repeat_delta);
    /* First fire completes the 400 ms grace — from now on repeat at
     * 8 Hz until RELEASED. */
    if (s_vol_repeat_in_grace) {
        s_vol_repeat_in_grace = false;
        lv_timer_set_period(t, 125);
    }
}

static void vol_button_event_cb(lv_event_t *e)
{
    int8_t sign = (int8_t)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_vol_repeat_delta = sign;
        fusion_cmd_volume_delta(sign);
        if (s_vol_repeat_timer) lv_timer_delete(s_vol_repeat_timer);
        s_vol_repeat_in_grace = true;
        s_vol_repeat_timer = lv_timer_create(vol_repeat_timer_cb, 400, NULL);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_vol_repeat_timer) {
            lv_timer_delete(s_vol_repeat_timer);
            s_vol_repeat_timer = NULL;
        }
        s_vol_repeat_delta = 0;
        s_vol_repeat_in_grace = false;
    }
}

/* ── Builder ────────────────────────────────────────────────────────── */

static lv_obj_t *make_button(lv_obj_t *parent, int32_t w, int32_t h,
                             int32_t x, int32_t y,
                             const char *glyph, const lv_font_t *font,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x - w / 2, y - h / 2);
    lv_obj_set_style_radius(btn, w / 2, 0);
    lv_obj_set_style_bg_color(btn, COL_BTN, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    if (glyph) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, glyph);
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(lbl, font ? font : &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);
    }
    return btn;
}

/* LV_EVENT_DELETE on the ± buttons kills the volume-repeat timer.
 * Without this, if the tile (or its parent tileview) is torn down while
 * the button is still pressed, LVGL is not guaranteed to deliver
 * RELEASED / PRESS_LOST to the object being freed. The 8 Hz timer would
 * then keep firing fusion_cmd_volume_delta forever — ramping volume to
 * 0 or MAX with no way to stop it short of a reboot. The next PRESSED
 * on a freshly-built tile would also lv_timer_delete the stale (freed)
 * timer pointer → UAF. */
static void vol_button_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_vol_repeat_timer) {
        lv_timer_delete(s_vol_repeat_timer);
        s_vol_repeat_timer = NULL;
    }
    s_vol_repeat_delta = 0;
    s_vol_repeat_in_grace = false;
}

void media_screen_create(lv_obj_t *tile)
{
    /* Reset rev-guard so a rebuild (e.g. tile teardown + fresh create
     * during an app-mode switch) actually re-populates all widgets on
     * the first update. Without this, s_last_rev keeps its old value
     * from the previous tile lifetime, media_screen_update early-outs
     * because fusion_state.rev hasn't changed since, and the freshly-
     * created widgets sit at their "-" / "00:00" defaults until the
     * next fusion event fires. */
    s_last_rev = UINT32_MAX;

    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* Content wrapper — all "online" widgets under here so we can
     * toggle the whole set in a single flag update when the head unit
     * goes offline. */
    s_content = lv_obj_create(tile);
    lv_obj_set_size(s_content, 800, 800);
    lv_obj_center(s_content);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Source badge (top-left of source strip, y=130) ─────────── */
    s_source_badge = lv_label_create(s_content);
    lv_label_set_text(s_source_badge, "-");
    lv_obj_set_style_text_color(s_source_badge, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_source_badge, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_source_badge, COL_BTN, 0);
    lv_obj_set_style_bg_opa(s_source_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_source_badge, 14, 0);
    lv_obj_set_style_pad_hor(s_source_badge, 14, 0);
    lv_obj_set_style_pad_ver(s_source_badge, 4, 0);
    lv_obj_set_style_text_align(s_source_badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_source_badge, 220, 28);
    /* Position: centre (330, 130) → top-left (220, 116). */
    lv_obj_set_pos(s_source_badge, 220, 116);

    /* ── Mute button (top-right, y=130) ─────────────────────────── */
    s_mute_btn = make_button(s_content, 56, 56, 560, 130,
                             LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_24,
                             mute_click_cb, NULL);
    s_mute_lbl = lv_obj_get_child(s_mute_btn, 0);

    /* ── Track title (M32, scroll-circular on long) ─────────────── */
    s_title_lbl = lv_label_create(s_content);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_title_lbl, "-");
    lv_obj_set_style_text_color(s_title_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_title_lbl, 500, 40);
    lv_obj_set_pos(s_title_lbl, 150, 155);   /* centre (400, 175) */

    /* ── Artist / album (M20 dim, dot-truncate) ─────────────────── */
    s_artist_lbl = lv_label_create(s_content);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_artist_lbl, "");
    lv_obj_set_style_text_color(s_artist_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_artist_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_artist_lbl, 500, 28);
    lv_obj_set_pos(s_artist_lbl, 150, 206);   /* centre (400, 220) */

    /* ── Elapsed / duration + progress bar (y=280..290) ─────────── */
    s_elapsed_lbl = lv_label_create(s_content);
    lv_label_set_text(s_elapsed_lbl, "00:00");
    lv_obj_set_style_text_color(s_elapsed_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_elapsed_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(s_elapsed_lbl, 140, 272);   /* centre (170, 280) */

    s_duration_lbl = lv_label_create(s_content);
    lv_label_set_text(s_duration_lbl, "--:--");
    lv_obj_set_style_text_color(s_duration_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_duration_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_duration_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_duration_lbl, 600, 272);  /* centre (630, 280) */

    s_progress_bar = lv_bar_create(s_content);
    lv_obj_set_size(s_progress_bar, 380, 14);
    lv_obj_set_pos(s_progress_bar, 210, 283);  /* centre (400, 290) */
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, COL_BAR_FILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 7, LV_PART_INDICATOR);

    /* ── Transport row (y=400) ──────────────────────────────────── */
    s_prev_btn = make_button(s_content, 96, 96, 260, 400,
                             LV_SYMBOL_PREV, &lv_font_montserrat_40,
                             prev_click_cb, NULL);
    s_play_btn = make_button(s_content, 128, 128, 400, 400,
                             LV_SYMBOL_PLAY, &lv_font_montserrat_48,
                             play_click_cb, NULL);
    s_play_lbl = lv_obj_get_child(s_play_btn, 0);
    s_next_btn = make_button(s_content, 96, 96, 540, 400,
                             LV_SYMBOL_NEXT, &lv_font_montserrat_40,
                             next_click_cb, NULL);
    /* Accent the play/pause button so it reads as the primary action. */
    lv_obj_set_style_bg_color(s_play_btn, COL_BTN_ACC, 0);

    /* ── Volume row (y=510, readout at y=545) ───────────────────── */
    s_vol_down_btn = make_button(s_content, 56, 56, 150, 510,
                                 LV_SYMBOL_MINUS, &lv_font_montserrat_24,
                                 NULL, (void *)(intptr_t)-1);
    /* Custom PRESSED/RELEASED handlers for the hold-repeat pattern.
     * DELETE guarantees the timer is killed if the button is destroyed
     * mid-press — see vol_button_delete_cb comment. */
    lv_obj_add_event_cb(s_vol_down_btn, vol_button_event_cb, LV_EVENT_PRESSED,     (void *)(intptr_t)-1);
    lv_obj_add_event_cb(s_vol_down_btn, vol_button_event_cb, LV_EVENT_RELEASED,   (void *)(intptr_t)-1);
    lv_obj_add_event_cb(s_vol_down_btn, vol_button_event_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(s_vol_down_btn, vol_button_delete_cb, LV_EVENT_DELETE,    NULL);

    s_vol_up_btn = make_button(s_content, 56, 56, 650, 510,
                               LV_SYMBOL_PLUS, &lv_font_montserrat_24,
                               NULL, (void *)(intptr_t)1);
    lv_obj_add_event_cb(s_vol_up_btn, vol_button_event_cb, LV_EVENT_PRESSED,     (void *)(intptr_t)1);
    lv_obj_add_event_cb(s_vol_up_btn, vol_button_event_cb, LV_EVENT_RELEASED,   (void *)(intptr_t)1);
    lv_obj_add_event_cb(s_vol_up_btn, vol_button_event_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)1);
    lv_obj_add_event_cb(s_vol_up_btn, vol_button_delete_cb, LV_EVENT_DELETE,    NULL);

    s_vol_bar = lv_bar_create(s_content);
    lv_obj_set_size(s_vol_bar, 320, 20);
    lv_obj_set_pos(s_vol_bar, 240, 500);       /* centre (400, 510) */
    lv_bar_set_range(s_vol_bar, 0, FUSION_VOL_MAX);
    lv_bar_set_value(s_vol_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_bar, COL_BAR_FILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_vol_bar, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_bar, 10, LV_PART_INDICATOR);

    s_vol_readout = lv_label_create(s_content);
    lv_label_set_text(s_vol_readout, "0");
    lv_obj_set_style_text_color(s_vol_readout, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_vol_readout, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_vol_readout, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_vol_readout, 60, 32);
    lv_obj_set_pos(s_vol_readout, 370, 529);   /* centre (400, 545) */

    /* ── Empty state overlay (sibling of s_content, hidden when online) ── */
    s_empty = lv_obj_create(tile);
    lv_obj_set_size(s_empty, 460, 120);
    lv_obj_center(s_empty);
    lv_obj_set_style_bg_opa(s_empty, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_empty, 0, 0);
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *empty_main = lv_label_create(s_empty);
    lv_label_set_text(empty_main, "NO RADIO DETECTED");
    lv_obj_set_style_text_color(empty_main, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(empty_main, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(empty_main, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty_main, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *empty_hint = lv_label_create(s_empty);
    lv_label_set_text(empty_hint, "Check Fusion NMEA-2000 wiring");
    lv_obj_set_style_text_color(empty_hint, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(empty_hint, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty_hint, LV_ALIGN_TOP_MID, 0, 60);

    /* Kick a status request so the head unit re-broadcasts everything.
     * Safe if there's no Fusion on the bus — the message goes nowhere. */
    fusion_cmd_request_status();
}

/* ── Update ─────────────────────────────────────────────────────────── */

void media_screen_update(void)
{
    if (!s_content) return;

    media_state_t st;
    fusion_state_snapshot(&st);
    if (st.rev == s_last_rev) return;
    s_last_rev = st.rev;

    /* Online / offline swap. */
    if (!st.online) {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    set_if_changed(s_source_badge, source_glyph(st.source_type));

    /* Title / artist / album — Fusion sends artist and album in
     * separate strings but visually we join them with an em-dash. */
    set_if_changed(s_title_lbl, st.title[0] ? st.title : "-");
    /* Note: use ASCII '-' as the artist/album separator. The em-dash
     * U+2014 is not in the compiled Montserrat range on this build
     * (Latin-1 only) — it would render as tofu. */
    char artist_line[FUSION_STR_LEN * 2 + 8];
    if (st.artist[0] && st.album[0]) {
        snprintf(artist_line, sizeof(artist_line), "%s  -  %s",
                 st.artist, st.album);
    } else if (st.artist[0]) {
        snprintf(artist_line, sizeof(artist_line), "%s", st.artist);
    } else if (st.album[0]) {
        snprintf(artist_line, sizeof(artist_line), "%s", st.album);
    } else {
        artist_line[0] = '\0';
    }
    set_if_changed(s_artist_lbl, artist_line);

    /* Time labels + progress bar. Duration 0 → streaming source, show
     * elapsed only and hide the duration side. */
    char elapsed_buf[8], duration_buf[8];
    fmt_mmss(elapsed_buf,  sizeof(elapsed_buf),  st.elapsed_s);
    if (st.duration_s > 0) {
        fmt_mmss(duration_buf, sizeof(duration_buf), st.duration_s);
        int pct = (int)((st.elapsed_s * 100) / st.duration_s);
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
        lv_obj_remove_flag(s_duration_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        strcpy(duration_buf, "--:--");
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_duration_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    set_if_changed(s_elapsed_lbl,  elapsed_buf);
    set_if_changed(s_duration_lbl, duration_buf);

    /* Play / pause icon. */
    const char *pp_glyph =
        (st.play_state == MP_PLAYING) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    set_if_changed(s_play_lbl, pp_glyph);

    /* Volume bar + readout + mute state. */
    lv_bar_set_value(s_vol_bar, st.volume, LV_ANIM_OFF);
    char vol_buf[8];
    snprintf(vol_buf, sizeof(vol_buf), "%u", (unsigned)st.volume);
    set_if_changed(s_vol_readout, vol_buf);

    if (st.muted) {
        lv_obj_set_style_bg_color(s_mute_btn, COL_MUTE_ON, 0);
        set_if_changed(s_mute_lbl, LV_SYMBOL_MUTE);
    } else {
        lv_obj_set_style_bg_color(s_mute_btn, COL_BTN, 0);
        set_if_changed(s_mute_lbl, LV_SYMBOL_VOLUME_MAX);
    }
}
