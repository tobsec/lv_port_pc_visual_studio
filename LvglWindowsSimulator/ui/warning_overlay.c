#include "warning_overlay.h"
#include "icons/lv_image_telltale_icons.h"
#include <stdio.h>
#include <string.h>

/* ── Warning IDs ── */
enum {
    WID_ENGINE_CAN_TIMEOUT = 0,
    WID_EMERGENCY_STOP,
    WID_OVERTEMP_OIL,
    WID_OVERTEMP_CLT,
    WID_LOW_OIL_PRESS,
    WID_LOW_VOLTAGE,
    WID_LOW_FUEL_PRESS,
    WID_LOW_WATER_FLOW,
    WID_LAMBDA_OOR,
    WID_RPM_REDLINE,
    WID_NAV_CAN_TIMEOUT,
    WID_DEPTH_LOW,
    WID_COUNT
};

/* CAN dependency */
typedef enum {
    DEP_NONE = 0,
    DEP_ENGINE_CAN,
    DEP_NAV_CAN
} can_dep_t;

typedef struct {
    warn_severity_t severity;
    uint8_t priority;        /* lower = higher priority */
    const char* title;
    char detail[64];
    const lv_image_dsc_t* icon;   /* DIN/SAE telltale (A8, tinted by overlay) */
    bool raw_active;
    bool active;
    bool was_active;         /* for critical: remembers it was triggered, even after clear */
    bool acknowledged;
    uint32_t activate_time;
    uint32_t debounce_start;
    uint32_t debounce_ms;
    float trigger_thresh;
    float clear_thresh;
    can_dep_t depends_on;
} warning_state_t;

static warning_state_t warnings[WID_COUNT];
static warn_thresholds_t s_thr;   /* user-tunable thresholds (set via API) */

/* ── Overlay widgets ── */
static lv_obj_t* overlay_scrim;
static lv_obj_t* overlay_panel;
static lv_obj_t* overlay_icon;
static lv_obj_t* overlay_title;
static lv_obj_t* overlay_detail;
static lv_obj_t* overlay_ack_btn;
static bool pulse_running = false;
static int32_t shown_warning_id = -1;

/* Colors */
#define COL_WARN_RED_DARK   lv_color_hex(0x2a0a0a)
#define COL_WARN_RED_BRIGHT lv_color_hex(0xff0000)
#define COL_WARN_YELLOW     lv_color_hex(0xffd600)
#define COL_WARN_AMBER_BG   lv_color_hex(0x2a2000)
#define COL_WARN_BLUE       lv_color_hex(0x4488ff)
#define COL_WARN_BLUE_BG    lv_color_hex(0x0a1a2a)
#define COL_TEXT_WHITE       lv_color_hex(0xe6edf3)

