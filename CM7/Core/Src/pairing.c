/**
 * @file pairing.c
 * @brief The hub's half of the key exchange, one at a time.
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */

#include <string.h>

#include "cmsis_os.h"
#include "main.h"
#include "hsem_table.h"
#include "FreeRTOS.h"
#include "task.h"

#include "pairing.h"
#include "hubipc.h"
#include "keystore.h"
#include "cfgstoreapi.h"
#include "crypto.h"
#include "ipc.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "telemetry.h"

#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"

#define PAIR_POLL_MS  20u

/* A record with no key yet holds zeros; the store answers no such question.
 * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
static int has_pubkey(const cfg_device_t *rec) {
    static const uint8_t zero[CFG_PUBKEY_BYTES];

    return memcmp(rec->pubkey, zero, sizeof(zero)) != 0;
}

/* Long enough for two scalar multiplications, short enough to free the slot. */
#define PAIR_PENDING_MS  12000u

static uint8_t hub_pub[32];
static uint8_t hub_pub_ready;

static struct {
    uint8_t  active;
    uint32_t dev_id;
    uint32_t started_ms;
    uint8_t  dev_nonce[8];
    uint8_t  dev_pub[KS_PUBKEY_BYTES];   /* carried to the store, learned here */
    crypto_pair_out_t out;
} pending;

static pairing_stats_t stats;

/* The transcript the live derive hashed, kept past its exchange.
 * radio_devices_docs/open_hub/radio/pairing.md */
static uint8_t  last_t[116];
static uint8_t  last_t_valid;
static uint32_t last_t_dev;
static uint32_t last_t_sf;

/* Recovered once from the stored private half, then cached.
 * radio_devices_docs/open_hub/radio/pairing.md */
static int ensure_hub_pub(const uint8_t priv[32]) {
    if (hub_pub_ready)
        return 0;
    if (crypto_x25519_public(priv, hub_pub) != 0)
        return -1;
    hub_pub_ready = 1;
    return 0;
}

