/*
 * pogls_ghost_async.h — POGLS V4  Ghost Async Path
 * ══════════════════════════════════════════════════════════════════════
 *
 * Separates GHOST ops from the critical MAIN write path.
 *
 * Problem (from benchmark D):
 *   Inline ghost store = every GHOST op blocks critical path.
 *   ~32% of ops land GHOST on mixed workload → real throughput loss.
 *
 * Solution: lock-free single-producer / single-consumer ring buffer.
 *   MAIN path:   push ghost entry into ring   (~3 ns, no branch)
 *   Background:  drain ring → write ghost lanes asynchronously
 *
 * Architecture:
 *
 *   pipeline_wire_process()          Ghost Drain Thread
 *         │                               │
 *   [GHOST decision]                 ghost_async_drain()
 *         │                               │
 *   ghost_async_push(ring, entry)    pulls from ring
 *         │ (lock-free SPSC)             │
 *         └──────[ring buffer]───────────┘
 *                                        │
 *                                  wd_push(lane) + WireBlock
 *
 * Ring layout:
 *   GHOST_RING_SIZE   = 8192 entries (power of 2)
 *   GhostAsyncEntry   = 32B (value + addr + sig + lane + pad)
 *   Ring overhead     = 8192 × 32 = 256 KB (fits L3 cache)
 *
 * Drain modes:
 *   DRAIN_IMMEDIATE   — foreground, no thread (safe fallback)
 *   DRAIN_THREADED    — background pthread, called from init
 *
 * Integration:
 *   1. Add GhostAsyncRing ring to PipelineWire (or standalone)
 *   2. Replace inline ghost wd_push() with ghost_async_push()
 *   3. Call ghost_async_start() after pipeline_wire_init()
 *   4. Call ghost_async_stop() before pipeline_wire_close()
 *
 * Design rules (FROZEN):
 *   - Never touch original files; ring is drop-in additive
 *   - MAIN path: zero mutex, zero syscall
 *   - Ghost drain: bounded latency — flushes every DRAIN_INTERVAL_MS
 *   - On overflow: push returns 0, caller falls back to inline write
 *   - Ring head/tail = uint32_t (wrap via & mask — never compare raw)
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_GHOST_ASYNC_H
#define POGLS_GHOST_ASYNC_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

/* ── ring dimensions ─────────────────────────────────────────────── */
#define GHOST_RING_SIZE      8192u     /* power of 2                  */
#define GHOST_RING_MASK      (GHOST_RING_SIZE - 1u)
#define GHOST_RING_HIGHWATER (GHOST_RING_SIZE * 3u / 4u)  /* 75% full */

/* ── drain timing ────────────────────────────────────────────────── */
#define GHOST_DRAIN_INTERVAL_US  500u  /* drain every 500 µs          */
#define GHOST_DRAIN_BATCH        256u  /* max entries per drain pass   */

/* ══════════════════════════════════════════════════════════════════
 * GhostAsyncEntry — 32B ring slot
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t  value;         /* data value                            */
    uint64_t  angular_addr;  /* angular address (lane routing input)  */
    uint64_t  sig;           /* ghost cache signature                 */
    uint8_t   lane;          /* target ghost lane (pre-computed)      */
    uint8_t   _pad[7];       /* align to 32B                          */
} GhostAsyncEntry;           /* 32B total                             */

