/*
 * pogls_infinity_castle.h — Self-Optimizing Engine (SOE)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Infinity Castle Architecture:
 *   Nakime (Revolver) + Rui (Trace) + Collapse + Predict + Ghost
 *
 * NO REPEAT WORK: READ → REMEMBER → PREDICT → SKIP
 *
 * Hardware-Aware:
 *   L1: < 32KB (hot path only)
 *   Cache-line aligned (16B/32B/64B)
 *   Branchless fast path
 *   No malloc in hot path
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_INFINITY_CASTLE_H
#define POGLS_INFINITY_CASTLE_H

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

/* ══════════════════════════════════════════════════════════════════
 * Global Config (tuneable)
 * ══════════════════════════════════════════════════════════════════ */
#define TRACE_ENABLE        1
#define COLLAPSE_ENABLE     1
#define PREDICT_ENABLE      1
#define GHOST_ENABLE        1

#define TRACE_TTL           4096
#define TRACE_SIZE          256        /* L1 fit: 256 × 16B = 4KB */

#define COLLAPSE_WINDOW     8
#define COLLAPSE_THRESHOLD  6
#define COLLAPSE_SIZE       (1<<15)    /* 32K entries = 1MB L2 */
#define COLLAPSE_TTL        8192

#define PREDICT_CONF_MIN    50         /* % confidence (tuned lower) */
#define ENERGY_LIMIT        70         /* % entropy */

#define DRIFT_GUARD_ENABLE  1

/* Compiler hints */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ══════════════════════════════════════════════════════════════════
 * Data Structures (cache-aligned)
 * ══════════════════════════════════════════════════════════════════ */

/* Trace Entry (16B — 4 per cache line) */
typedef struct __attribute__((packed, aligned(16))) {
    uint64_t offset;      /* spatial + temporal address */
    uint32_t xor_sig;     /* lightweight integrity */
    uint16_t lane;        /* routing hint */
    uint8_t  priority;    /* 0=normal, 1=high */
    uint16_t age;         /* decay counter — range 0..TTL (>255, needs uint16) */
} TraceEntry;

/* Collapse Entry (32B — 2 per cache line) */
typedef struct __attribute__((aligned(32))) {
    uint64_t signature;   /* pattern hash */
    uint32_t hit_count;   /* reuse counter */
    uint16_t lane_mask;   /* active lanes */
    uint8_t  confidence;  /* prediction strength */
    uint16_t age;         /* decay counter — range 0..TTL (>255, needs uint16) */
    uint8_t  pad[12];     /* align to 32B */
} CollapseEntry;

/* Predict Entry (16B) */
typedef struct __attribute__((aligned(16))) {
    uint64_t signature;
    uint64_t next_offset;
    uint8_t  confidence;
    uint8_t  pad[7];
} PredictEntry;

/* Ghost Entry (16B — cached output) */
typedef struct __attribute__((aligned(16))) {
    uint64_t signature;
    uint64_t output_ptr;  /* pointer to cached result */
} GhostEntry;

/* ══════════════════════════════════════════════════════════════════
 * L1 Pack (< 32KB budget!)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* Hot path only */
    TraceEntry   trace[TRACE_SIZE];      /* 4KB */
    uint64_t     window[COLLAPSE_WINDOW]; /* 64B */
    uint16_t     trace_head;
    uint16_t     window_head;
    
    /* Stats (inline for cache) */
    uint64_t     trace_hits;
    uint64_t     collapse_hits;
    uint64_t     ghost_hits;
    uint64_t     cycles_total;
    uint64_t     cycles_fastpath;
} __attribute__((aligned(64))) L1Pack;

/* ══════════════════════════════════════════════════════════════════
 * L2/L3/RAM Tables (cold data)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    CollapseEntry table[COLLAPSE_SIZE];   /* L2: 1MB */
} CollapseTable;

typedef struct {
    PredictEntry  cache[4096];            /* L3: 64KB */
} PredictCache;

typedef struct {
    GhostEntry   cache[8192];             /* RAM: 128KB */
} GhostCache;

/* ══════════════════════════════════════════════════════════════════
 * Phase Control (CRITICAL for drift prevention)
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    PHASE_PREWRITE,
    PHASE_COMMIT
} Phase;

typedef struct {
    L1Pack         l1;
    CollapseTable  collapse;
    PredictCache   predict;
    GhostCache     ghost;
    
    Phase          phase;
    uint8_t        modules_enabled;
    uint32_t       entropy;
} InfinityCastle;

/* ══════════════════════════════════════════════════════════════════
 * Signature Engine (branchless, inline)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t build_signature(const uint64_t *window)
{
    uint64_t h = 1469598103934665603ULL;  /* FNV offset */
    
    #pragma unroll
    for (int i = 0; i < COLLAPSE_WINDOW; i++) {
        h ^= window[i];
        h *= 1099511628211ULL;  /* FNV prime */
    }
    
    return h;
}

