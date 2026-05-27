#include <Windows.h>
#include <math.h>

#include <LvglWindowsIconResource.h>

#include "lvgl/lvgl.h"
#include "ui/screen_manager.h"
#include "ui/gauge_data.h"
#include "ui/map_renderer.h"
#include "ui/golden_screen.h"

/*
 * Marine Gauge Simulator — Golden Screen
 *
 * 800x800 round display with full gauge layout.
 * Simulated data sweeps through realistic values for visual testing.
 */

#define DISP_HOR  800
#define DISP_VER  800

/* Simulated gauge data — sweeps through values over time */
static gauge_data_t sim_data;
static bool sim_low_oil = false;
static bool sim_overtemp = false;
static uint32_t sim_tick = 0;

static void sim_data_init(void)
{
    memset(&sim_data, 0, sizeof(sim_data));
    sim_data.rpm = 850;
    sim_data.map_kpa = 45;
    sim_data.oil_pressure_kpa = 350;
    sim_data.oil_temp_c = 85;
    sim_data.coolant_temp_c = 72;
    sim_data.battery_voltage = 14.1f;
    sim_data.fuel_rate_lph = 8.5f;
    sim_data.coolant_pressure_kpa = 55;
    sim_data.fuel_pressure_kpa = 380;
    sim_data.lambda1 = 1.00f;
    sim_data.lambda2 = 1.00f;
    sim_data.iat_c = 32;
    sim_data.latitude = 45.00;
    sim_data.longitude = 14.61;
    sim_data.sog_knots = 7.2f;
    sim_data.cog_degrees = 247;
    sim_data.depth_m = 8.5f;
    sim_data.water_temp_c = 18;
    sim_data.baro_pressure_kpa = 101.3f;
    sim_data.engine_can_active = true;
    sim_data.nav_can_active = true;
    /* Mark every PDU valid so the simulator never shows "---". */
    sim_data.valid.engine_rapid = sim_data.valid.engine_dyn = sim_data.valid.iat = true;
    sim_data.valid.lambda1 = sim_data.valid.lambda2 = true;
    sim_data.valid.position = sim_data.valid.cogsog = sim_data.valid.depth = true;
    sim_data.valid.heading = sim_data.valid.water_temp = true;
}

/* Settings hooks — no backlight or NVS in the simulator. */
static void sim_set_brightness(uint8_t pct) { (void)pct; }
static void sim_set_demo(bool demo) { (void)demo; }
static void sim_set_threshold(int id, uint16_t val) { (void)id; (void)val; }

static void sim_data_update(void)
{
    sim_tick++;
    /* Slow sinusoidal sweep for smooth demo */
    float t = sim_tick * 0.002f;

    /* RPM: idle 700 → cruise 3000 → WOT 5800, with a smooth cycle */
    float rpm_base = 3000.0f + 2500.0f * sinf(t * 0.7f);
    sim_data.rpm = fmaxf(500, fminf(5800, rpm_base + 150 * sinf(t * 3.0f)));

    /* MAP follows RPM loosely */
    sim_data.map_kpa = 30 + (sim_data.rpm / 6000.0f) * 80.0f + 5 * sinf(t * 2.0f);

    /* Oil pressure follows RPM (unless overridden by test key) */
    if (!sim_low_oil)
        sim_data.oil_pressure_kpa = 150 + (sim_data.rpm / 6000.0f) * 350.0f;

    /* Temperatures drift slowly (unless overridden by test key) */
    if (!sim_overtemp)
        sim_data.oil_temp_c = 85 + 15 * sinf(t * 0.3f);
    sim_data.coolant_temp_c = 70 + 8 * sinf(t * 0.25f);
    sim_data.water_temp_c = 16 + 4 * sinf(t * 0.1f);

    /* Battery voltage */
    sim_data.battery_voltage = 13.8f + 0.5f * sinf(t * 0.4f);

    /* Lambda oscillates near stoich */
    sim_data.lambda1 = 1.00f + 0.05f * sinf(t * 1.5f);
    sim_data.lambda2 = 1.00f + 0.05f * sinf(t * 1.5f + 0.3f);

    /* IAT */
    sim_data.iat_c = 30 + 8 * sinf(t * 0.2f);

    /* SOG varies like boat speed */
    sim_data.sog_knots = 8.0f + 6.0f * sinf(t * 0.15f);
    sim_data.depth_m = 6.0f + 5.0f * sinf(t * 0.08f);

    /* GPS: slow circle near Punat (~0.01 deg radius ≈ 1km) */
    float gps_t = t * 0.05f;
    sim_data.latitude  = 45.00 + 0.05 * sin(gps_t);   /* ~290px radius: crosses the map deadband */
    sim_data.longitude = 14.61 + 0.05 * cos(gps_t);
    sim_data.cog_degrees = fmodf(90.0f - gps_t * 180.0f / 3.14159f, 360.0f);
    if (sim_data.cog_degrees < 0) sim_data.cog_degrees += 360.0f;

    sim_data.fuel_rate_lph = 5 + (sim_data.rpm / 6000.0f) * 30.0f;
    sim_data.fuel_pressure_kpa = 370 + 20 * sinf(t * 0.5f);
    sim_data.coolant_pressure_kpa = 45 + 20 * sinf(t * 0.35f);
}

