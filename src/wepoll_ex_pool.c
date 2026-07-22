/*
 * wepoll_ex_pool.c — AFD buffer pool + MPSC lock-free ready queue.
 *
 * The AFD buffer pool is a mutex-protected LIFO stack of pre-allocated
 * buffers.  The small critical section avoids the ABA/reclamation
 * hazards of an untagged Treiber stack.
 *
 * The ready queue uses the intrusive single-consumer MPSC algorithm
 * where producers exchange a head pointer and then publish through the
 * predecessor's next link.  The consumer injects a sentinel when it
 * reaches the last item, so a returned node is never still writable by
 * a producer and can be reclaimed immediately.
 *
 * Queue producers remain lock-free.  Draining is O(n), where n is the
 * number of events returned.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* AFD buffer pool — LIFO stack of pre-allocated buffers.              */
/* --------------------------------------------------------------------- */

typedef struct ep_pool_entry {
    struct ep_pool_entry *next;
    struct ep_pool_entry *all_next;
    ep_afd_pool_t        *owner;
    uint64_t              magic;
    int                   in_pool;
    max_align_t           alignment;
    unsigned char         data[];
} ep_pool_entry_t;

#define EP_POOL_ENTRY_MAGIC UINT64_C(0x4550504f4f4c4558)

static void *ep_pool_entry_buf(ep_pool_entry_t *entry)
{
    return entry->data;
}

static ep_pool_entry_t *ep_pool_buf_entry(void *buf)
{
    return (ep_pool_entry_t *)((unsigned char *)buf -
                               offsetof(ep_pool_entry_t, data));
}

static ep_pool_entry_t *ep_pool_entry_alloc(ep_afd_pool_t *p, int in_pool)
{
    const size_t header_size = offsetof(ep_pool_entry_t, data);
    if (p->buf_size > SIZE_MAX - header_size) {
        ep_set_errno(ENOMEM);
        return NULL;
    }

    ep_pool_entry_t *entry =
        (ep_pool_entry_t *)malloc(header_size + p->buf_size);
    if (entry == NULL) {
        ep_set_errno(ENOMEM);
        return NULL;
    }

    entry->next = NULL;
    entry->all_next = NULL;
    entry->owner = p;
    entry->magic = EP_POOL_ENTRY_MAGIC;
    entry->in_pool = in_pool;
    return entry;
}

static void ep_pool_entries_free(ep_pool_entry_t *entry)
{
    while (entry != NULL) {
        ep_pool_entry_t *next = entry->all_next;
        entry->magic = 0;
        entry->owner = NULL;
        free(entry);
        entry = next;
    }
}

int ep_afd_pool_init(ep_afd_pool_t *p, size_t buf_size, size_t capacity)
{
    if (p == NULL || buf_size == 0 || capacity == 0) {
        ep_set_errno(EINVAL);
        return -1;
    }

    memset(p, 0, sizeof(*p));
    int rc = pthread_mutex_init(&p->lock, NULL);
    if (rc != 0) {
        ep_set_errno(rc);
        return -1;
    }

    p->buf_size = buf_size;
    p->capacity = capacity;
    atomic_init(&p->in_use, 0);
    atomic_init(&p->peak, 0);
    p->initialized = 1;

    for (size_t i = 0; i < capacity; i++) {
        ep_pool_entry_t *entry = ep_pool_entry_alloc(p, 1);
        if (entry == NULL) {
            ep_pool_entries_free((ep_pool_entry_t *)p->all_entries);
            p->stack = NULL;
            p->all_entries = NULL;
            p->allocated = 0;
            p->initialized = 0;
            pthread_mutex_destroy(&p->lock);
            return -1;
        }

        entry->next = (ep_pool_entry_t *)p->stack;
        p->stack = entry;
        entry->all_next = (ep_pool_entry_t *)p->all_entries;
        p->all_entries = entry;
        p->allocated++;
    }
    return 0;
}

