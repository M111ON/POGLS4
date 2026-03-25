/*
 * pogls_temporal_core.h — POGLS V4.x Temporal Virtual Core Fabric
 * ══════════════════════════════════════════════════════════════════════
 *
 * Temporal Virtual Core = space (split) + time (phase)
 *
 * CORE IDEA:
 *   1 physical core → N virtual cores via phase slicing
 *   cycle = 720 steps (temporal closure)
 *   phase = step % 720
 *   core_id   = phase % N
 *   core_tick = phase / N
 *   run(core_id) only when core_tick == 0
 *
 * Properties:
 *   - deterministic 100% (no shared mutation)
 *   - fault isolation per core (rewind per core)
 *   - zero lock contention (per-core state)
 *   - scale: CPU少 → ลด N / เพิ่ม interval
 *            CPU มาก → run parallel k cores
 *
 * Integration with canonical engine:
 *   input = v_clean (from GPU canonical, already valid)
 *   state = step(state[core_id], v_clean)  ← no verify needed
 *
 * Sub-cycles (gear sync):
 *   144 = anchor enforcement (strong)
 *   80  = medium gear
 *   60  = light gear
 *   40  = burst sync
 *   720 = full cycle (commit point)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_TEMPORAL_CORE_H
#define POGLS_TEMPORAL_CORE_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* ── constants ────────────────────────────────────────────────────── */
#define TC_CYCLE        720u    /* master cycle (temporal closure)      */
#define TC_ANCHOR       144u    /* strong enforcement sub-cycle         */
#define TC_GEAR_MED     80u     /* medium gear                          */
#define TC_GEAR_LIGHT   60u     /* light gear                           */
#define TC_GEAR_BURST   40u     /* burst sync                           */
#define TC_CORES_MAX    16u     /* max virtual cores (= Hydra heads)    */
#define TC_REWIND_DEPTH 18u     /* rewind slots per core (gate_18)      */
#define TC_MAGIC        0x54434F52u  /* "TCOR"                          */

/* compile-time checks */
typedef char _tc_cycle  [(TC_CYCLE % TC_ANCHOR   == 0) ? 1 : -1];
typedef char _tc_gear_l [(TC_CYCLE % TC_GEAR_LIGHT == 0) ? 1 : -1];
typedef char _tc_gear_m [(TC_CYCLE % TC_GEAR_MED  == 0) ? 1 : -1];

/* ── gear event flags ─────────────────────────────────────────────── */
#define TC_EVENT_NONE         0x00u
#define TC_EVENT_BURST        0x01u  /* phase % 40  == 0               */
#define TC_EVENT_GEAR_LIGHT   0x02u  /* phase % 60  == 0               */
#define TC_EVENT_GEAR_MED     0x04u  /* phase % 80  == 0               */
#define TC_EVENT_ANCHOR       0x08u  /* phase % 144 == 0 (strong)      */
#define TC_EVENT_CYCLE_END    0x10u  /* phase == 719 (full cycle done)  */
#define TC_EVENT_CORE_TICK    0x20u  /* core_tick == 0 (run this core)  */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — PER-CORE STATE (isolated, no shared mutation)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  core_id;          /* 0..N-1                               */
    uint32_t  seq;              /* monotonic write counter              */
    uint64_t  last_value;       /* last v_clean processed               */
    uint64_t  state_hash;       /* rolling XOR of processed values      */
    uint32_t  anchor_id;        /* current anchor (default 144)         */
    uint32_t  rewind_head;      /* rewind buffer write pointer          */
    uint64_t  rewind_buf[TC_REWIND_DEPTH]; /* per-core rewind ring      */
    uint64_t  total_ops;        /* ops processed by this core           */
    uint64_t  total_cycles;     /* full 720-cycles completed            */
    uint8_t   flags;            /* TC_EVENT_* bitmask                   */
    uint8_t   _pad[7];
} TCCore;                       /* ~200B                                */

static inline void tc_core_init(TCCore *c, uint32_t core_id)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->core_id   = core_id;
    c->anchor_id = TC_ANCHOR;   /* default anchor = 144                 */
}

/* ── per-core state step ─────────────────────────────────────────── */
static inline void tc_core_step(TCCore *c, uint64_t v_clean)
{
    if (!c) return;

    /* push to rewind buffer (gate_18 aligned) */
    c->rewind_buf[c->rewind_head % TC_REWIND_DEPTH] = v_clean;
    c->rewind_head++;

    /* update state (XOR rolling hash — deterministic) */
    c->state_hash ^= v_clean;
    c->state_hash ^= c->state_hash >> 17;
    c->last_value  = v_clean;
    c->seq++;
    c->total_ops++;
}