/* Constant-time: memcmp leaks where it first differs. */
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;

    for (size_t i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static int nonce_is_zero(const uint8_t n[8]) {
    uint8_t d = 0;

    for (int i = 0; i < 8; i++)
        d |= n[i];
    return d == 0;
}

static void drop_pending(void) {
    mbedtls_platform_zeroize(&pending, sizeof(pending));
}

static void serve_pair_req(const ipc_msg_t *m) {
    ipc_pair_req_evt_t e;
    ipc_pair_rsp_evt_t r;
    const cfg_device_t *rec;
    uint8_t hub_priv[32];
    uint8_t fp[32];
    uint8_t status = IPC_ST_BAD_ARG;

    stats.reqs++;
    if (m->len < sizeof(e)) {
        stats.bad_len++;
        goto refuse;
    }
    memcpy(&e, m->payload, sizeof(e));

    /* Enrolment is what authenticates this exchange.
     * radio_devices_docs/open_hub/radio/pairing.md */
    rec = cfg_find(e.dev_id);
    if (rec == NULL || rec->state == CFG_DEV_FREE) {
        stats.not_enrolled++;
        goto refuse;
    }

    /* The unseeded-RNG signature only; the repeat check below is the general one. */
    if (nonce_is_zero(e.dev_nonce)) {
        stats.zero_nonce++;
        goto refuse;
    }
    /* A nonce this device has already used: every stuck-RNG mode, and replay. */
    if (rec->state == CFG_DEV_PAIRED &&
        memcmp(rec->last_nonce, e.dev_nonce, 8) == 0) {
        stats.repeat_nonce++;
        goto refuse;
    }

    /* A record with no key adopts the one on the wire; one with a key pins it.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    if (has_pubkey(rec) && !ct_equal(e.pubkey, rec->pubkey, sizeof(e.pubkey))) {
        stats.bad_fingerprint++;
        /* The fingerprint of what arrived, which is what the operator can compare. */
        if (mbedtls_sha256(e.pubkey, sizeof(e.pubkey), fp, 0) == 0)
            memcpy(stats.last_fp, fp, sizeof(stats.last_fp));
        memcpy(stats.last_pubkey, e.pubkey, sizeof(stats.last_pubkey));
        goto refuse;
    }

    if (hub_key_get(hub_priv) != 0 || ensure_hub_pub(hub_priv) != 0) {
        stats.no_hub_key++;
        status = IPC_ST_RADIO_ERR;
        goto refuse;
    }

    drop_pending();
    if (crypto_pair_derive(hub_priv, hub_pub, e.pubkey, PAIRING_HUB_ID,
                           e.dev_id, e.superframe, e.dev_nonce,
                           &pending.out) != 0) {
        mbedtls_platform_zeroize(hub_priv, sizeof(hub_priv));
        stats.derive_failed++;
        status = IPC_ST_RADIO_ERR;
        goto refuse;
    }
    mbedtls_platform_zeroize(hub_priv, sizeof(hub_priv));

    /* From the value passed, never re-read: which superframe reached the transcript. */
    memcpy(last_t, pending.out.transcript, sizeof(last_t));
    last_t_dev   = e.dev_id;
    last_t_sf    = e.superframe;
    last_t_valid = 1;

    pending.active     = 1;
    pending.dev_id     = e.dev_id;
    pending.started_ms = (uint32_t)osKernelGetTickCount();
    memcpy(pending.dev_nonce, e.dev_nonce, 8);
    memcpy(pending.dev_pub, e.pubkey, sizeof(pending.dev_pub));

    memcpy(r.eph_pubkey, pending.out.eph_pub, sizeof(r.eph_pubkey));
    memcpy(r.confirm, pending.out.confirm_hub, sizeof(r.confirm));
    stats.derived++;
    (void)ipc_send_event_reply(m, IPC_ST_OK, &r, (uint8_t)sizeof(r));
    return;

refuse:
    /* Always answer: a silent refusal costs CM4 the whole quiesce. */
    (void)ipc_send_event_reply(m, status, NULL, 0);
}

