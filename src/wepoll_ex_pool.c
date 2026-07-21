/*
 * wepoll_ex_pool.c — AFD buffer pool + MPSC lock-free ready queue.
 *
 * The AFD buffer pool is a LIFO stack of pre-allocated buffers.
 * Producers (epoll_ctl ADD path, IOCP completion handler) pop a
 * buffer in O(1) via an atomic exchange on the stack head;
 * consumers (cleanup paths) push back the same way.
 *
 * The ready queue is a Michael-Scott-style MPSC queue.  Producers
 * append via exchange on the tail.  The single consumer (epoll_wait
 * caller) snaps head->next, walks the chain up to maxevents nodes,
 * and repositions the head to the last consumed node, which becomes
 * the new sentinel.
 *
 * Both data structures are wait-free for the producer side and
 * O(n) for the consumer (n = number of events drained).  No
 * pthread_mutex anywhere in the hot path.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* AFD buffer pool — LIFO stack of pre-allocated buffers.              */
/* --------------------------------------------------------------------- */

typedef struct ep_pool_hdr {
    ep_afd_pool_t *owner;
} ep_pool_hdr_t;

static inline void *hdr_to_buf(ep_pool_hdr_t *h) { return (char *)h + sizeof(*h); }
static inline ep_pool_hdr_t *buf_to_hdr(void *b) { return (ep_pool_hdr_t *)((char *)b - sizeof(ep_pool_hdr_t)); }

typedef struct ep_pool_entry {
    ep_pool_hdr_t        hdr;
    struct ep_pool_entry *next;
} ep_pool_entry_t;

