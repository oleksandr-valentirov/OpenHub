/* Disciplines the microsecond timebase against the LSE crystal.
 *
 * HSE on this board is the ST-Link MCO, measured +4200 ppm. A TDMA guard band
 * has to cover the drift between hub and device since their last resync, so at
 * 4000 ppm the grid spends its capacity on guard instead of slots. The 32.768
 * kHz crystal is the only accurate reference on the board.
 *
 * TIM16 timestamps LSE edges with the same PLL1 tree TIM2 counts, so the ratio
 * of measured to nominal ticks is the error TIM2 carries. TIM2 is left alone:
 * its prescaler steps 0.5% at a time and cannot express a 0.4% correction. The
 * schedule is corrected instead, in timebase_us_to_ticks(). */

#include "main.h"
#include "calib.h"
#include "timebase.h"

extern TIM_HandleTypeDef htim16;
extern IWDG_HandleTypeDef hiwdg2;

#define LSE_HZ              32768u
#define IC_DIV              8u      /* TIM16 ICPSC: LSE periods per capture */
#define WINDOW_CAPTURES     32u     /* 256 LSE periods, 7.8 ms */
/* A window's own noise is ~350 ppm and does not fall when the window is made
 * longer, so it is beaten down by averaging many windows instead. */
#define AVG_SHIFT           12u     /* 4096 windows, ~32 s */
#define SPAN_TOLERANCE_PCT  2u
#define FIRST_WINDOW_US     500000u

static uint32_t nominal_window;     /* ticks per window when the clock is exact */
static uint32_t span_min;
static uint32_t span_max;
static uint16_t last_cc;
static uint32_t acc_ticks;
static uint16_t acc_caps;
static uint8_t  have_ref;
static uint8_t  started;
static uint8_t  ready;
static uint32_t windows;
/* When the last window landed. `ready` and `windows` only ever say that one
 * did, once - so a stopped LSE leaves the last measured scale being reported as
 * though it were current, with nothing anywhere saying how old it is. */
static uint32_t last_window_tk;
static uint64_t avg_acc;   /* the average, held << AVG_SHIFT */
static uint32_t rejects;
static uint16_t w_lo, w_hi;          /* span extremes inside the window being built */
static uint16_t span_lo, span_hi;    /* and inside the last one that completed */
static int32_t  ppm_min = 0x7FFFFFFF;
static int32_t  ppm_max = -0x7FFFFFFF;

/* Derived from the nominal HSE_VALUE rather than measured, which is the point:
 * the nominal is the reference the LSE-timed window is compared against. */
static uint32_t apb2_timer_hz(void) {
    uint32_t pclk = HAL_RCC_GetPCLK2Freq();
    uint32_t ppre = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) >> RCC_D2CFGR_D2PPRE2_Pos;

    if (ppre < 4u) return pclk;                              /* APB2 undivided */
    if ((RCC->CFGR & RCC_CFGR_TIMPRE) == 0u) return pclk * 2u;
    return (ppre >= 5u) ? pclk * 4u : pclk * 2u;             /* TIMPRE stops at /4 */
}

static void restart(void) {
    /* Drop the pending timestamp, not just the accumulator. It was taken before
     * the gap, so the span from it to the next capture covers an unknown number
     * of LSE periods - and such a span can still land inside the tolerance and
     * be counted as one period, which is a silent error of hundreds of ppm. */
    (void)htim16.Instance->CCR1;                             /* clears CC1IF */
    __HAL_TIM_CLEAR_FLAG(&htim16, TIM_FLAG_CC1OF);
    have_ref  = 0;
    acc_ticks = 0;
    acc_caps  = 0;
    w_lo      = 0xFFFFu;
    w_hi      = 0;
}

void calib_init(void) {
    uint32_t hz = apb2_timer_hz();
    uint32_t span;
    uint32_t deadline;

    span = (uint32_t)(((uint64_t)hz * IC_DIV + LSE_HZ / 2u) / LSE_HZ);
    nominal_window = (uint32_t)(((uint64_t)hz * IC_DIV * WINDOW_CAPTURES + LSE_HZ / 2u) / LSE_HZ);
    span_min = span - span * SPAN_TOLERANCE_PCT / 100u;
    span_max = span + span * SPAN_TOLERANCE_PCT / 100u;

    /* A span past the counter period aliases onto a shorter one, and the 16-bit
     * subtraction below would return a wrong value rather than an error. */
    if (span_max > htim16.Init.Period) return;

    if (HAL_TIM_IC_Start(&htim16, TIM_CHANNEL_1) != HAL_OK) return;
    started = 1;
    restart();

    /* One window before the grid starts, so the first superframe is already on
     * the corrected period. A dead crystal must not hang the radio core. */
    deadline = rfm_micros() + FIRST_WINDOW_US;
    while (!ready && !timebase_elapsed(deadline)) {
        /* The wait is bounded at 500 ms against a 512 ms watchdog, and the LSI
         * driving that watchdog is only specified to +/-50%. A dead crystal is
         * meant to degrade to an uncalibrated timebase, not to a boot loop. */
        HAL_IWDG_Refresh(&hiwdg2);
        calib_poll();
    }
}

