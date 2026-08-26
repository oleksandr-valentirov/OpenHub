/* The downlink rotation over a roster, with no board under it.
 * radio_devices_docs/open_hub/radio/superloop.md */
#include <stdio.h>
#include <string.h>

#include "dlsched.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

#define SLOTS  64u

/* One superframe in two carries a downlink; RADIO_DOWNLINK_EVERY is the rule. */
#define DL_EVERY     2u
#define LISTEN_EVERY 8u

static dev_entry_t roster[SLOTS];

static void seed(uint8_t slot, uint32_t frames_ok, uint8_t every_now) {
    memset(&roster[slot], 0, sizeof(roster[slot]));
    roster[slot].used       = 1u;
    roster[slot].slot       = slot;
    roster[slot].frames_ok  = frames_ok;
    roster[slot].every_now  = every_now;
}

/* Counts who is served in each device's own window over a run of superframes. */
static void run(uint32_t superframes, unsigned *served_in_own_window) {
    uint8_t next = 0u;
    uint32_t sf;

    for (sf = 0u; sf < superframes; sf++) {
        int picked;

        if ((sf % DL_EVERY) != 0u)
            continue;
        picked = dl_pick(roster, (uint8_t)SLOTS, &next, sf);
        if (picked >= 0 && (sf % LISTEN_EVERY) == 0u && sf > 0u)
            served_in_own_window[picked]++;
    }
}

/* A device the hub has never heard is due everywhere and must still win a window. */
static void test_unheard_device_is_not_starved(void) {
    unsigned got[SLOTS] = {0};

    memset(roster, 0, sizeof(roster));
    seed(0u, 0u, 8u);          /* never heard: ADR-0023 may have muted it */
    seed(1u, 1u, 8u);          /* reporting normally */
    run(8u * LISTEN_EVERY, got);

    CHECK(got[0] > 0u, "the unheard device was served in 0 of its 7 windows");
    CHECK(got[1] > 0u, "the heard device was served in 0 of its 7 windows");
}

/* The control: with both heard the cursor alternates, so the harness can pass. */
static void test_two_heard_devices_alternate(void) {
    unsigned got[SLOTS] = {0};

    memset(roster, 0, sizeof(roster));
    seed(0u, 1u, 8u);
    seed(1u, 1u, 8u);
    run(8u * LISTEN_EVERY, got);

    CHECK(got[0] >= 3u, "slot 0 served %u times, want >= 3", got[0]);
    CHECK(got[1] >= 3u, "slot 1 served %u times, want >= 3", got[1]);
}

/* One unheard device alone must still be served, or the rescue branch is moot. */
static void test_lone_unheard_device_is_served(void) {
    unsigned got[SLOTS] = {0};

    memset(roster, 0, sizeof(roster));
    seed(0u, 0u, 8u);
    run(8u * LISTEN_EVERY, got);

    CHECK(got[0] >= 7u, "slot 0 served %u of 7 windows", got[0]);
}

int main(void) {
    printf("== dlsched\n");
    test_unheard_device_is_not_starved();
    test_two_heard_devices_alternate();
    test_lone_unheard_device_is_served();
    if (failures != 0) {
        printf("dlsched: %d failed\n", failures);
        return 1;
    }
    printf("all dlsched checks passed\n");
    return 0;
}
