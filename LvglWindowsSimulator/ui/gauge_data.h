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
    bool can_active;
} gauge_data_t;

#ifdef __cplusplus
}
#endif

#endif /* GAUGE_DATA_H */
