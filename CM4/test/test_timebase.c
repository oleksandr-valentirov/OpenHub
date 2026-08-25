/**
 * @file test_timebase.c
 * @brief The hub's clock backend, which every scheduled interval passes through.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */
#include <stdio.h>
#include <stdint.h>

#include "main.h"
/* The real header, by path, not the one -I shim would find.
 * radio_devices_docs/open_hub/testing/host-tests.md */
#include "../Core/Inc/clock.h"

/* timebase.c reads the counter through this; the test drives it. */
static TIM_TypeDef tim2_regs;
TIM_HandleTypeDef htim2 = { &tim2_regs };

static int failures;
static int checks;

static void eq(uint32_t got, uint32_t want, const char *what) {
    checks++;
    if (got != want) {
        printf("FAIL %-52s got %lu want %lu\n", what,
               (unsigned long)got, (unsigned long)want);
        failures++;
    }
}

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

#define Q24  (1u << 24)

/* --- the guard, which is what this file was written for ---------------- */

static void a_zero_scale_is_refused_and_the_refusal_is_counted(void) {
    uint32_t before = timebase_scale();
    uint32_t refused = timebase_scale_refused();

    timebase_set_scale(0u);

    eq(timebase_scale(), before, "a zero scale does not reach the divisor");
    eq(timebase_scale_refused(), refused + 1u,
       "and the refusal is counted, not silent");

    /* After the refusal the clock still converts.
     * radio_devices_docs/open_hub/radio/timebase.md */
    eq(timebase_ticks_to_us(1000u), 1000u,
       "the conversion still works after a refused write");
}

static void a_refused_write_does_not_hide_a_later_good_one(void) {
    timebase_set_scale(0u);
    timebase_set_scale(Q24 + Q24 / 1000u);      /* +1000 ppm */
    check(timebase_scale() != Q24,
          "a good write after a refused one is installed");
    timebase_set_scale(Q24);
}

/* --- the conversions -------------------------------------------------- */

static void at_nominal_both_conversions_are_the_identity(void) {
    timebase_set_scale(Q24);
    eq(timebase_us_to_ticks(2000000u), 2000000u, "nominal: us to ticks");
    eq(timebase_ticks_to_us(2000000u), 2000000u, "nominal: ticks to us");
}

/* An exact scale for the arithmetic, a measured one for the relation.
 * radio_devices_docs/open_hub/radio/timebase.md */
static void an_exact_double_scale_doubles_the_tick_count(void) {
    timebase_set_scale(2u * Q24);              /* a 2 MHz tick, exact in Q24 */

    eq(timebase_us_to_ticks(2000000u), 4000000u,
       "at two ticks per microsecond a superframe is 4000000 ticks");
    eq(timebase_ticks_to_us(4000000u), 2000000u,
       "and 4000000 ticks is a superframe again");
    timebase_set_scale(Q24);
}

static void a_fast_clock_needs_more_ticks_for_the_same_interval(void) {
    /* The scale the hub actually measured: 2381 ppm fast, item 35's mean. */
    const uint32_t ppm = 2381u;
    const uint32_t scale = Q24 + (uint32_t)(((uint64_t)Q24 * ppm) / 1000000u);
    uint32_t tk, back;

    timebase_set_scale(scale);
    tk = timebase_us_to_ticks(2000000u);
    back = timebase_ticks_to_us(tk);

    /* The relation, not a literal: rounding moves the literal. */
    check(tk > 2000000u, "a fast clock needs more ticks than microseconds");
    {
        long implied = (long)tk - 2000000L;         /* ticks of excess */
        long want = (long)((2000000ull * ppm) / 1000000ull);
        checks++;
        if (implied < want - 2 || implied > want + 2) {
            printf("FAIL %-52s excess %ld want ~%ld\n",
                   "the excess is the ppm the clock is fast by", implied, want);
            failures++;
        }
    }
    checks++;
    if (back < 1999998u || back > 2000002u) {
        printf("FAIL %-52s got %lu\n",
               "and the two conversions invert", (unsigned long)back);
        failures++;
    }
    timebase_set_scale(Q24);
}

static void the_scale_is_reported_so_a_figure_can_carry_its_ppm(void) {
    timebase_set_scale(Q24 + Q24 / 1000u);
    eq(timebase_scale(), Q24 + Q24 / 1000u, "the scale in force is readable");
    timebase_set_scale(Q24);
    eq(timebase_scale(), Q24, "and it is settable back");
}

/* --- the clock, and the wrap ------------------------------------------ */

static void elapsed_is_correct_across_the_counter_wrap(void) {
    /* Short of the 32-bit wrap, with a deadline past it. */
    uint32_t before = (uint32_t)(0u - 1000u);
    uint32_t deadline = before + 2000u;         /* wraps */

    tim2_regs.CNT = before;
    eq(timebase_now(), before, "the clock reads the counter");
    eq(timebase_elapsed(deadline), 0u, "a deadline past the wrap is still ahead");

    tim2_regs.CNT = deadline - 1u;
    eq(timebase_elapsed(deadline), 0u, "one tick short is still ahead");

    tim2_regs.CNT = deadline;
    eq(timebase_elapsed(deadline), 1u, "the deadline instant itself is reached");

    tim2_regs.CNT = deadline + 1u;
    eq(timebase_elapsed(deadline), 1u, "and past it, having wrapped");
}

int main(void) {
    tim2_regs.CNT = 0u;

    at_nominal_both_conversions_are_the_identity();
    an_exact_double_scale_doubles_the_tick_count();
    a_fast_clock_needs_more_ticks_for_the_same_interval();
    the_scale_is_reported_so_a_figure_can_carry_its_ppm();
    a_zero_scale_is_refused_and_the_refusal_is_counted();
    a_refused_write_does_not_hide_a_later_good_one();
    elapsed_is_correct_across_the_counter_wrap();

    printf("timebase: %s (%d checks, %lu write(s) refused)\n",
           failures ? "FAILED" : "ok", checks,
           (unsigned long)timebase_scale_refused());
    return failures ? 1 : 0;
}
