#include <Windows.h>
#include <math.h>

#include <LvglWindowsIconResource.h>

#include "lvgl/lvgl.h"
#include "ui/golden_screen.h"
#include "ui/gauge_data.h"
#include "ui/map_renderer.h"

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
    sim_data.sog_knots = 7.2f;
    sim_data.cog_degrees = 247;
    sim_data.depth_m = 8.5f;
    sim_data.water_temp_c = 18;
    sim_data.baro_pressure_kpa = 101.3f;
    sim_data.can_active = true;
}

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

    /* Oil pressure follows RPM */
    sim_data.oil_pressure_kpa = 150 + (sim_data.rpm / 6000.0f) * 350.0f;

    /* Temperatures drift slowly */
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

    sim_data.fuel_rate_lph = 5 + (sim_data.rpm / 6000.0f) * 30.0f;
    sim_data.fuel_pressure_kpa = 370 + 20 * sinf(t * 0.5f);
    sim_data.coolant_pressure_kpa = 45 + 20 * sinf(t * 0.35f);
}

static bool key_plus_was_down = false;
static bool key_minus_was_down = false;

static void sim_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    sim_data_update();
    golden_screen_update(&sim_data);

    /* Poll +/- keys for map zoom (edge-triggered) */
    bool plus_down = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
    bool minus_down = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;

    if (plus_down && !key_plus_was_down)  map_renderer_zoom_in();
    if (minus_down && !key_minus_was_down) map_renderer_zoom_out();

    key_plus_was_down = plus_down;
    key_minus_was_down = minus_down;
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

    /* Initialize simulated data and create the golden screen */
    sim_data_init();
    golden_screen_set_tile_path("C:/Data/marine-gauge/tools/MAP_BIN");
    golden_screen_create();
    golden_screen_update(&sim_data);

    /* Timer to update simulated data at ~50Hz */
    lv_timer_create(sim_timer_cb, 20, NULL);

    while (1)
    {
        uint32_t time_till_next = lv_timer_handler();
        lv_delay_ms(time_till_next);
    }

    return 0;
}