/* per-core rewind (undo last N ops) */
static inline uint64_t tc_core_rewind(TCCore *c, uint32_t steps)
{
    if (!c || steps == 0) return 0;
    if (steps > TC_REWIND_DEPTH) steps = TC_REWIND_DEPTH;
    uint32_t available = (c->rewind_head < TC_REWIND_DEPTH)
                         ? c->rewind_head : TC_REWIND_DEPTH;
    if (steps > available) steps = available;
    uint64_t rewound = 0;
    for (uint32_t i = 0; i < steps; i++) {
        if (c->rewind_head == 0) break;
        c->rewind_head--;
        uint64_t v = c->rewind_buf[c->rewind_head % TC_REWIND_DEPTH];
        c->state_hash ^= v;
        c->state_hash ^= c->state_hash >> 17;
        if (c->total_ops > 0) c->total_ops--;
        rewound++;
    }
    return rewound;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — TEMPORAL FABRIC (N virtual cores + 720 scheduler)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  magic;
    uint32_t  N;            /* number of virtual cores (1..TC_CORES_MAX) */
    uint64_t  step;         /* global step counter (monotonic)            */
    uint32_t  phase;        /* step % 720                                 */
    uint32_t  core_id;      /* current core: phase % N                    */
    uint32_t  core_tick;    /* phase / N (run when == 0)                  */
    uint8_t   events;       /* TC_EVENT_* bitmask for current step        */
    uint8_t   _pad[3];
    uint64_t  total_steps;
    uint64_t  total_cycles;
    TCCore    cores[TC_CORES_MAX];
} TCFabric;

static inline void tc_fabric_init(TCFabric *f, uint32_t N)
{
    if (!f) return;
    if (N == 0 || N > TC_CORES_MAX) N = TC_CORES_MAX;
    memset(f, 0, sizeof(*f));
    f->magic = TC_MAGIC;
    f->N     = N;
    for (uint32_t i = 0; i < N; i++)
        tc_core_init(&f->cores[i], i);
}

/* compute events for current phase */
static inline uint8_t tc_events(uint32_t phase, uint32_t core_tick)
{
    uint8_t ev = TC_EVENT_NONE;
    if (phase % TC_GEAR_BURST  == 0) ev |= TC_EVENT_BURST;
    if (phase % TC_GEAR_LIGHT  == 0) ev |= TC_EVENT_GEAR_LIGHT;
    if (phase % TC_GEAR_MED    == 0) ev |= TC_EVENT_GEAR_MED;
    if (phase % TC_ANCHOR      == 0) ev |= TC_EVENT_ANCHOR;
    if (phase == TC_CYCLE - 1u)      ev |= TC_EVENT_CYCLE_END;
    if (core_tick == 0)              ev |= TC_EVENT_CORE_TICK;
    return ev;
}

/*
 * tc_dispatch — main scheduler entry point
 *
 * Call once per step with v_clean (from GPU canonical)
 * Returns: events bitmask (caller checks TC_EVENT_* flags)
 *
 * Execution rule:
 *   only run core_id when core_tick == 0
 *   → N virtual cores share 720-step cycle evenly
 */
static inline uint8_t tc_dispatch(TCFabric *f, uint64_t v_clean)
{
    if (!f) return TC_EVENT_NONE;

    /* phase based on current step (before increment) */
    f->phase     = (uint32_t)(f->total_steps % TC_CYCLE);
    f->core_id   = f->phase % f->N;
    f->core_tick = f->phase / f->N;
    f->events    = tc_events(f->phase, f->core_tick);
    f->step      = f->total_steps;
    f->total_steps++;

    /* run core only on its tick (core_tick == 0) */
    if (f->core_tick == 0) {
        tc_core_step(&f->cores[f->core_id], v_clean);
    }

    /* cycle complete */
    if (f->events & TC_EVENT_CYCLE_END)
        f->total_cycles++;

    return f->events;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — ANCHOR ENFORCEMENT HOOK
 *
 * Called by pipeline at TC_EVENT_ANCHOR (phase % 144 == 0)
 * → strong canonical enforcement on all cores
 * ══════════════════════════════════════════════════════════════════════ */

typedef uint32_t (*TCCanonFn)(uint32_t v);   /* cpu_canonicalize */

static inline void tc_anchor_enforce(TCFabric *f, TCCanonFn canon_fn)
{
    if (!f || !canon_fn) return;
    for (uint32_t i = 0; i < f->N; i++) {
        TCCore *c = &f->cores[i];
        /* re-canonicalize last value to confirm no drift */
        uint32_t v32 = (uint32_t)(c->last_value & 0xFFFFFFFFu);
        uint32_t v_check = canon_fn(v32);
        /* if drift detected: snap state_hash */
        if (v_check != v32) {
            c->state_hash ^= (uint64_t)v_check;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — SCALING HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* interval between ticks for core_id (steps between runs) */
static inline uint32_t tc_core_interval(const TCFabric *f)
{
    return f ? f->N : 1u;
}

/* how many cores run in parallel (for multi-CPU) */
static inline uint32_t tc_parallel_cores(const TCFabric *f, uint32_t k)
{
    if (!f || k == 0) return 1;
    return (k < f->N) ? k : f->N;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — STATS
 * ══════════════════════════════════════════════════════════════════════ */

static inline void tc_stats_print(const TCFabric *f)
{
    if (!f) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Temporal Core Fabric Stats                     ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ N=%u virtual cores,  cycle=720,  anchor=144      ║\n", f->N);
    printf("║ total_steps:  %10llu                         ║\n",
           (unsigned long long)f->total_steps);
    printf("║ total_cycles: %10llu                         ║\n",
           (unsigned long long)f->total_cycles);
    printf("╠══════════════════════════════════════════════════╣\n");
    for (uint32_t i = 0; i < f->N; i++) {
        printf("║ core[%2u] ops=%-8llu state=%08llx          ║\n",
               i,
               (unsigned long long)f->cores[i].total_ops,
               (unsigned long long)(f->cores[i].state_hash & 0xFFFFFFFFu));
    }
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_TEMPORAL_CORE_H */