void ep_afd_pool_destroy(ep_afd_pool_t *p)
{
    if (p == NULL || !p->initialized) return;

    int rc = pthread_mutex_lock(&p->lock);
    if (rc != 0) {
        ep_set_errno(rc);
        return;
    }

    if (atomic_load_explicit(&p->in_use, memory_order_relaxed) != 0) {
        pthread_mutex_unlock(&p->lock);
        ep_set_errno(EBUSY);
        return;
    }

    ep_pool_entry_t *entries = (ep_pool_entry_t *)p->all_entries;
    p->stack = NULL;
    p->all_entries = NULL;
    p->allocated = 0;
    p->initialized = 0;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_destroy(&p->lock);

    ep_pool_entries_free(entries);
}

void *ep_afd_pool_take(ep_afd_pool_t *p)
{
    if (p == NULL || !p->initialized) {
        ep_set_errno(EFAULT);
        return NULL;
    }

    int rc = pthread_mutex_lock(&p->lock);
    if (rc != 0) {
        ep_set_errno(rc);
        return NULL;
    }

    if (!p->initialized) {
        pthread_mutex_unlock(&p->lock);
        ep_set_errno(EFAULT);
        return NULL;
    }

    ep_pool_entry_t *entry = (ep_pool_entry_t *)p->stack;
    if (entry != NULL) {
        p->stack = entry->next;
    } else {
        entry = ep_pool_entry_alloc(p, 0);
        if (entry == NULL) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        entry->all_next = (ep_pool_entry_t *)p->all_entries;
        p->all_entries = entry;
        p->allocated++;
    }

    entry->next = NULL;
    entry->in_pool = 0;

    size_t now = atomic_load_explicit(&p->in_use, memory_order_relaxed) + 1;
    atomic_store_explicit(&p->in_use, now, memory_order_relaxed);
    size_t peak = atomic_load_explicit(&p->peak, memory_order_relaxed);
    if (now > peak) {
        atomic_store_explicit(&p->peak, now, memory_order_relaxed);
    }

    pthread_mutex_unlock(&p->lock);
    return ep_pool_entry_buf(entry);
}

void ep_afd_pool_give(ep_afd_pool_t *p, void *buf)
{
    if (p == NULL || buf == NULL) return;

    ep_pool_entry_t *entry = ep_pool_buf_entry(buf);
    if (entry->magic != EP_POOL_ENTRY_MAGIC || entry->owner != p ||
        !p->initialized) {
        ep_set_errno(EINVAL);
        return;
    }

    int rc = pthread_mutex_lock(&p->lock);
    if (rc != 0) {
        ep_set_errno(rc);
        return;
    }

    if (!p->initialized || entry->in_pool) {
        pthread_mutex_unlock(&p->lock);
        ep_set_errno(EINVAL);
        return;
    }

    size_t in_use = atomic_load_explicit(&p->in_use, memory_order_relaxed);
    entry->in_pool = 1;
    entry->next = (ep_pool_entry_t *)p->stack;
    p->stack = entry;
    if (in_use > 0) {
        atomic_store_explicit(&p->in_use, in_use - 1,
                              memory_order_relaxed);
    } else {
        ep_set_errno(EINVAL);
    }

    pthread_mutex_unlock(&p->lock);
}

/* --------------------------------------------------------------------- */
/* Intrusive MPSC ready queue with a single consumer.                  */
/*                                                                     */
/* Producers exchange `head`, then publish the new node through the   */
/* previous head's `next` field.  When the consumer reaches the last  */
/* visible node it appends the permanent stub.  That hand-off ensures */
/* the returned node is no longer a possible producer predecessor, so */
/* it can be returned to the pool immediately.                         */
/* --------------------------------------------------------------------- */

