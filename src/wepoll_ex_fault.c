/*
 * wepoll_ex_fault.c -- deterministic internal fault injection.
 *
 * Fault points are process-global because they are used only by bounded,
 * single-purpose test executables.  All state remains atomic so a test can
 * safely exercise a configured point from concurrent control paths.
 */
#include "wepoll_ex_internal.h"

#include <stdint.h>

#ifdef WEPOLL_EX_ENABLE_FAULT_INJECTION

typedef struct ep_fault_slot {
    _Atomic uint64_t fail_at;
    _Atomic uint64_t hits;
    _Atomic int error;
} ep_fault_slot_t;

static _Atomic uint64_t g_fault_mask;
static ep_fault_slot_t g_fault_slots[EP_FAULT_POINT_COUNT];

_Static_assert(EP_FAULT_POINT_COUNT <= 64,
               "fault point mask is limited to 64 entries");

static int ep_fault_point_valid(ep_fault_point_t point)
{
    return point >= 0 && point < EP_FAULT_POINT_COUNT;
}

int ep_fault_configure(ep_fault_point_t point, uint64_t fail_at, int error)
{
    uint64_t bit;
    ep_fault_slot_t *slot;

    if (!ep_fault_point_valid(point) || fail_at == 0 || error <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }

    bit = UINT64_C(1) << (unsigned int)point;
    slot = &g_fault_slots[point];

    /* Configuration is changed only while the test harness is quiescent.
     * Clear the bit first so a new hit cannot enter while the slot is being
     * replaced; concurrent operation after publication remains lock-free. */
    atomic_fetch_and_explicit(&g_fault_mask, ~bit, memory_order_acq_rel);
    atomic_store_explicit(&slot->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->error, error, memory_order_relaxed);
    atomic_store_explicit(&slot->fail_at, fail_at, memory_order_relaxed);
    atomic_fetch_or_explicit(&g_fault_mask, bit, memory_order_release);
    return 0;
}

void ep_fault_reset(void)
{
    atomic_store_explicit(&g_fault_mask, 0, memory_order_release);
    for (int i = 0; i < EP_FAULT_POINT_COUNT; i++) {
        atomic_store_explicit(&g_fault_slots[i].fail_at, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&g_fault_slots[i].hits, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&g_fault_slots[i].error, 0,
                              memory_order_relaxed);
    }
}

int ep_fault_hit(ep_fault_point_t point)
{
    uint64_t bit;
    uint64_t ordinal;
    uint64_t fail_at;
    int error;
    ep_fault_slot_t *slot;

    if (!ep_fault_point_valid(point))
        return 0;

    bit = UINT64_C(1) << (unsigned int)point;
    if ((atomic_load_explicit(&g_fault_mask, memory_order_acquire) & bit) == 0)
        return 0;

    slot = &g_fault_slots[point];
    ordinal = atomic_fetch_add_explicit(&slot->hits, 1,
                                        memory_order_relaxed) + 1;
    fail_at = atomic_load_explicit(&slot->fail_at, memory_order_relaxed);
    if (ordinal != fail_at)
        return 0;

    error = atomic_load_explicit(&slot->error, memory_order_relaxed);
    ep_set_errno(error);
    return -1;
}

uint64_t ep_fault_hits(ep_fault_point_t point)
{
    if (!ep_fault_point_valid(point)) {
        ep_set_errno(EINVAL);
        return 0;
    }
    return atomic_load_explicit(&g_fault_slots[point].hits,
                                memory_order_relaxed);
}

#else

/* Avoid a pedantic empty-translation-unit diagnostic in ordinary builds. */
typedef int ep_fault_injection_disabled_translation_unit_t;

#endif
