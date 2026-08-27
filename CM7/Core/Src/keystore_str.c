/**
 * @file keystore_str.c
 * @brief The store's failure renderer, alone, so a host test can reach it.
 *
 * keystore.c includes main.h for the HAL and this function needs none of it.
 * Four store conditions once shared one number, so what matters about these
 * strings is that no two of them are equal - a build returning one sentence for
 * every code reads as plausible and passes every other check.
 * radio_devices_docs/open_hub/arch/keystore.md
 */

#include "keystore.h"

const char *ks_fail_str(ks_fail_t f) {
    switch (f) {
    case KS_FAIL_NONE:       return "none";
    case KS_FAIL_NOT_READY:  return "the store was never initialised";
    case KS_FAIL_LATCHED:    return "an earlier write failed and latched it shut";
    case KS_FAIL_LOG_FULL:   return "both sectors are used; only an erase reclaims";
    case KS_FAIL_UNLOCK:     return "HAL_FLASH_Unlock refused";
    case KS_FAIL_PROGRAM:    return "HAL_FLASH_Program refused";
    case KS_FAIL_LOCK:       return "the record landed and HAL_FLASH_Lock refused";
    case KS_FAIL_CACHE_FULL: return "flash took it; the RAM cache is full";
    case KS_FAIL_SCAN_OVER:  return "boot found more device ids than the cache fits";
    case KS_FAIL_RETIRED:    return "retired: the configuration store owns these sectors";
    }
    /* No default: above, so -Wswitch catches a wordless enumerator. */
    return "unknown";
}