static void serve_pair_conf(const ipc_msg_t *m) {
    ipc_pair_conf_evt_t e;
    ipc_device_keys_t k;
    const cfg_device_t *rec;
    uint8_t status = IPC_ST_BAD_ARG;

    stats.confs++;
    if (m->len < sizeof(e)) {
        stats.bad_len++;
        goto refuse;
    }
    memcpy(&e, m->payload, sizeof(e));

    if (!pending.active || pending.dev_id != e.dev_id) {
        stats.no_pending++;
        goto refuse;
    }
    if (!ct_equal(e.confirm, pending.out.confirm_dev, sizeof(e.confirm))) {
        /* Dropped, never retried under the same ephemeral: a retry is an oracle.
         * radio_devices_docs/open_hub/radio/pairing.md */
        stats.bad_confirm++;
        drop_pending();
        goto refuse;
    }

    rec = cfg_find(e.dev_id);
    if (rec == NULL || rec->state == CFG_DEV_FREE) {
        stats.not_enrolled++;
        goto refuse;
    }

    memset(&k, 0, sizeof(k));
    /* Fetched before the record is written, never after.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (hub_net_key_get(k.hop_key) != 0) {
        stats.errors++;
        status = IPC_ST_RADIO_ERR;
        goto refuse;
    }

    if (cfg_pair_complete(e.dev_id, pending.out.key_session,
                         pending.dev_nonce, pairing_epoch_now(),
                         pending.dev_pub) != 0) {
        stats.store_failed++;
        status = IPC_ST_RADIO_ERR;
        goto refuse;
    }

    k.dev_id       = e.dev_id;
    k.key_gen      = rec->key_gen;
    k.slot         = rec->slot;
    k.report_every = pairing_report_every();
    memcpy(k.session_key, pending.out.key_session, sizeof(k.session_key));

    stats.paired++;
    (void)ipc_send_event_reply(m, IPC_ST_OK, &k, (uint8_t)sizeof(k));
    mbedtls_platform_zeroize(&k, sizeof(k));
    drop_pending();
    return;

refuse:
    (void)ipc_send_event_reply(m, status, NULL, 0);
}

/* Replays the store into a freshly booted CM4.
 * radio_devices_docs/open_hub/radio/pairing.md */
static void install_paired_devices(void) {
    ipc_msg_t reply;
    uint8_t net_key[16];
    uint32_t i;

    if (cfg_live_devices() == 0u)
        return;
    /* Only if something is paired: creating one is a flash write. */
    if (hub_net_key_get(net_key) != 0) {
        stats.errors++;
        return;
    }

    for (i = 0; i < CFG_DEVICE_MAX; i++) {
        const cfg_device_t *r = cfg_at(i);
        ipc_device_keys_t k;

        if (r == NULL || r->state != CFG_DEV_PAIRED)
            continue;

        memset(&k, 0, sizeof(k));
        k.dev_id       = r->dev_id;
        k.key_gen      = r->key_gen;
        k.slot         = r->slot;
        k.report_every = pairing_report_every();
        memcpy(k.session_key, r->session_key, sizeof(k.session_key));
        memcpy(k.hop_key, net_key, sizeof(k.hop_key));

        if (hub_ipc_call(IPC_REQ_INSTALL_DEVICE, 0, &k, (uint8_t)sizeof(k),
                         &reply) == IPC_ST_OK)
            stats.installed++;
        else
            stats.install_failed++;
        mbedtls_platform_zeroize(&k, sizeof(k));
    }
    mbedtls_platform_zeroize(net_key, sizeof(net_key));
}

const uint8_t *pairing_last_transcript(uint32_t *dev_id, uint32_t *superframe) {
    if (!last_t_valid)
        return NULL;
    if (dev_id != NULL)     *dev_id = last_t_dev;
    if (superframe != NULL) *superframe = last_t_sf;
    return last_t;
}

const pairing_stats_t *pairing_get_stats(void) {
    return &stats;
}

/* pair_v4's invitation, built here and keyed by CM4.
 * radio_devices_docs/open_hub/radio/pairing.md */
static struct {
    uint8_t  armed;
    uint32_t dev_id;
    uint32_t expires_ms;
    uint32_t last_target;     /* the superframe of the last frame pushed */
    uint8_t  pending_arm;     /* the CLI asked; the derivation has not run yet */
    uint32_t pending_dev;
    uint32_t pending_ms;
} pi;

static pairing_init_stats_t pi_stats;

const pairing_init_stats_t *pairing_init_get_stats(void) {
    pi_stats.armed = pi.armed;
    return &pi_stats;
}

void pairing_arm_init(uint32_t dev_id, uint32_t window_ms) {
    pi.pending_dev = dev_id;
    pi.pending_ms  = window_ms;
    pi.pending_arm = 1;
}

void pairing_disarm_init(void) {
    pi.armed = 0;
    pi.pending_arm = 0;
}

/* Once per window, never per frame, and z1_derivations measures the claim.
 * radio_devices_docs/open_hub/radio/pairing.md */
static void pair_init_derive(void) {
    const cfg_device_t *rec;
    uint8_t hub_priv[32];

    pi.pending_arm = 0;
    pi.armed = 0;

    rec = cfg_find(pi.pending_dev);
    if (rec == NULL || rec->state == CFG_DEV_FREE) {
        pi_stats.derive_failed++;
        return;
    }
    /* Only the hub's own key: mode OPEN has no secret to derive a MAC key from.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    if (hub_key_get(hub_priv) != 0 || ensure_hub_pub(hub_priv) != 0) {
        mbedtls_platform_zeroize(hub_priv, sizeof(hub_priv));
        pi_stats.derive_failed++;
        return;
    }
    mbedtls_platform_zeroize(hub_priv, sizeof(hub_priv));

    pi.dev_id      = pi.pending_dev;
    pi.expires_ms  = (uint32_t)osKernelGetTickCount() + pi.pending_ms;
    pi.last_target = 0;
    pi.armed       = 1;
    pi_stats.z1_derivations++;
}

/* Builds the next invitation and hands it to CM4 ahead of its superframe.
 * radio_devices_docs/open_hub/radio/pairing.md */
static void pair_init_service(void) {
    ipc_hop_at_t h;
    ipc_pair_init_t msg;
    radio_pair_init_t f;
    ipc_msg_t reply;
    uint32_t now, target;

    if (pi.pending_arm)
        pair_init_derive();
    if (!pi.armed)
        return;
    if ((int32_t)(osKernelGetTickCount() - pi.expires_ms) > 0) {
        pairing_disarm_init();
        return;
    }

    if (hub_ipc_call(IPC_REQ_HOP_AT, 0, NULL, 0, &reply) != IPC_ST_OK ||
        reply.len < sizeof(h))
        return;
    memcpy(&h, reply.payload, sizeof(h));
    now = h.superframe;

    /* One frame in flight, and strictly greater rather than >=.
     * radio_devices_docs/open_hub/radio/pairing.md */
    if (pi.last_target != 0u && (int32_t)(now - pi.last_target) <= 0)
        return;

    /* Two superframes of lead, then rounded up to the retry cadence. */
    target = now + 2u;
    target += (RADIO_PAIR_INIT_EVERY - (target % RADIO_PAIR_INIT_EVERY))
              % RADIO_PAIR_INIT_EVERY;
    /* Guard the build, not the push.
     * radio_devices_docs/open_hub/radio/pairing.md */
    if (target == pi.last_target)
        return;
    pi.last_target = target;

    f.type       = RADIO_FRAME_PAIR_INIT;
    f.version    = RADIO_PAIR_INIT_VERSION;
    f.net_id     = RADIO_NET_ID;
    f.hub_id     = PAIRING_HUB_ID;
    f.dev_id     = pi.dev_id;
    f.superframe = target;
    /* The device learns the hub here; the MAC is zero and says so. ADR-0024 */
    f.mode = RADIO_ENROL_MODE_OPEN;
    memcpy(f.hub_static, hub_pub, sizeof(f.hub_static));
    memset(f.mac, 0, sizeof(f.mac));
    pi_stats.built++;
    memcpy(pi_stats.last_frame, &f, sizeof(f));
    pi_stats.last_len = (uint8_t)sizeof(f);

    memset(&msg, 0, sizeof(msg));
    msg.superframe = target;
    msg.len        = (uint8_t)sizeof(f);
    memcpy(msg.frame, &f, sizeof(f));
    if (hub_ipc_call(IPC_REQ_SET_PAIR_INIT, 0, &msg, (uint8_t)sizeof(msg),
                     &reply) != IPC_ST_OK) {
        pi_stats.push_failed++;
        return;
    }
    pi_stats.pushed++;
    pi_stats.last_superframe = target;
}

/* The poll stays as the fallback: a dead doorbell must not be a wedge. */
static osSemaphoreId_t doorbell_sem;
static uint32_t doorbell_isr, wake_timeout;

/* Re-armed here: one notification is consumed by the interrupt that delivers it. */
void HAL_HSEM_FreeCallback(uint32_t SemMask) {
    if ((SemMask & __HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7)) == 0u)
        return;
    doorbell_isr++;
    /* The NVIC line is enabled from another task; the semaphore may not exist. */
    if (doorbell_sem != NULL)
        (void)osSemaphoreRelease(doorbell_sem);
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7));
}

