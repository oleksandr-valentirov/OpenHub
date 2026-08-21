/**
 * @file ipc.c
 * @brief The ring mechanics both cores compile, with one writer per index.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */
#include <string.h>

#include "ipc.h"
#include "hsem_table.h"
#include "stm32h7xx_hal.h"

/* Both cores define it; both linkers put .shared_mem at the RAM_D3 origin. */
SHARED_MEM ipc_shared_t shared_ipc;

/* One counter per core; a reply always echoes rather than allocating. */
static uint16_t next_seq = 1;
static uint32_t stale_replies = 0;
static uint32_t stale_event_replies = 0;

static uint16_t alloc_seq(void) {
    uint16_t seq = next_seq++;

    if (next_seq == 0u)               /* 0 is never a live sequence number */
        next_seq = 1;
    return seq;
}

static uint32_t ring_next(uint32_t i) {
    return (i + 1u) & (IPC_RING_SLOTS - 1u);
}

static int ring_push(ipc_ring_t *r, const ipc_msg_t *m) {
    uint32_t head = r->head;
    uint32_t slot = ring_next(head);

    if (slot == r->tail)
        return 0;                     /* full: the consumer has not kept up */

    r->slot[head] = *m;
    __DMB();                          /* the slot must be visible before head moves */
    r->head = slot;
    return 1;
}

static int ring_pop(ipc_ring_t *r, ipc_msg_t *m) {
    uint32_t tail = r->tail;

    if (tail == r->head)
        return 0;                     /* empty */

    *m = r->slot[tail];
    __DMB();                          /* the slot must be read before tail moves */
    r->tail = ring_next(tail);
    return 1;
}

/* A doorbell, not a lock: the data is already safe without it. */
static void ipc_ring_doorbell(uint32_t sem) {
    HAL_HSEM_FastTake(sem);
    HAL_HSEM_Release(sem, 0);
}

static void msg_fill(ipc_msg_t *m, uint16_t seq, uint8_t type, uint8_t status,
                     const void *payload, uint8_t len) {
    memset(m, 0, sizeof(*m));
    m->seq = seq;
    m->type = type;
    m->status = status;
    m->len = (len > IPC_PAYLOAD_MAX) ? IPC_PAYLOAD_MAX : len;
    if (payload != NULL && m->len > 0u)
        memcpy(m->payload, payload, m->len);
}

void ipc_init(void) {
    memset(&shared_ipc, 0, sizeof(shared_ipc));
    __DMB();                          /* rings empty before the header claims they are */
    shared_ipc.version = IPC_VERSION;
    shared_ipc.magic = IPC_MAGIC;
}

int ipc_ready(void) {
    return (shared_ipc.magic == IPC_MAGIC) && (shared_ipc.version == IPC_VERSION);
}

int ipc_send_request(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out) {
    ipc_msg_t m;
    uint16_t seq;

    if (!ipc_ready())
        return 1;

    seq = alloc_seq();
    msg_fill(&m, seq, type, 0, payload, len);
    if (!ring_push(&shared_ipc.req, &m))
        return 1;

    if (seq_out != NULL)
        *seq_out = seq;

    ipc_ring_doorbell(HSEM_M7_TO_M4_RFM);
    return 0;
}

int ipc_poll_reply(uint16_t seq, ipc_msg_t *out) {
    ipc_msg_t m;

    while (ring_pop(&shared_ipc.rsp, &m)) {
        if (m.seq == seq) {
            *out = m;
            return 1;
        }
        /* Anything else answers a request that already timed out. */
        stale_replies++;
    }
    return 0;
}

int ipc_poll_request(ipc_msg_t *out) {
    if (!ipc_ready())
        return 0;
    return ring_pop(&shared_ipc.req, out);
}

int ipc_send_reply(const ipc_msg_t *req, uint8_t status, const void *payload, uint8_t len) {
    ipc_msg_t m;

    msg_fill(&m, req->seq, req->type, status, payload, len);
    if (!ring_push(&shared_ipc.rsp, &m))
        return 1;

    ipc_ring_doorbell(HSEM_M4_TO_M7);
    return 0;
}

/* The mirror of ipc_send_request, sharing the M4->M7 doorbell.
 * radio_devices_docs/open_hub/arch/ipc.md */
int ipc_send_event(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out) {
    ipc_msg_t m;
    uint16_t seq;

    if (!ipc_ready())
        return 1;

    seq = alloc_seq();
    msg_fill(&m, seq, type, 0, payload, len);
    if (!ring_push(&shared_ipc.evt, &m))
        return 1;

    if (seq_out != NULL)
        *seq_out = seq;

    ipc_ring_doorbell(HSEM_M4_TO_M7);
    return 0;
}

int ipc_poll_event_reply(uint16_t seq, ipc_msg_t *out) {
    ipc_msg_t m;

    while (ring_pop(&shared_ipc.evt_rsp, &m)) {
        if (m.seq == seq) {
            *out = m;
            return 1;
        }
        stale_event_replies++;
    }
    return 0;
}

/* No sequence filter, so the caller owns the dispatch and nothing is dropped
 * on its behalf. radio_devices_docs/open_hub/arch/ipc.md */
int ipc_poll_any_event_reply(ipc_msg_t *out) {
    return ring_pop(&shared_ipc.evt_rsp, out);
}

int ipc_poll_event(ipc_msg_t *out) {
    if (!ipc_ready())
        return 0;
    return ring_pop(&shared_ipc.evt, out);
}

int ipc_send_event_reply(const ipc_msg_t *evt, uint8_t status, const void *payload, uint8_t len) {
    ipc_msg_t m;

    msg_fill(&m, evt->seq, evt->type, status, payload, len);
    if (!ring_push(&shared_ipc.evt_rsp, &m))
        return 1;

    ipc_ring_doorbell(HSEM_M7_TO_M4_RFM);
    return 0;
}

uint32_t ipc_stale_replies(void) {
    return stale_replies;
}

uint32_t ipc_stale_event_replies(void) {
    return stale_event_replies;
}

void ipc_ring_state(const ipc_ring_t *r, uint32_t *head, uint32_t *tail) {
    *head = r->head;
    *tail = r->tail;
}