static void ep_ready_link(ep_ready_queue_t *q, ep_ready_node_t *node,
                          int account)
{
    atomic_store_explicit(&node->next, (ep_ready_node_t *)NULL,
                          memory_order_relaxed);
    if (account) {
        atomic_fetch_add_explicit(&q->queued, 1, memory_order_relaxed);
    }

    ep_ready_node_t *prev =
        atomic_exchange_explicit(&q->head, node, memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
}

static void ep_ready_account_pop(ep_ready_queue_t *q)
{
    size_t queued = atomic_load_explicit(&q->queued, memory_order_relaxed);
    while (queued > 0 &&
           !atomic_compare_exchange_weak_explicit(
               &q->queued, &queued, queued - 1,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static ep_ready_node_t *ep_ready_pop_one(ep_ready_queue_t *q)
{
    ep_ready_node_t *tail = q->tail;
    ep_ready_node_t *next =
        atomic_load_explicit(&tail->next, memory_order_acquire);

    if (tail == q->stub) {
        if (next == NULL) return NULL;
        q->tail = next;
        tail = next;
        next = atomic_load_explicit(&tail->next, memory_order_acquire);
    }

    if (next != NULL) {
        q->tail = next;
        atomic_store_explicit(&tail->next, (ep_ready_node_t *)NULL,
                              memory_order_relaxed);
        ep_ready_account_pop(q);
        return tail;
    }

    ep_ready_node_t *head =
        atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail != head) {
        /* A producer exchanged head but has not linked `tail->next` yet. */
        return NULL;
    }

    ep_ready_link(q, q->stub, 0);
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (next == NULL) return NULL;

    q->tail = next;
    atomic_store_explicit(&tail->next, (ep_ready_node_t *)NULL,
                          memory_order_relaxed);
    ep_ready_account_pop(q);
    return tail;
}

void ep_ready_init(ep_ready_queue_t *q)
{
    if (q == NULL) return;

    memset(q, 0, sizeof(*q));
    q->stub = (ep_ready_node_t *)calloc(1, sizeof(*q->stub));
    if (q->stub == NULL) {
        ep_set_errno(ENOMEM);
        return;
    }

    atomic_init(&q->stub->next, (ep_ready_node_t *)NULL);
    atomic_init(&q->head, q->stub);
    q->tail = q->stub;
    atomic_init(&q->queued, 0);
    q->initialized = 1;
}

void ep_ready_destroy(ep_ready_queue_t *q)
{
    if (q == NULL || !q->initialized) return;

    /* Producers must be quiesced and the caller must reclaim every real
     * node before destroy.  Silently detaching caller-owned nodes here
     * would make it impossible to return them to their backing pool. */
    if (atomic_load_explicit(&q->queued, memory_order_relaxed) != 0) {
        ep_set_errno(EBUSY);
        return;
    }

    q->initialized = 0;
    atomic_store_explicit(&q->head, (ep_ready_node_t *)NULL,
                          memory_order_relaxed);
    q->tail = NULL;
    atomic_store_explicit(&q->queued, 0, memory_order_relaxed);
    free(q->stub);
    q->stub = NULL;
}

void ep_ready_push(ep_ready_queue_t *q, ep_ready_node_t *node)
{
    if (q == NULL || node == NULL || !q->initialized) return;
    ep_ready_link(q, node, 1);
}

ep_ready_node_t *ep_ready_drain(ep_ready_queue_t *q, int maxevents)
{
    if (q == NULL || maxevents <= 0 || !q->initialized) return NULL;

    ep_ready_node_t *out_head = NULL;
    ep_ready_node_t *out_tail = NULL;
    for (int count = 0; count < maxevents; count++) {
        ep_ready_node_t *node = ep_ready_pop_one(q);
        if (node == NULL) break;

        if (out_tail == NULL) {
            out_head = node;
            out_tail = node;
        } else {
            atomic_store_explicit(&out_tail->next, node,
                                  memory_order_relaxed);
            out_tail = node;
        }
    }

    return out_head;
}

/* --------------------------------------------------------------------- */
/* Ready-node pool convenience.                                      */
/* --------------------------------------------------------------------- */

ep_ready_node_t *ep_ready_node_alloc(ep_port_t *port)
{
    if (port == NULL) {
        ep_set_errno(EFAULT);
        return NULL;
    }
    void *buf = ep_afd_pool_take(&port->ready_node_pool);
    if (buf == NULL) return NULL;
    ep_ready_node_t *n = (ep_ready_node_t *)buf;
    atomic_init(&n->next, (ep_ready_node_t *)NULL);
    n->sock      = NULL;
    n->events    = 0;
    n->flags     = 0;
    n->timestamp = 0;
    return n;
}

void ep_ready_node_free(ep_port_t *port, ep_ready_node_t *n)
{
    if (port == NULL || n == NULL) return;
    ep_afd_pool_give(&port->ready_node_pool, n);
}