uint32_t pairing_doorbells(uint32_t *timeouts) {
    if (timeouts != NULL)
        *timeouts = wake_timeout;
    return doorbell_isr;
}

/* Counted here, so nothing has to infer arrivals from a poll. ROADMAP item 2 */
static struct {
    uint32_t seen;
    uint32_t short_payload;    /* an event too small to be the report it claims */
    uint32_t last_tick;        /* when this core handled it, in its own clock */
    ipc_device_report_t last;
} up_evt;

/* Answered even when refused: the reply is what times the hub half.
 * ROADMAP item 2 */
static void serve_uplink(const ipc_msg_t *m) {
    if (m->len < sizeof(up_evt.last)) {
        up_evt.short_payload++;
        (void)ipc_send_event_reply(m, IPC_ST_BAD_ARG, NULL, 0);
        return;
    }
    memcpy(&up_evt.last, m->payload, sizeof(up_evt.last));
    up_evt.last_tick = osKernelGetTickCount();
    up_evt.seen++;
    /* Handed on before the reply, so the deadline is not paid for twice.
     * ROADMAP item 2 */
    telemetry_notify_uplink(&up_evt.last);
    /* Sent after the handling, never before: it is the handling it reports. */
    (void)ipc_send_event_reply(m, IPC_ST_OK, NULL, 0);
}