static inline uint32_t xor_checksum(const void *data, size_t len)
{
    const uint64_t *p = (const uint64_t *)data;
    uint64_t xor = 0;
    
    for (size_t i = 0; i < len / 8; i++) {
        xor ^= p[i];
    }
    
    return (uint32_t)(xor ^ (xor >> 32));
}

/* ══════════════════════════════════════════════════════════════════
 * Module Implementations
 * ══════════════════════════════════════════════════════════════════ */

/* ── TRACE: Record read ───────────────────────────────────────────── */
static inline void trace_record(InfinityCastle *ic,
                                  uint64_t offset,
                                  const void *data,
                                  size_t len)
{
    if (!TRACE_ENABLE || ic->phase == PHASE_COMMIT) return;
    
    L1Pack *l1 = &ic->l1;
    uint16_t idx = l1->trace_head % TRACE_SIZE;
    
    TraceEntry *entry = &l1->trace[idx];
    entry->offset = offset;
    entry->xor_sig = xor_checksum(data, len);
    entry->lane = offset % 54;
    entry->priority = 0;  /* TODO: classify */
    entry->age = 0;
    
    l1->trace_head++;
}

/* ── TRACE: Lookup ────────────────────────────────────────────────── */
static inline TraceEntry *trace_lookup(InfinityCastle *ic, uint64_t offset)
{
    L1Pack *l1 = &ic->l1;
    
    /* Linear scan (vectorizable — 4 entries per cache line) */
    for (uint16_t i = 0; i < TRACE_SIZE; i++) {
        TraceEntry *e = &l1->trace[i];
        if (e->offset == offset && e->age < TRACE_TTL) {
            l1->trace_hits++;
            return e;
        }
    }
    
    return NULL;
}

/* ── COLLAPSE: Update window ──────────────────────────────────────── */
static inline void collapse_update_window(InfinityCastle *ic, uint64_t offset)
{
    L1Pack *l1 = &ic->l1;
    uint16_t idx = l1->window_head % COLLAPSE_WINDOW;
    l1->window[idx] = offset;
    l1->window_head++;
}

/* ── COLLAPSE: Lookup (flat table, no chaining) ───────────────────── */
static inline CollapseEntry *collapse_lookup(InfinityCastle *ic, uint64_t sig)
{
    if (!COLLAPSE_ENABLE) return NULL;
    
    uint32_t idx = sig & (COLLAPSE_SIZE - 1);
    CollapseEntry *entry = &ic->collapse.table[idx];
    
    /* Simple replacement (no chaining for speed) */
    if (entry->signature != sig) {
        if (entry->age > COLLAPSE_TTL) {
            /* Replace old entry */
            entry->signature = sig;
            entry->hit_count = 1;
            entry->confidence = 0;
            entry->age = 0;
        }
        return NULL;
    }
    
    /* Hit! */
    entry->hit_count++;
    if (entry->hit_count >= COLLAPSE_THRESHOLD) {
        entry->confidence++;
    }
    
    ic->l1.collapse_hits++;
    return entry;
}

/* ── GHOST: Lookup cached output ──────────────────────────────────── */
static inline void *ghost_lookup(InfinityCastle *ic, uint64_t sig)
{
    if (!GHOST_ENABLE) return NULL;
    
    uint32_t idx = sig & (8192 - 1);
    GhostEntry *entry = &ic->ghost.cache[idx];
    
    if (entry->signature == sig) {
        ic->l1.ghost_hits++;
        return (void *)entry->output_ptr;
    }
    
    return NULL;
}

/* ── PREDICT: Prefetch next ───────────────────────────────────────── */
static inline void predict_prefetch(InfinityCastle *ic, uint64_t sig)
{
    if (!PREDICT_ENABLE) return;
    
    uint32_t idx = sig & (4096 - 1);
    PredictEntry *entry = &ic->predict.cache[idx];
    
    if (entry->signature == sig && entry->confidence >= PREDICT_CONF_MIN) {
        /* Prefetch next offset */
        __builtin_prefetch((void *)entry->next_offset, 0, 3);
    }
}