/* compile-time size check */
typedef char _ghost_entry_size_check[
    (sizeof(GhostAsyncEntry) == 32u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * GhostAsyncRing — SPSC lock-free ring
 * ══════════════════════════════════════════════════════════════════
 *
 * Cache-line aligned: producer owns head, consumer owns tail.
 * Separated into different cache lines to prevent false sharing.
 */
typedef struct {
    /* producer side — cache line 0 */
    volatile uint32_t  head __attribute__((aligned(64)));
    uint32_t           _hp[15];   /* pad to 64B                       */

    /* consumer side — cache line 1 */
    volatile uint32_t  tail __attribute__((aligned(64)));
    uint32_t           _tp[15];   /* pad to 64B                       */

    /* ring data — separate allocation avoids false sharing with ctrl  */
    GhostAsyncEntry    ring[GHOST_RING_SIZE];

    /* stats (written by consumer only) */
    uint64_t  pushed;
    uint64_t  drained;
    uint64_t  overflows;   /* push() returned 0 → caller did inline   */
    uint64_t  drain_passes;

    /* thread control */
    volatile int  running;
    pthread_t     drain_thread;
    int           thread_started;

    /* drain callback — called with each entry batch */
    void (*on_drain)(const GhostAsyncEntry *entries, uint32_t count, void *ctx);
    void *drain_ctx;

    uint32_t  magic;
} GhostAsyncRing;

#define GHOST_RING_MAGIC  0x47415352u   /* "GASR" */

/* ══════════════════════════════════════════════════════════════════
 * push — MAIN path (producer, lock-free)
 *
 * Returns 1 on success, 0 on overflow (caller falls back to inline).
 * Called from pipeline_wire_process() on ROUTE_GHOST decision.
 * ══════════════════════════════════════════════════════════════════ */
static inline int ghost_async_push(GhostAsyncRing *r,
                                   uint64_t value,
                                   uint64_t angular_addr,
                                   uint64_t sig,
                                   uint8_t  lane)
{
    if (!r) return 0;
    uint32_t h = r->head;
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

    /* full check: producer cannot advance head past tail + SIZE */
    if ((uint32_t)(h - t) >= GHOST_RING_SIZE) {
        __atomic_fetch_add(&r->overflows, 1, __ATOMIC_RELAXED);
        return 0;   /* overflow → caller falls back to inline write    */
    }

    GhostAsyncEntry *e = &r->ring[h & GHOST_RING_MASK];
    e->value        = value;
    e->angular_addr = angular_addr;
    e->sig          = sig;
    e->lane         = lane;

    /* release: entry must be fully written before head advances */
    __atomic_store_n(&r->head, h + 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&r->pushed, 1, __ATOMIC_RELAXED);
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * drain pass — consumer side (background thread or foreground)
 *
 * Returns number of entries processed.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t ghost_async_drain_pass(GhostAsyncRing *r)
{
    uint32_t t = r->tail;
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

    uint32_t avail = h - t;
    if (avail == 0) return 0;

    /* cap to batch size */
    if (avail > GHOST_DRAIN_BATCH) avail = GHOST_DRAIN_BATCH;

    /* collect batch */
    GhostAsyncEntry batch[GHOST_DRAIN_BATCH];
    for (uint32_t i = 0; i < avail; i++) {
        batch[i] = r->ring[(t + i) & GHOST_RING_MASK];
    }

    /* advance tail before callback (pipeline can keep pushing) */
    __atomic_store_n(&r->tail, t + avail, __ATOMIC_RELEASE);
    __atomic_fetch_add(&r->drained, avail, __ATOMIC_RELAXED);
    r->drain_passes++;

    /* call drain handler */
    if (r->on_drain) {
        r->on_drain(batch, avail, r->drain_ctx);
    }

    return avail;
}

/* ── drain thread body ───────────────────────────────────────────── */
static void *_ghost_drain_thread(void *arg)
{
    GhostAsyncRing *r = (GhostAsyncRing *)arg;
    struct timespec ts = { 0, GHOST_DRAIN_INTERVAL_US * 1000L };

    while (__atomic_load_n(&r->running, __ATOMIC_ACQUIRE)) {
        ghost_async_drain_pass(r);
        nanosleep(&ts, NULL);
    }
    /* final drain on shutdown */
    ghost_async_drain_pass(r);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 * init / start / stop / destroy
 * ══════════════════════════════════════════════════════════════════ */

/*
 * ghost_async_init — zero-init ring and set drain callback.
 *
 * on_drain(entries, count, ctx):
 *   Called from drain thread with a batch of ghost entries.
 *   Caller should write to WireDelta lanes here.
 *   May be NULL (entries are discarded — useful for testing).
 */
static inline int ghost_async_init(GhostAsyncRing *r,
                                   void (*on_drain)(const GhostAsyncEntry*,
                                                    uint32_t, void*),
                                   void *ctx)
{
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->on_drain       = on_drain;
    r->drain_ctx      = ctx;
    r->running        = 0;
    r->thread_started = 0;
    r->magic          = GHOST_RING_MAGIC;
    return 0;
}

/* ghost_async_start — launch background drain thread */
static inline int ghost_async_start(GhostAsyncRing *r)
{
    if (!r || r->thread_started) return -1;
    __atomic_store_n(&r->running, 1, __ATOMIC_RELEASE);
    int rc = pthread_create(&r->drain_thread, NULL, _ghost_drain_thread, r);
    if (rc == 0) r->thread_started = 1;
    return rc;
}

/* ghost_async_stop — signal drain thread and join */
static inline int ghost_async_stop(GhostAsyncRing *r)
{
    if (!r || !r->thread_started) return -1;
    __atomic_store_n(&r->running, 0, __ATOMIC_RELEASE);
    pthread_join(r->drain_thread, NULL);
    r->thread_started = 0;
    return 0;
}

/*
 * ghost_async_drain_foreground — no thread, call manually.
 * Safe to call from main thread when DRAIN_IMMEDIATE mode is wanted.
 */
static inline uint32_t ghost_async_drain_foreground(GhostAsyncRing *r)
{
    if (!r) return 0;
    uint32_t total = 0;
    uint32_t n;
    do {
        n = ghost_async_drain_pass(r);
        total += n;
    } while (n == GHOST_DRAIN_BATCH);  /* keep draining if full batch */
    return total;
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void ghost_async_stats(const GhostAsyncRing *r)
{
    if (!r) return;
    uint64_t total = r->pushed + r->overflows;
    if (total == 0) total = 1;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Ghost Async Ring Stats                         ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Pushed:       %10llu                        ║\n",
           (unsigned long long)r->pushed);
    printf("║ Drained:      %10llu                        ║\n",
           (unsigned long long)r->drained);
    printf("║ Overflows:    %10llu (%3llu%% fallback inline) ║\n",
           (unsigned long long)r->overflows,
           (unsigned long long)(r->overflows * 100u / total));
    printf("║ Drain passes: %10llu                        ║\n",
           (unsigned long long)r->drain_passes);
    printf("║ Pending:      %10u                        ║\n",
           (uint32_t)(r->head - r->tail));
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

/* ── ring occupancy (0–100) ──────────────────────────────────────── */
static inline uint32_t ghost_async_occupancy_pct(const GhostAsyncRing *r)
{
    if (!r) return 0;
    uint32_t used = r->head - r->tail;
    return (used * 100u) / GHOST_RING_SIZE;
}

/* ── overflow check — has ring ever overflowed? ──────────────────── */
static inline int ghost_async_had_overflow(const GhostAsyncRing *r)
{
    return r && r->overflows > 0;
}

#endif /* POGLS_GHOST_ASYNC_H */
