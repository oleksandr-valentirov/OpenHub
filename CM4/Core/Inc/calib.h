#pragma once

#include <stdint.h>

/* Measures the microsecond timebase against the LSE crystal and publishes the
 * correction to timebase.c. See docs/radio/timebase.md. */

void     calib_init(void);        /* starts the capture unit and takes one window */
void     calib_poll(void);        /* drains captures; call from the superloop */
uint8_t  calib_ready(void);       /* a window has landed, so the scale is measured */
int32_t  calib_ppm(void);         /* timer clock offset from nominal, signed ppm */
uint32_t calib_span_lo(void);  /* span extremes inside the last window, in ticks */
uint32_t calib_span_hi(void);
int32_t  calib_ppm_min(void);  /* spread across windows: wide means a bad measurement */
int32_t  calib_ppm_max(void);
uint32_t calib_windows(void);     /* completed windows */
uint32_t calib_rejects(void);     /* windows dropped by the consistency checks */
uint32_t calib_age_tk(void);      /* ticks since the last accepted window */
