#ifndef WARN_THRESHOLDS_H
#define WARN_THRESHOLDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * User-tunable warning thresholds (a subset of the warning system editable on
 * the settings screen). The enum order is the persistence/editor index — keep
 * app_settings.thresholds[] (firmware) in the same order.
 */
typedef enum {
    THR_RPM_REDLINE,    /* RPM */
    THR_OIL_TEMP_MAX,   /* deg C */
    THR_CLT_TEMP_MAX,   /* deg C */
    THR_OIL_PRESS_MIN,  /* kPa */
    THR_DEPTH_MIN,      /* cm */
    THR_COUNT
} thr_id_t;

typedef struct {
    uint16_t v[THR_COUNT];
} warn_thresholds_t;

static inline void warn_thresholds_defaults(warn_thresholds_t* t)
{
    t->v[THR_RPM_REDLINE]   = 5200;
    t->v[THR_OIL_TEMP_MAX]  = 125;
    t->v[THR_CLT_TEMP_MAX]  = 85;
    t->v[THR_OIL_PRESS_MIN] = 150;
    t->v[THR_DEPTH_MIN]     = 200;   /* 2.0 m */
}

#ifdef __cplusplus
}
#endif

#endif /* WARN_THRESHOLDS_H */