/* ── Init warning definitions ── */
static void init_warnings(void)
{
    memset(warnings, 0, sizeof(warnings));
    warn_thresholds_defaults(&s_thr);

    /* Engine CAN timeout is a normal condition when the ignition is off (ECU
     * unpowered), so it's a warning, not a red critical. */
    warnings[WID_ENGINE_CAN_TIMEOUT] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 0, .title = "ENGINE CAN TIMEOUT",
        .icon = &tt_icon_can_network,
        .debounce_ms = 2000, .depends_on = DEP_NONE };

    /* Critical */
    warnings[WID_EMERGENCY_STOP] = (warning_state_t){
        .severity = WARN_CRITICAL, .priority = 1, .title = "EMERGENCY STOP",
        .icon = &tt_icon_emergency_stop,
        .debounce_ms = 0, .depends_on = DEP_ENGINE_CAN };

    warnings[WID_OVERTEMP_OIL] = (warning_state_t){
        .severity = WARN_CRITICAL, .priority = 2, .title = "OIL OVERTEMP",
        .icon = &tt_icon_oil_temp,
        .debounce_ms = 1000, .trigger_thresh = 125, .clear_thresh = 120,
        .depends_on = DEP_ENGINE_CAN };

    warnings[WID_OVERTEMP_CLT] = (warning_state_t){
        .severity = WARN_CRITICAL, .priority = 3, .title = "COOLANT OVERTEMP",
        .icon = &tt_icon_coolant_temp,
        .debounce_ms = 1000, .trigger_thresh = 85, .clear_thresh = 80,
        .depends_on = DEP_ENGINE_CAN };

    warnings[WID_LOW_OIL_PRESS] = (warning_state_t){
        .severity = WARN_CRITICAL, .priority = 4, .title = "LOW OIL PRESSURE",
        .icon = &tt_icon_oil_press,
        .debounce_ms = 1000, .trigger_thresh = 150, .clear_thresh = 165,
        .depends_on = DEP_ENGINE_CAN };

    /* Warning */
    warnings[WID_LOW_VOLTAGE] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 5, .title = "LOW VOLTAGE",
        .icon = &tt_icon_battery,
        .debounce_ms = 2000, .trigger_thresh = 12.0f, .clear_thresh = 12.5f,
        .depends_on = DEP_ENGINE_CAN };

    warnings[WID_LOW_FUEL_PRESS] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 6, .title = "LOW FUEL PRESSURE",
        .icon = &tt_icon_fuel,
        .debounce_ms = 1000, .trigger_thresh = 275, .clear_thresh = 290,
        .depends_on = DEP_ENGINE_CAN };

    warnings[WID_LOW_WATER_FLOW] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 7, .title = "LOW WATER FLOW",
        .icon = &tt_icon_water_flow,
        .debounce_ms = 1000, .trigger_thresh = 15, .clear_thresh = 20,
        .depends_on = DEP_ENGINE_CAN };

    warnings[WID_LAMBDA_OOR] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 8, .title = "LAMBDA OUT OF RANGE",
        .icon = &tt_icon_lambda,
        .debounce_ms = 3000, .depends_on = DEP_ENGINE_CAN };

    warnings[WID_RPM_REDLINE] = (warning_state_t){
        .severity = WARN_WARNING, .priority = 9, .title = "RPM REDLINE",
        .icon = &tt_icon_rpm_redline,
        .debounce_ms = 500, .trigger_thresh = 5200, .clear_thresh = 5000,
        .depends_on = DEP_ENGINE_CAN };

    /* Info */
    warnings[WID_NAV_CAN_TIMEOUT] = (warning_state_t){
        .severity = WARN_INFO, .priority = 10, .title = "NAV CAN TIMEOUT",
        .icon = &tt_icon_can_network,
        .debounce_ms = 2000, .depends_on = DEP_NONE };

    warnings[WID_DEPTH_LOW] = (warning_state_t){
        .severity = WARN_INFO, .priority = 11, .title = "SHALLOW WATER",
        .icon = &tt_icon_shallow_water,
        .debounce_ms = 0, .trigger_thresh = 2.0f, .clear_thresh = 2.5f,
        .depends_on = DEP_NAV_CAN };
}

/* ── Debounce helper ── */
static void debounce_warning(warning_state_t* w, bool condition, uint32_t now)
{
    /* Hysteresis is handled by the caller — condition already accounts for it */
    if (condition) {
        w->raw_active = true;
        if (w->debounce_ms == 0) {
            w->active = true;
            w->was_active = true;
            if (w->activate_time == 0) w->activate_time = now;
        } else {
            if (w->debounce_start == 0) w->debounce_start = now;
            if (now - w->debounce_start >= w->debounce_ms) {
                w->active = true;
                w->was_active = true;
                if (w->activate_time == 0) w->activate_time = now;
            }
        }
    } else {
        w->raw_active = false;
        w->debounce_start = 0;
        w->active = false;
        if (w->severity != WARN_CRITICAL) {
            /* Non-critical: reset everything on clear */
            w->was_active = false;
            w->acknowledged = false;
            w->activate_time = 0;
        } else if (w->acknowledged) {
            /* Critical that was already ACK'd: fully reset so it can retrigger */
            w->was_active = false;
            w->acknowledged = false;
            w->activate_time = 0;
        }
        /* Critical not yet ACK'd: was_active stays true, overlay persists */
    }
}

/* Hysteresis helper: true if should trigger, false if should clear */
static bool hyst_below(float value, float trigger, float clear, bool currently_active)
{
    if (!currently_active) return value < trigger;
    else                   return value < clear;   /* stay active until above clear */
}

static bool hyst_above(float value, float trigger, float clear, bool currently_active)
{
    if (!currently_active) return value > trigger;
    else                   return value > clear;
}

/* ── Pulse toggle (0.5 Hz color toggle instead of per-frame animation) ── */
static void start_pulse(void)
{
    pulse_running = true;
}

