/* A private port implementation exposes the admission boundary and counts
 * its allocations without adding test hooks to the installed library. */
#include "wepoll_ex_internal.h"

#include <stdio.h>
#include <stdlib.h>

static int track_context;
static void *context_allocation;
static unsigned context_allocations;
static unsigned context_frees;

static void *test_calloc(size_t count, size_t size)
{
    void *allocation = calloc(count, size);

    if (track_context && allocation != NULL) {
        context_allocation = allocation;
        context_allocations++;
    }
    return allocation;
}

static void test_free(void *allocation)
{
    if (track_context && allocation != NULL &&
        allocation == context_allocation) {
        context_frees++;
    }
    free(allocation);
}

#define calloc test_calloc
#define free test_free
#include "../src/wepoll_ex_port.c"
#undef free
#undef calloc

int main(void)
{
    ep_port_t *port = NULL;
    int result;
    int failed;

    if (ep_port_create(1, 0, &port) != 0) return 2;
    atomic_store(&g_active_quarantines, EP_MAX_ACTIVE_QUARANTINES);
    pthread_mutex_lock(&port->wait_lock);
    track_context = 1;
    result = ep_port_quarantine_recoverable_locked(port, ETIMEDOUT);
    track_context = 0;
    failed = result != -1 || errno != ETIMEDOUT ||
        context_allocations != 1 || context_frees != 1 ||
        atomic_load(&g_active_quarantines) != EP_MAX_ACTIVE_QUARANTINES ||
        atomic_load(&g_quarantined_ports) != 1 ||
        atomic_load(&g_irrecoverable_ports) != 1;
    if (failed) {
        fprintf(stderr, "reaper cap: result=%d allocations=%u frees=%u\n",
                result, context_allocations, context_frees);
    }
    if (context_frees == 0) free(context_allocation);
    atomic_store(&g_active_quarantines, 0);
    /* This synthetic port has no kernel-owned requests, so the test can
     * reclaim the storage the production abandonment path must retain. */
    pthread_mutex_lock(&port->wait_lock);
    ep_port_finish_destroy_locked(port);
    return failed ? 1 : 0;
}
