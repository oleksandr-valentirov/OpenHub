/**
 * @file test_failstr.c
 * @brief Two renderers exist to separate cases, so no two of their words may match.
 *
 * Four store conditions once shared one number and two IPC failures shared
 * another. Both renderers name their case now - and a build that returned one
 * string for every code would print a plausible sentence and pass every other
 * check there is, which is what this suite exists to refuse.
 * radio_devices_docs/open_hub/arch/ipc.md,
 * radio_devices_docs/open_hub/arch/keystore.md
 */

#include <stdio.h>
#include <string.h>

#include "keystore.h"
#include "hubipc.h"

static int fails;
static int checks;

#define CHECK(cond, ...) do {                                    \
    checks++;                                                    \
    if (!(cond)) {                                               \
        printf("FAIL %s:%d  ", __FILE__, __LINE__);              \
        printf(__VA_ARGS__);                                     \
        printf("\n");                                            \
        fails++;                                                 \
    }                                                            \
} while (0)

/* Named, not counted: 0..N passes on a wordless enumerator. */
static const ks_fail_t KS_CASES[] = {
    KS_FAIL_NONE, KS_FAIL_NOT_READY, KS_FAIL_LATCHED, KS_FAIL_LOG_FULL,
    KS_FAIL_UNLOCK, KS_FAIL_PROGRAM, KS_FAIL_LOCK, KS_FAIL_CACHE_FULL,
    KS_FAIL_SCAN_OVER, KS_FAIL_RETIRED
};
static const char *const KS_NAMES[] = {
    "KS_FAIL_NONE", "KS_FAIL_NOT_READY", "KS_FAIL_LATCHED", "KS_FAIL_LOG_FULL",
    "KS_FAIL_UNLOCK", "KS_FAIL_PROGRAM", "KS_FAIL_LOCK", "KS_FAIL_CACHE_FULL",
    "KS_FAIL_SCAN_OVER", "KS_FAIL_RETIRED"
};

static const int IPC_CASES[] = {
    IPC_ST_OK, IPC_ST_UNKNOWN_REQ, IPC_ST_BAD_ARG, IPC_ST_RADIO_ERR,
    HUB_IPC_NO_REPLY, HUB_IPC_ERR_ARG, HUB_IPC_ERR_INIT, HUB_IPC_ERR_BUSY,
    HUB_IPC_ERR_SEND
};
static const char *const IPC_NAMES[] = {
    "IPC_ST_OK", "IPC_ST_UNKNOWN_REQ", "IPC_ST_BAD_ARG", "IPC_ST_RADIO_ERR",
    "HUB_IPC_NO_REPLY", "HUB_IPC_ERR_ARG", "HUB_IPC_ERR_INIT",
    "HUB_IPC_ERR_BUSY", "HUB_IPC_ERR_SEND"
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Takes the count from before the arm, so this line can contradict itself.
 * radio_devices_docs/open_hub/arch/ipc.md */
static void report(const char *what, size_t n, int before) {
    if (fails == before)
        printf("  %-28s %2zu cases, all distinct\n", what, n);
    else
        printf("  %-28s %2zu cases, %d NOT distinct\n", what, n,
               fails - before);
}

static void test_keystore_words(void) {
    int before = fails;
    size_t i, j;

    for (i = 0; i < COUNT(KS_CASES); i++) {
        const char *s = ks_fail_str(KS_CASES[i]);

        CHECK(s != NULL, "%s rendered NULL", KS_NAMES[i]);
        if (s == NULL)
            continue;
        CHECK(s[0] != '\0', "%s rendered an empty string", KS_NAMES[i]);
        /* The fallback is a real string, so a missing case reads as a word. */
        CHECK(strcmp(s, "unknown") != 0,
              "%s fell through to the unknown fallback", KS_NAMES[i]);
    }

    for (i = 0; i < COUNT(KS_CASES); i++)
        for (j = i + 1; j < COUNT(KS_CASES); j++)
            CHECK(strcmp(ks_fail_str(KS_CASES[i]),
                         ks_fail_str(KS_CASES[j])) != 0,
                  "%s and %s share the word \"%s\"",
                  KS_NAMES[i], KS_NAMES[j], ks_fail_str(KS_CASES[i]));

    report("ks_fail_str", COUNT(KS_CASES), before);
}

static void test_ipc_words(void) {
    int before = fails;
    size_t i, j;

    for (i = 0; i < COUNT(IPC_CASES); i++) {
        const char *s = hub_ipc_str(IPC_CASES[i]);

        CHECK(s != NULL, "%s rendered NULL", IPC_NAMES[i]);
        if (s == NULL)
            continue;
        CHECK(s[0] != '\0', "%s rendered an empty string", IPC_NAMES[i]);
        CHECK(strcmp(s, "an unknown status") != 0,
              "%s fell through to the unknown fallback", IPC_NAMES[i]);
    }

    for (i = 0; i < COUNT(IPC_CASES); i++)
        for (j = i + 1; j < COUNT(IPC_CASES); j++)
            CHECK(strcmp(hub_ipc_str(IPC_CASES[i]),
                         hub_ipc_str(IPC_CASES[j])) != 0,
                  "%s and %s share the word \"%s\"",
                  IPC_NAMES[i], IPC_NAMES[j], hub_ipc_str(IPC_CASES[i]));

    report("hub_ipc_str", COUNT(IPC_CASES), before);
}

/* An unknown code rendering as a known one is worse than no renderer. */
static void test_fallbacks(void) {
    int before = fails;
    const char *ks = ks_fail_str((ks_fail_t)0x7Fu);
    const char *ip = hub_ipc_str(-99);
    size_t i;

    CHECK(ks != NULL && strcmp(ks, "unknown") == 0,
          "an out-of-range ks_fail_t rendered \"%s\"", ks ? ks : "(null)");
    CHECK(ip != NULL && strcmp(ip, "an unknown status") == 0,
          "an out-of-range hub_ipc rc rendered \"%s\"", ip ? ip : "(null)");

    for (i = 0; i < COUNT(KS_CASES); i++)
        CHECK(strcmp(ks_fail_str(KS_CASES[i]), ks) != 0,
              "%s is indistinguishable from the unknown fallback", KS_NAMES[i]);
    for (i = 0; i < COUNT(IPC_CASES); i++)
        CHECK(strcmp(hub_ipc_str(IPC_CASES[i]), ip) != 0,
              "%s is indistinguishable from the unknown fallback", IPC_NAMES[i]);

    if (fails == before)
        printf("  %-28s both reachable, neither shadows a case\n", "fallbacks");
    else
        printf("  %-28s %d problem(s)\n", "fallbacks", fails - before);
}

/* A shrunk check count reads exactly like a clean run.
 * CM4/test refuses one; so does this. */
#define CHECKS_EXPECTED 159

int main(void) {
    printf("== failstr\n");
    test_keystore_words();
    test_ipc_words();
    test_fallbacks();

    if (checks < CHECKS_EXPECTED) {
        printf("FAIL the suite ran %d checks and %d were expected\n",
               checks, CHECKS_EXPECTED);
        fails++;
    }
    if (fails != 0) {
        printf("\n%d failure(s) over %d checks\n", fails, checks);
        return 1;
    }
    printf("\nall failstr checks passed (%d checks)\n", checks);
    return 0;
}
