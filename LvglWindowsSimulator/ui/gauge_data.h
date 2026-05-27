#ifndef GAUGE_DATA_H
#define GAUGE_DATA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Engine — from PGN 127488 */
    float rpm;              /* 0-8000 */
    float map_kpa;          /* manifold absolute pressure */

    /* Engine Dynamic — from PGN 127489 */
    float oil_pressure_kpa;
    float oil_temp_c;
    float coolant_temp_c;
    float battery_voltage;
    float fuel_rate_lph;
    float coolant_pressure_kpa;
    float fuel_pressure_kpa;
    uint32_t engine_hours_s;

    /* Transmission (repurposed) — from PGN 127493 */
    float lambda1;
    float lambda2;
    float iat_c;

    /* Navigation */
    double latitude;
    double longitude;
    float sog_knots;
    float cog_degrees;
    float heading_degrees;

    /* Depth & water */
    float depth_m;
    float water_temp_c;

    /* Environmental */
    float baro_pressure_kpa;

    /* Metadata */
    uint32_t last_update_ms;
    bool engine_can_active;  /* any rusEFI ECU PGN received within timeout */
    bool nav_can_active;     /* any Raymarine a78 PGN received within timeout */

    /* Per-PDU validity — false means that source PGN/frame has timed out, and
     * the UI shows "---" for the corresponding value(s). */
    struct {
        bool engine_rapid;   /* PGN 127488: rpm, map_kpa */
        bool engine_dyn;     /* PGN 127489: oil/coolant/battery/fuel/hours/pressures */
        bool iat;            /* PGN 127493: iat_c */
        bool lambda1;        /* raw CAN 0x180 */
        bool lambda2;        /* raw CAN 0x181 */
        bool position;       /* PGN 129025: latitude, longitude */
        bool cogsog;         /* PGN 129026: sog_knots, cog_degrees */
        bool depth;          /* PGN 128267: depth_m */
        bool heading;        /* PGN 127250: heading_degrees */
        bool water_temp;     /* PGN 130312: water_temp_c */
    } valid;
} gauge_data_t;

#ifdef __cplusplus
}
#endif

#endif /* GAUGE_DATA_H */
