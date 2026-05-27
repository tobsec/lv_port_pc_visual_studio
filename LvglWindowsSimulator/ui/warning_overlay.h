#ifndef WARNING_OVERLAY_H
#define WARNING_OVERLAY_H

#include "lvgl/lvgl.h"
#include "gauge_data.h"
#include "warn_thresholds.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WARN_NONE = 0,
    WARN_INFO,
    WARN_WARNING,
    WARN_CRITICAL
} warn_severity_t;

/* Initialize the warning overlay. Parent should be the circle container
 * (sibling of tileview, visible across all screens). */
void warning_overlay_init(lv_obj_t* parent);

/* Evaluate gauge data and update warning state. */
void warning_overlay_update(const gauge_data_t* data);

/* Acknowledge the current critical warning (from touch/button). */
void warning_overlay_ack(void);

/* Set the user-tunable warning thresholds (applied immediately). */
void warning_overlay_set_thresholds(const warn_thresholds_t* t);

#ifdef __cplusplus
}
#endif

#endif /* WARNING_OVERLAY_H */