static void stop_pulse(void)
{
    if (!pulse_running) return;
    pulse_running = false;
    lv_obj_set_style_bg_color(overlay_panel, COL_WARN_RED_DARK, 0);
    /* Restore icon to the critical-red telltale color in case pulse stopped
     * mid-bright-phase (where the icon was flipped to white for contrast). */
    lv_obj_set_style_image_recolor(overlay_icon, lv_color_hex(0xff1744), 0);
}

/* Call from warning_overlay_update — toggles color once per second.
 * Anti-phase icon tinting keeps the telltale visible on BOTH phases of the
 * background pulse: dark-red bg → red icon (normal critical tell-tale),
 * bright-red bg → white icon (otherwise icon would blend into bg and vanish). */
static void update_pulse(void)
{
    if (!pulse_running) return;
    static bool last_bright = false;
    bool bright = (lv_tick_get() / 1000) & 1;  /* toggle every 1000ms */
    if (bright != last_bright) {
        last_bright = bright;
        lv_obj_set_style_bg_color(overlay_panel,
            bright ? COL_WARN_RED_BRIGHT : COL_WARN_RED_DARK, 0);
        lv_obj_set_style_image_recolor(overlay_icon,
            bright ? lv_color_white() : lv_color_hex(0xff1744), 0);
    }
}