void calib_poll(void) {
    uint16_t cc;
    uint16_t span;

    if (!started) return;

    /* Overcapture: a timestamp was overwritten before it was read, so the span
     * no longer covers a known number of LSE periods. The hardware reporting
     * this is why the count never has to be inferred from elapsed time. */
    if (__HAL_TIM_GET_FLAG(&htim16, TIM_FLAG_CC1OF)) {
        __HAL_TIM_CLEAR_FLAG(&htim16, TIM_FLAG_CC1OF);
        restart();
        return;
    }
    if (!__HAL_TIM_GET_FLAG(&htim16, TIM_FLAG_CC1)) return;

    cc = (uint16_t)htim16.Instance->CCR1;   /* the read clears CC1IF */

    if (!have_ref) {
        last_cc  = cc;
        have_ref = 1;
        return;
    }

    span    = (uint16_t)(cc - last_cc);
    last_cc = cc;

    if (span < span_min || span > span_max) {
        rejects++;
        restart();
        return;
    }

    if (span < w_lo) w_lo = span;
    if (span > w_hi) w_hi = span;
    acc_ticks += span;
    if (++acc_caps < WINDOW_CAPTURES) return;

    {
        uint32_t raw = (uint32_t)(((uint64_t)acc_ticks << 24) / nominal_window);
        int32_t  ppm;

        /* An exact running mean while the average is young, exponential once it
         * has 4096 windows behind it - the same accumulator serves both, since
         * a full sum is already the exponential filter's state. Held shifted:
         * updating a Q24 value in place would stall, because a correction
         * smaller than the divisor truncates to zero and the last 244 ppm never
         * arrive. */
        if (windows < (1u << AVG_SHIFT)) {
            avg_acc += raw;
            timebase_set_scale((uint32_t)(avg_acc / (windows + 1u)));
        } else {
            avg_acc = (uint64_t)((int64_t)avg_acc +
                                 ((int64_t)raw - (int64_t)(avg_acc >> AVG_SHIFT)));
            timebase_set_scale((uint32_t)(avg_acc >> AVG_SHIFT));
        }
        windows++;
        last_window_tk = rfm_micros();
        ready = 1;

        /* Tracked on the raw window, not the average: this is the input noise,
         * and averaging it before measuring it would hide exactly what it shows. */
        if (windows > 8u) {
            ppm = (int32_t)((((int64_t)raw - (int64_t)(1 << 24)) * 1000000) >> 24);
            if (ppm < ppm_min) ppm_min = ppm;
            if (ppm > ppm_max) ppm_max = ppm;
        }
    }

    /* last_cc is kept, so windows run back to back with no blind gap. */
    span_lo   = w_lo;
    span_hi   = w_hi;
    acc_ticks = 0;
    acc_caps  = 0;
    w_lo      = 0xFFFFu;
    w_hi      = 0;
}

uint8_t  calib_ready(void)   { return ready; }
uint32_t calib_windows(void) { return windows; }
uint32_t calib_rejects(void) { return rejects; }
/* Ticks since the last accepted window, so a caller can decide whether the
 * scale it is about to use is a measurement or a memory. */
uint32_t calib_age_tk(void) { return ready ? (rfm_micros() - last_window_tk) : 0xFFFFFFFFu; }

uint32_t calib_span_lo(void) { return span_lo; }
uint32_t calib_span_hi(void) { return span_hi; }

int32_t calib_ppm_min(void) { return (windows > 8u) ? ppm_min : 0; }
int32_t calib_ppm_max(void) { return (windows > 8u) ? ppm_max : 0; }

int32_t calib_ppm(void) {
    int64_t d = (int64_t)timebase_scale() - (int64_t)(1 << 24);
    return (int32_t)((d * 1000000) >> 24);
}