static bool key_plus_was_down = false;
static bool key_minus_was_down = false;
static bool key_left_was_down = false;
static bool key_right_was_down = false;
static bool key_w_was_down = false;
static bool key_e_was_down = false;

static void sim_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    sim_data_update();
    screen_manager_update(&sim_data);

    /* Poll +/- keys for map zoom (edge-triggered) */
    bool plus_down = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
    bool minus_down = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;

    /* Zoom whichever map is on the active screen */
    map_renderer_t* active_map = screen_manager_get_active_map();
    if (active_map) {
        if (plus_down && !key_plus_was_down)  map_renderer_zoom_in(active_map);
        if (minus_down && !key_minus_was_down) map_renderer_zoom_out(active_map);
    }

    key_plus_was_down = plus_down;
    key_minus_was_down = minus_down;

    /* Poll arrow keys for screen navigation (edge-triggered) */
    bool left_down = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    bool right_down = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

    if (left_down && !key_left_was_down)   screen_manager_prev();
    if (right_down && !key_right_was_down) screen_manager_next();

    /* W = toggle simulated low oil pressure, E = toggle overtemperature */
    bool w_down = (GetAsyncKeyState('W') & 0x8000) != 0;
    bool e_down = (GetAsyncKeyState('E') & 0x8000) != 0;
    if (w_down && !key_w_was_down) {
        sim_low_oil = !sim_low_oil;
        sim_data.oil_pressure_kpa = sim_low_oil ? 100.0f : 350.0f;
    }
    if (e_down && !key_e_was_down) {
        sim_overtemp = !sim_overtemp;
        sim_data.oil_temp_c = sim_overtemp ? 130.0f : 85.0f;
    }
    key_w_was_down = w_down;
    key_e_was_down = e_down;

    key_left_was_down = left_down;
    key_right_was_down = right_down;
}

int main()
{
    lv_init();

#if LV_TXT_ENC == LV_TXT_ENC_UTF8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    int32_t zoom_level = 100;
    bool allow_dpi_override = false;
    bool simulator_mode = true;
    lv_display_t* display = lv_windows_create_display(
        L"Marine Gauge Simulator 800x800",
        DISP_HOR,
        DISP_VER,
        zoom_level,
        allow_dpi_override,
        simulator_mode);
    if (!display)
    {
        return -1;
    }

    HWND window_handle = lv_windows_get_display_window_handle(display);
    if (!window_handle)
    {
        return -1;
    }

    HICON icon_handle = LoadIconW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCE(IDI_LVGL_WINDOWS));
    if (icon_handle)
    {
        SendMessageW(window_handle, WM_SETICON, TRUE, (LPARAM)icon_handle);
        SendMessageW(window_handle, WM_SETICON, FALSE, (LPARAM)icon_handle);
    }

    lv_indev_t* pointer_indev = lv_windows_acquire_pointer_indev(display);
    if (!pointer_indev)
    {
        return -1;
    }

    lv_indev_t* keypad_indev = lv_windows_acquire_keypad_indev(display);
    if (!keypad_indev)
    {
        return -1;
    }

    lv_indev_t* encoder_indev = lv_windows_acquire_encoder_indev(display);
    if (!encoder_indev)
    {
        return -1;
    }

    /* Initialize simulated data and create all screens */
    sim_data_init();
    screen_manager_set_tile_path("C:/Data/marine-gauge/tools/MAP_BIN");
    screen_manager_set_alt_tile_path("C:/Data/marine-gauge/tools/MAP_DARK");
    screen_hooks_t hooks = { sim_set_brightness, sim_set_demo, sim_set_threshold };
    screen_manager_set_hooks(&hooks, 100, true);
    warn_thresholds_t thr; warn_thresholds_defaults(&thr);
    screen_manager_set_thresholds(&thr);
    screen_manager_create();
    screen_manager_set_demo(true);   /* simulator always runs on simulated data */
    screen_manager_update(&sim_data);

    /* Timer to update simulated data at ~50Hz */
    lv_timer_create(sim_timer_cb, 50, NULL);  /* 20Hz data update */

    while (1)
    {
        uint32_t time_till_next = lv_timer_handler();
        lv_delay_ms(time_till_next);
    }

    return 0;
}