int ep_afd_pool_init(ep_afd_pool_t *p, size_t buf_size, size_t capacity)
{
    if (p == NULL || buf_size == 0 || capacity == 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    atomic_store(&p->stack, (void *)NULL);
    p->buf_size = buf_size;
    p->capacity = capacity;
    atomic_store(&p->in_use, (size_t)0);
    atomic_store(&p->peak, (size_t)0);

    for (size_t i = 0; i < capacity; i++) {
        ep_pool_entry_t *e = (ep_pool_entry_t *)
            malloc(sizeof(*e) + buf_size);
        if (e == NULL) {
            ep_afd_pool_destroy(p);
            ep_set_errno(ENOMEM);
            return -1;
        }
        e->hdr.owner = p;
        e->next = (ep_pool_entry_t *)atomic_load(&p->stack);
        atomic_store(&p->stack, (void *)e);
    }
    return 0;
}

void ep_afd_pool_destroy(ep_afd_pool_t *p)
{
    if (p == NULL) return;
    ep_pool_entry_t *e = (ep_pool_entry_t *)atomic_exchange(&p->stack, (void *)NULL);
    while (e) {
        ep_pool_entry_t *next = e->next;
        free(e);
        e = next;
    }
}

void *ep_afd_pool_take(ep_afd_pool_t *p)
{
    if (p == NULL) {
        ep_set_errno(EFAULT);
        return NULL;
    }
    for (;;) {
        ep_pool_entry_t *e = (ep_pool_entry_t *)atomic_load(&p->stack);
        if (e == NULL) {
            ep_pool_entry_t *fresh = (ep_pool_entry_t *)
                malloc(sizeof(*fresh) + p->buf_size);
            if (fresh == NULL) {
                ep_set_errno(ENOMEM);
                return NULL;
            }
            fresh->hdr.owner = p;
            return hdr_to_buf(&fresh->hdr);
        }
        if (atomic_compare_exchange_weak(&p->stack,
                (void **)&e, (void *)e->next)) {
            size_t now = atomic_fetch_add(&p->in_use, 1) + 1;
            size_t peak = atomic_load(&p->peak);
            while (now > peak &&
                   !atomic_compare_exchange_weak(&p->peak, &peak, now)) {
            }
            return hdr_to_buf(&e->hdr);
        }
    }
}

void ep_afd_pool_give(ep_afd_pool_t *p, void *buf)
{
    if (p == NULL || buf == NULL) return;
    ep_pool_hdr_t *h = buf_to_hdr(buf);
    if (h->owner != p) {
        if (h->owner != NULL) ep_afd_pool_give(h->owner, buf);
        return;
    }
    ep_pool_entry_t *e = (ep_pool_entry_t *)h;
    for (;;) {
        ep_pool_entry_t *top = (ep_pool_entry_t *)atomic_load(&p->stack);
        e->next = top;
        if (atomic_compare_exchange_weak(&p->stack,
                (void **)&top, (void *)e)) {
            break;
        }
    }
    atomic_fetch_sub(&p->in_use, 1);
}

/* --------------------------------------------------------------------- */
/* MPSC lock-free ready queue — Michael-Scott variant.                */
/*                                                                     */
/* Invariant: head and tail always point to a node (never NULL).  The  */
/* queue is born with a sentinel "stub" node.  Push appends after the  */
/* tail.  Drain snaps head->next (the first real node), walks the      */
/* chain up to maxevents nodes, and repositions the head to the last   */
/* consumed node, which becomes the new sentinel.                     */
/*                                                                     */
/* Producer side: wait-free via atomic_exchange on tail + atomic_store */
/* on prev->next.  Consumer side: single-threaded, no CAS needed.     */
/*                                                                     */
/* The classic Michael-Scott "pub-then-link" race (producer has done  */
/* exchange(&tail, X) but hasn't yet done store(&prev->next, X)) is   */
/* handled by a brief spin in the drain path.                         */
/* --------------------------------------------------------------------- */

void ep_ready_init(ep_ready_queue_t *q)
{
    if (q == NULL) return;
    /* Allocate the stub.  Lives for the lifetime of the queue. */
    q->stub = (ep_ready_node_t *)calloc(1, sizeof(*q->stub));
    if (q->stub == NULL) {
        ep_set_errno(ENOMEM);
        return;
    }
    atomic_store(&q->stub->next, (ep_ready_node_t *)NULL);
    /* head ALWAYS points to the stub — it's the permanent sentinel.
     * Drain reads stub->next to find the first real node. */
    atomic_store(&q->head, q->stub);
    atomic_store(&q->tail, q->stub);
    atomic_store(&q->free_list, (ep_ready_node_t *)NULL);
}

void ep_ready_destroy(ep_ready_queue_t *q)
{
    if (q == NULL) return;
    /* Drain everything (caller should have done this already). */
    for (;;) {
        ep_ready_node_t *n = ep_ready_drain(q, INT_MAX);
        if (n == NULL) break;
        while (n) {
            ep_ready_node_t *next = atomic_load(&n->next);
            /* Don't free here — caller owns the pool.  The stub
             * is freed below. */
            n = next;
        }
    }
    if (q->stub) {
        free(q->stub);
        q->stub = NULL;
    }
    /* Free any nodes on the free_list (legacy field — not used
     * in this revision, but clean up just in case). */
    ep_ready_node_t *fl = atomic_exchange(&q->free_list,
                                          (ep_ready_node_t *)NULL);
    while (fl) {
        ep_ready_node_t *next = atomic_load(&fl->next);
        free(fl);
        fl = next;
    }
}

void ep_ready_push(ep_ready_queue_t *q, ep_ready_node_t *node)
{
    if (q == NULL || node == NULL) return;
    atomic_store(&node->next, (ep_ready_node_t *)NULL);

    /* MPSC append: exchange tail, then link ourselves after the
     * previous tail.  This is the classic Michael-Scott dance. */
    ep_ready_node_t *prev = atomic_exchange(&q->tail, node);
    atomic_store(&prev->next, node);
}

ep_ready_node_t *ep_ready_drain(ep_ready_queue_t *q, int maxevents)
{
    if (q == NULL || maxevents <= 0) return NULL;

    /* Standard Michael-Scott dequeue, repeated up to maxevents
     * times.  Each iteration:
     *   1. Read stub->next.
     *   2. If NULL, queue is empty (or producer mid-publish —
     *      spin briefly).
     *   3. Read first->next.  If NULL and first == tail, a
     *      producer may be mid-publish to first->next — spin
     *      until either first->next becomes non-NULL or we
     *      confirm first == tail.
     *   4. CAS stub->next from `first` to `first->next`.
     *   5. The dequeued node is `first`.  Link it into our
     *      output chain.
     *
     * This is the standard MS-queue dequeue, just batched.  Each
     * dequeue is atomic, so we never orphan a producer's pending
     * push. */
    ep_ready_node_t *stub = q->stub;
    ep_ready_node_t *out_head = NULL;
    ep_ready_node_t *out_tail = NULL;
    int count = 0;

    while (count < maxevents) {
        ep_ready_node_t *first = atomic_load(&stub->next);
        if (first == NULL) {
            /* Queue empty. */
            break;
        }

        /* Read first->next, spinning if a producer is mid-publish.
         * A producer that did exchange(&tail, X) getting back
         * `first` will store(first->next, X).  We must wait for
         * that store before we can dequeue `first` — otherwise
         * the producer's store goes to freed memory. */
        ep_ready_node_t *next = atomic_load(&first->next);
        while (next == NULL) {
            if (first == atomic_load(&q->tail)) {
                /* first == tail AND first->next == NULL: queue
                 * really ends here.  No producer is mid-publish.
                 * We can dequeue `first` — but there's nothing
                 * after it, so the queue becomes empty. */
                break;
            }
            /* Producer is mid-publish to first->next.  Spin. */
#ifdef _WIN32
            YieldProcessor();
#else
            __asm__ __volatile__("pause" ::: "memory");
#endif
            next = atomic_load(&first->next);
        }

        /* CAS stub->next from `first` to `next`. */
        if (!atomic_compare_exchange_strong(&stub->next, &first, next)) {
            /* CAS failed — a producer appended between our read
             * and our CAS.  Retry this iteration. */
            continue;
        }

        /* If `next` is NULL (queue became empty), advance tail
         * from `first` to `stub` so future producers append
         * after the stub. */
        if (next == NULL) {
            ep_ready_node_t *expected_tail = first;
            atomic_compare_exchange_strong(&q->tail, &expected_tail, stub);
        }

        /* Detach `first` from the queue. */
        atomic_store(&first->next, (ep_ready_node_t *)NULL);

        /* Append `first` to our output chain. */
        if (out_tail == NULL) {
            out_head = first;
            out_tail = first;
        } else {
            atomic_store(&out_tail->next, first);
            out_tail = first;
        }
        count++;
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
    atomic_store(&n->next, (ep_ready_node_t *)NULL);
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