/* ── DRIFT GUARD: Verify integrity ────────────────────────────────── */
static inline int drift_guard_verify(InfinityCastle *ic,
                                       uint64_t offset,
                                       const void *data,
                                       size_t len)
{
    if (!DRIFT_GUARD_ENABLE) return 1;
    
    TraceEntry *trace = trace_lookup(ic, offset);
    if (!trace) return 1;  /* No trace = OK */
    
    uint32_t current_sig = xor_checksum(data, len);
    
    if (current_sig != trace->xor_sig) {
        /* DRIFT DETECTED! */
        ic->modules_enabled = 0;  /* Disable all optimizations */
        return 0;
    }
    
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * Main Processing Path
 * ══════════════════════════════════════════════════════════════════ */
static inline void *infinity_castle_process(InfinityCastle *ic,
                                              uint64_t offset,
                                              void *input_data,
                                              size_t len,
                                              void *(*compute_fn)(void *))
{
    uint64_t start = __builtin_ia32_rdtsc();  /* Cycle counter */
    
    /* Update collapse window */
    collapse_update_window(ic, offset);
    
    /* Build signature */
    uint64_t sig = build_signature(ic->l1.window);
    
    /* ── FAST PATH 1: Ghost hit (bypass everything!) ────────────── */
    if (likely(GHOST_ENABLE && ic->modules_enabled)) {
        void *cached = ghost_lookup(ic, sig);
        if (cached) {
            ic->l1.cycles_fastpath += __builtin_ia32_rdtsc() - start;
            return cached;  /* <40ns path! */
        }
    }
    
    /* ── FAST PATH 2: Predict + prefetch ────────────────────────── */
    if (likely(PREDICT_ENABLE && ic->modules_enabled)) {
        predict_prefetch(ic, sig);
    }
    
    /* ── FAST PATH 3: Collapse hit ──────────────────────────────── */
    if (likely(COLLAPSE_ENABLE && ic->modules_enabled)) {
        CollapseEntry *collapse = collapse_lookup(ic, sig);
        if (collapse && collapse->confidence >= PREDICT_CONF_MIN) {
            /* High confidence → use ghost cache */
            void *cached = ghost_lookup(ic, sig);
            if (cached) {
                ic->l1.cycles_fastpath += __builtin_ia32_rdtsc() - start;
                return cached;  /* <80ns path! */
            }
        }
    }
    
    /* ── SLOW PATH: Compute ─────────────────────────────────────── */
    void *result = compute_fn(input_data);
    
    /* ── Record for future optimization ─────────────────────────── */
    if (ic->phase == PHASE_PREWRITE) {
        trace_record(ic, offset, result, len);
        
        /* Store in ghost cache (aggressive — always store) */
        uint32_t idx = sig & (8192 - 1);
        ic->ghost.cache[idx].signature = sig;
        ic->ghost.cache[idx].output_ptr = (uint64_t)result;
    }
    
    ic->l1.cycles_total += __builtin_ia32_rdtsc() - start;
    return result;
}

/* ── Initialize ───────────────────────────────────────────────────── */
static inline int infinity_castle_init(InfinityCastle *ic)
{
    memset(ic, 0, sizeof(*ic));
    ic->phase = PHASE_PREWRITE;
    ic->modules_enabled = 1;
    return 0;
}

/* ── Set phase (CRITICAL!) ────────────────────────────────────────── */
static inline void infinity_castle_set_phase(InfinityCastle *ic, Phase phase)
{
    ic->phase = phase;
    
    if (phase == PHASE_COMMIT) {
        /* Disable optimizations during commit */
        ic->modules_enabled = 0;
    } else {
        ic->modules_enabled = 1;
    }
}

/* ── Stats ────────────────────────────────────────────────────────── */
static inline void infinity_castle_print_stats(const InfinityCastle *ic)
{
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Infinity Castle Stats                         ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Trace hits:      %10llu                    ║\n",
           (unsigned long long)ic->l1.trace_hits);
    printf("║ Collapse hits:   %10llu                    ║\n",
           (unsigned long long)ic->l1.collapse_hits);
    printf("║ Ghost hits:      %10llu                    ║\n",
           (unsigned long long)ic->l1.ghost_hits);
    printf("║                                                ║\n");
    
    if (ic->l1.cycles_total > 0) {
        double avg_cycles = (double)ic->l1.cycles_total / 
                            (ic->l1.trace_hits + ic->l1.collapse_hits + ic->l1.ghost_hits + 1);
        double fastpath_cycles = (double)ic->l1.cycles_fastpath /
                                  (ic->l1.ghost_hits + ic->l1.collapse_hits + 1);
        
        printf("║ Avg cycles:      %10.1f                    ║\n", avg_cycles);
        printf("║ Fast path cycles:%10.1f                    ║\n", fastpath_cycles);
    }
    
    printf("╚════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_INFINITY_CASTLE_H */