uint32_t pairing_uplink_events(uint32_t *short_payload, uint32_t *last_tick,
                               ipc_device_report_t *last) {
    if (short_payload != NULL)
        *short_payload = up_evt.short_payload;
    if (last_tick != NULL)
        *last_tick = up_evt.last_tick;
    if (last != NULL)
        *last = up_evt.last;
    return up_evt.seen;
}

void PairingTask(void *argument) {
    ipc_msg_t m;

    (void)argument;
    doorbell_sem = osSemaphoreNew(1, 0, NULL);
    /* Inside a wait that already happens, so it spends none of it.
     * radio_devices_docs/open_hub/security/self-tests.md */
    (void)crypto_selftest_fast();
    /* CM4 is not listening until LwIP is up and its radio has come up.
     * radio_devices_docs/open_hub/radio/pairing.md */
    osDelay(3000);
    install_paired_devices();

    for (;;) {
        while (ipc_poll_event(&m)) {
            switch (m.type) {
            case IPC_EVT_PAIR_REQ:  serve_pair_req(&m);  break;
            case IPC_EVT_PAIR_CONF: serve_pair_conf(&m); break;
            case IPC_EVT_UPLINK:    serve_uplink(&m);    break;
            default:
                (void)ipc_send_event_reply(&m, IPC_ST_UNKNOWN_REQ, NULL, 0);
                break;
            }
        }

        /* The timeout is what makes "one at a time" safe rather than wedged.
         * radio_devices_docs/open_hub/radio/pairing.md */
        if (pending.active &&
            (uint32_t)(osKernelGetTickCount() - pending.started_ms) > PAIR_PENDING_MS) {
            stats.timed_out++;
            drop_pending();
        }

        pair_init_service();

        /* Woken by the doorbell, or by the timeout when it does not come. */
        if (doorbell_sem == NULL)
            osDelay(PAIR_POLL_MS);
        else if (osSemaphoreAcquire(doorbell_sem, PAIR_POLL_MS) != osOK)
            wake_timeout++;
    }
}

/* Granted at pairing, not compiled into the device.
 * radio_devices_docs/open_hub/radio/pairing.md */
static uint8_t report_every = RADIO_REPORT_EVERY_DEFAULT;

uint8_t pairing_report_every(void) {
    return report_every;
}

void pairing_set_report_every(uint8_t n) {
    report_every = (n == 0u) ? 1u : n;
}

/* The epoch a key agreed now belongs to, asked of CM4 rather than kept twice.
 * radio_devices_docs/open_hub/radio/pairing.md */
uint32_t pairing_epoch_now(void) {
    ipc_msg_t reply;
    ipc_timing_t t;

    if (hub_ipc_call(IPC_REQ_GET_TIMING, 0, NULL, 0, &reply) != IPC_ST_OK ||
        reply.len < sizeof(t))
        return 0;
    memcpy(&t, reply.payload, sizeof(t));
    return t.superframe / SUPERFRAME_PER_DAY;
}