/* ── ACK button handler ── */
static void ack_btn_cb(lv_event_t* e)
{
    (void)e;
    warning_overlay_ack();
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

/* ── Show/hide overlay ── */
static void show_warning(int32_t id)
{
    if (id < 0 || id >= WID_COUNT) {
        /* Hide both scrim and panel */
        lv_obj_add_flag(overlay_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        stop_pulse();
        shown_warning_id = -1;
        return;
    }

    warning_state_t* w = &warnings[id];
    shown_warning_id = id;

    if (w->icon) lv_image_set_src(overlay_icon, w->icon);
    lv_label_set_text(overlay_title, w->title);
    lv_label_set_text(overlay_detail, w->detail);

    /* If critical and condition has cleared, update detail text */
    if (w->severity == WARN_CRITICAL && !w->active && w->was_active) {
        char cleared_detail[80];
        snprintf(cleared_detail, sizeof(cleared_detail), "%.60s (CLEARED)", w->detail);
        lv_label_set_text(overlay_detail, cleared_detail);
    }

    /* Style by severity. The telltale icon is an A8 image that LVGL renders
     * as recolor × alpha — set the recolor to follow severity. */
    switch (w->severity) {
        case WARN_CRITICAL:
            lv_obj_set_style_border_color(overlay_panel, lv_color_hex(0xff1744), 0);
            lv_obj_set_style_border_width(overlay_panel, 3, 0);
            lv_obj_set_style_bg_color(overlay_panel, COL_WARN_RED_DARK, 0);
            lv_obj_set_style_image_recolor(overlay_icon, lv_color_hex(0xff1744), 0);
            lv_obj_remove_flag(overlay_ack_btn, LV_OBJ_FLAG_HIDDEN);
            if (w->active) start_pulse(); /* pulse only while condition is active */
            else stop_pulse();
            break;
        case WARN_WARNING:
            lv_obj_set_style_border_color(overlay_panel, COL_WARN_YELLOW, 0);
            lv_obj_set_style_border_width(overlay_panel, 2, 0);
            lv_obj_set_style_bg_color(overlay_panel, COL_WARN_AMBER_BG, 0);
            lv_obj_set_style_image_recolor(overlay_icon, COL_WARN_YELLOW, 0);
            lv_obj_remove_flag(overlay_ack_btn, LV_OBJ_FLAG_HIDDEN);
            stop_pulse();
            break;
        case WARN_INFO:
            lv_obj_set_style_border_color(overlay_panel, COL_WARN_BLUE, 0);
            lv_obj_set_style_border_width(overlay_panel, 2, 0);
            lv_obj_set_style_bg_color(overlay_panel, COL_WARN_BLUE_BG, 0);
            lv_obj_set_style_image_recolor(overlay_icon, COL_WARN_BLUE, 0);
            lv_obj_remove_flag(overlay_ack_btn, LV_OBJ_FLAG_HIDDEN);  /* info is dismissable */
            stop_pulse();
            break;
        default:
            break;
    }

    /* Don't unhide scrim — it's transparent and adds rendering overhead.
     * Only show the panel itself. */
    lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ── Public API ── */

void warning_overlay_init(lv_obj_t* parent)
{
    init_warnings();

    /* Dark scrim — dims the rest of the screen */
    overlay_scrim = lv_obj_create(parent);
    lv_obj_set_size(overlay_scrim, 800, 800);
    lv_obj_center(overlay_scrim);
    lv_obj_set_style_bg_opa(overlay_scrim, LV_OPA_TRANSP, 0);  /* no scrim — avoids full-screen alpha blending */
    lv_obj_set_style_border_width(overlay_scrim, 0, 0);
    lv_obj_set_style_radius(overlay_scrim, 0, 0);
    lv_obj_remove_flag(overlay_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay_scrim, LV_OBJ_FLAG_HIDDEN);

    /* Overlay panel — sibling of scrim, on top */
    overlay_panel = lv_obj_create(parent);
    lv_obj_set_size(overlay_panel, 500, 140);
    lv_obj_center(overlay_panel);
    lv_obj_set_style_radius(overlay_panel, 16, 0);
    lv_obj_set_style_bg_color(overlay_panel, COL_WARN_RED_DARK, 0);
    lv_obj_set_style_bg_opa(overlay_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(overlay_panel, lv_color_hex(0xff1744), 0);
    lv_obj_set_style_border_width(overlay_panel, 3, 0);
    lv_obj_set_style_pad_all(overlay_panel, 0, 0);
    lv_obj_set_scrollbar_mode(overlay_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Prevent swipe from bubbling through the warning */
    lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);

    /* Warning icon — A8 telltale image (48x48). The src is set per warning
     * in show_warning(); the recolor style does the tinting per severity
     * (alpha-only images render as recolor × alpha). */
    overlay_icon = lv_image_create(overlay_panel);
    lv_image_set_src(overlay_icon, &tt_icon_engine);    /* placeholder src */
    lv_obj_set_style_image_recolor(overlay_icon, lv_color_hex(0xff1744), 0);
    lv_obj_set_style_image_recolor_opa(overlay_icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(overlay_icon, 16, 14);

    /* Title */
    overlay_title = lv_label_create(overlay_panel);
    lv_label_set_text(overlay_title, "");
    lv_obj_set_style_text_color(overlay_title, COL_TEXT_WHITE, 0);
    lv_obj_set_style_text_font(overlay_title, &lv_font_montserrat_26, 0);
    lv_obj_set_pos(overlay_title, 75, 18);

    /* Detail */
    overlay_detail = lv_label_create(overlay_panel);
    lv_label_set_text(overlay_detail, "");
    lv_obj_set_style_text_color(overlay_detail, lv_color_hex(0x8b949e), 0);
    lv_obj_set_style_text_font(overlay_detail, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(overlay_detail, 75, 58);

    /* ACK button — only for critical */
    overlay_ack_btn = lv_button_create(overlay_panel);
    lv_obj_set_size(overlay_ack_btn, 80, 36);
    lv_obj_set_pos(overlay_ack_btn, 400, 92);
    lv_obj_set_style_radius(overlay_ack_btn, 8, 0);
    lv_obj_set_style_bg_color(overlay_ack_btn, lv_color_hex(0x3a3a3c), 0);
    lv_obj_set_style_border_width(overlay_ack_btn, 0, 0);
    lv_obj_add_event_cb(overlay_ack_btn, ack_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_flag(overlay_ack_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(overlay_ack_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ack_label = lv_label_create(overlay_ack_btn);
    lv_label_set_text(ack_label, "ACK");
    lv_obj_set_style_text_color(ack_label, COL_TEXT_WHITE, 0);
    lv_obj_set_style_text_font(ack_label, &lv_font_montserrat_16, 0);
    lv_obj_center(ack_label);
}

void warning_overlay_update(const gauge_data_t* d)
{
    uint32_t now = lv_tick_get();

    /* ── CAN short-circuit ── */
    /* Engine CAN */
    debounce_warning(&warnings[WID_ENGINE_CAN_TIMEOUT], !d->engine_can_active, now);
    if (!d->engine_can_active) {
        /* Suppress all engine-dependent warnings (stale data) */
        for (int i = 0; i < WID_COUNT; i++) {
            if (warnings[i].depends_on == DEP_ENGINE_CAN) {
                warnings[i].raw_active = false;
                warnings[i].debounce_start = 0;
                if (warnings[i].active) {
                    warnings[i].active = false;
                    warnings[i].acknowledged = false;
                    warnings[i].activate_time = 0;
                }
            }
        }
    } else {
        /* Evaluate engine-dependent warnings */
        snprintf(warnings[WID_EMERGENCY_STOP].detail, sizeof(warnings[0].detail),
            "MAP %.0f kPa", d->map_kpa);
        debounce_warning(&warnings[WID_EMERGENCY_STOP],
            (d->map_kpa < 1.0f || d->map_kpa >= 101.0f) && d->battery_voltage > 7.0f, now);

        float oil_max = s_thr.v[THR_OIL_TEMP_MAX];
        snprintf(warnings[WID_OVERTEMP_OIL].detail, sizeof(warnings[0].detail),
            "Oil %.0f°C - max %.0f°C", d->oil_temp_c, oil_max);
        debounce_warning(&warnings[WID_OVERTEMP_OIL],
            hyst_above(d->oil_temp_c, oil_max, oil_max - 5, warnings[WID_OVERTEMP_OIL].active), now);

        float clt_max = s_thr.v[THR_CLT_TEMP_MAX];
        snprintf(warnings[WID_OVERTEMP_CLT].detail, sizeof(warnings[0].detail),
            "Coolant %.0f°C - max %.0f°C", d->coolant_temp_c, clt_max);
        debounce_warning(&warnings[WID_OVERTEMP_CLT],
            hyst_above(d->coolant_temp_c, clt_max, clt_max - 5, warnings[WID_OVERTEMP_CLT].active), now);

        float oil_min = s_thr.v[THR_OIL_PRESS_MIN];
        snprintf(warnings[WID_LOW_OIL_PRESS].detail, sizeof(warnings[0].detail),
            "%.1f bar - min %.1f bar", d->oil_pressure_kpa / 100.0f, oil_min / 100.0f);
        debounce_warning(&warnings[WID_LOW_OIL_PRESS],
            d->rpm > 400 && hyst_below(d->oil_pressure_kpa, oil_min, oil_min + 15,
                warnings[WID_LOW_OIL_PRESS].active), now);

        snprintf(warnings[WID_LOW_VOLTAGE].detail, sizeof(warnings[0].detail),
            "%.1f V - min 12.0 V", d->battery_voltage);
        debounce_warning(&warnings[WID_LOW_VOLTAGE],
            hyst_below(d->battery_voltage, 12.0f, 12.5f, warnings[WID_LOW_VOLTAGE].active), now);

        snprintf(warnings[WID_LOW_FUEL_PRESS].detail, sizeof(warnings[0].detail),
            "%.0f kPa - min 275 kPa", d->fuel_pressure_kpa);
        debounce_warning(&warnings[WID_LOW_FUEL_PRESS],
            hyst_below(d->fuel_pressure_kpa, 275, 290, warnings[WID_LOW_FUEL_PRESS].active), now);

        snprintf(warnings[WID_LOW_WATER_FLOW].detail, sizeof(warnings[0].detail),
            "%.0f kPa - min 15 kPa", d->coolant_pressure_kpa);
        debounce_warning(&warnings[WID_LOW_WATER_FLOW],
            hyst_below(d->coolant_pressure_kpa, 15, 20, warnings[WID_LOW_WATER_FLOW].active), now);

        float worst_lam = d->lambda1 > d->lambda2 ? d->lambda1 : d->lambda2;
        snprintf(warnings[WID_LAMBDA_OOR].detail, sizeof(warnings[0].detail),
            "L1:%.2f L2:%.2f - range 0.80-0.95", d->lambda1, d->lambda2);
        /* User runs rich — normal lambda 0.80-0.95, nominal 0.85. Trigger as
         * soon as the value leaves that band; clear with a small hysteresis
         * buffer so brief excursions don't latch the warning. */
        bool lam_trigger = (worst_lam < 0.80f || worst_lam > 0.95f);
        bool lam_clear   = (worst_lam >= 0.82f && worst_lam <= 0.93f);
        debounce_warning(&warnings[WID_LAMBDA_OOR],
            warnings[WID_LAMBDA_OOR].active ? !lam_clear : lam_trigger, now);

        float rpm_max = s_thr.v[THR_RPM_REDLINE];
        snprintf(warnings[WID_RPM_REDLINE].detail, sizeof(warnings[0].detail),
            "%.0f RPM - max %.0f", d->rpm, rpm_max);
        debounce_warning(&warnings[WID_RPM_REDLINE],
            hyst_above(d->rpm, rpm_max, rpm_max - 200, warnings[WID_RPM_REDLINE].active), now);
    }

    /* Nav CAN */
    debounce_warning(&warnings[WID_NAV_CAN_TIMEOUT], !d->nav_can_active, now);
    if (!d->nav_can_active) {
        for (int i = 0; i < WID_COUNT; i++) {
            if (warnings[i].depends_on == DEP_NAV_CAN) {
                warnings[i].raw_active = false;
                warnings[i].debounce_start = 0;
                if (warnings[i].active) {
                    warnings[i].active = false;
                    warnings[i].acknowledged = false;
                    warnings[i].activate_time = 0;
                }
            }
        }
    } else {
        float depth_min = s_thr.v[THR_DEPTH_MIN] / 100.0f;   /* cm → m */
        snprintf(warnings[WID_DEPTH_LOW].detail, sizeof(warnings[0].detail),
            "%.1f m - min %.1f m", d->depth_m, depth_min);
        debounce_warning(&warnings[WID_DEPTH_LOW],
            hyst_below(d->depth_m, depth_min, depth_min + 0.5f, warnings[WID_DEPTH_LOW].active), now);
    }

    /* Info warnings are dismissed by ACK (like warnings), not on a timer —
     * a timed auto-dismiss made persistent infos blink every few seconds. */

    /* ── Find highest priority warning to show ── */
    /* Critical: show while was_active && !acknowledged (persists after condition clears)
     * Warning: show while active && !acknowledged
     * Info: show while active */
    int32_t best = -1;
    for (int i = 0; i < WID_COUNT; i++) {
        bool should_show;
        if (warnings[i].severity == WARN_CRITICAL)
            should_show = warnings[i].was_active && !warnings[i].acknowledged;
        else /* WARN_WARNING or WARN_INFO — both dismissable by ACK */
            should_show = warnings[i].active && !warnings[i].acknowledged;

        if (!should_show) continue;
        if (best < 0 || warnings[i].priority < warnings[best].priority)
            best = i;
    }

    update_pulse();

    /* Update overlay */
    if (best != shown_warning_id) {
        show_warning(best);
    } else if (best >= 0) {
        warning_state_t* w = &warnings[best];

        /* Throttle detail text updates to ~2 Hz to avoid per-frame invalidation */
        static uint32_t last_detail_tick = 0;
        uint32_t detail_now = lv_tick_get();
        if (detail_now - last_detail_tick >= 500) {
            last_detail_tick = detail_now;
            const char *current_text = lv_label_get_text(overlay_detail);
            if (w->severity == WARN_CRITICAL && !w->active && w->was_active) {
                char cleared_detail[80];
                snprintf(cleared_detail, sizeof(cleared_detail), "%.60s (CLEARED)", w->detail);
                if (strcmp(current_text, cleared_detail) != 0) {
                    lv_label_set_text(overlay_detail, cleared_detail);
                    stop_pulse();
                    lv_obj_set_style_bg_color(overlay_panel, COL_WARN_RED_DARK, 0);
                }
            } else {
                if (strcmp(current_text, w->detail) != 0) {
                    lv_label_set_text(overlay_detail, w->detail);
                }
                if (w->severity == WARN_CRITICAL && w->active) start_pulse();
            }
        }
    }
}

void warning_overlay_set_thresholds(const warn_thresholds_t* t)
{
    if (t) s_thr = *t;
}

void warning_overlay_ack(void)
{
    if (shown_warning_id >= 0 && shown_warning_id < WID_COUNT) {
        warning_state_t* w = &warnings[shown_warning_id];
        w->acknowledged = true;
        w->was_active = false;
        w->activate_time = 0;
        stop_pulse();

        /* Find next warning to show */
        int32_t next = -1;
        for (int i = 0; i < WID_COUNT; i++) {
            bool should_show;
            if (warnings[i].severity == WARN_CRITICAL)
                should_show = warnings[i].was_active && !warnings[i].acknowledged;
            else /* WARN_WARNING or WARN_INFO */
                should_show = warnings[i].active && !warnings[i].acknowledged;
            if (!should_show) continue;
            if (next < 0 || warnings[i].priority < warnings[next].priority)
                next = i;
        }
        show_warning(next);
    }
}
