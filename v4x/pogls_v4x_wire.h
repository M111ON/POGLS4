/*
 * pogls_v4x_wire.h — POGLS V4.x Full Pipeline Wire
 * ══════════════════════════════════════════════════════════════════════
 *
 * Step 3: wire can_push/pop → tc_dispatch → ma_step → commit
 *
 * Pipeline (per step):
 *   v_raw
 *     ↓ cpu_canonicalize()       — snap to valid Pythagorean grid
 *   v_clean
 *     ↓ tc_dispatch()            — 720-step phase scheduler
 *   events (TC_EVENT_* bitmask)
 *     ↓ ma_step()                — multi-anchor soft snap
 *   v_snapped
 *     ↓ v4x_commit()             — write to output ring + state update
 *
 * Anchor enforcement:
 *   TC_EVENT_ANCHOR  (phase % 144 == 0) → ma_select_best + alpha rise
 *   TC_EVENT_CYCLE_END (phase == 719)   → alpha decay + snapshot sync
 *
 * Rules (frozen):
 *   - cpu_canonicalize() = CPU mirror of GPU kernel (no float, no CUDA)
 *   - v_clean is ALWAYS canonical before tc_dispatch
 *   - ma_step() only soft-snaps — never rejects, never blocks
 *   - commit runs every step (no gating on events)
 *   - N must match between TCFabric and MAFabric
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_V4X_WIRE_H
#define POGLS_V4X_WIRE_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── pull canonical CPU mirror (no CUDA needed) ─────────────────────
 * Define guard so pogls_canonical.cu skips CUDA-only sections         */
#ifndef POGLS_CANONICAL_CPU_ONLY
#  define POGLS_CANONICAL_CPU_ONLY
#endif

/* constants from canonical engine */
#define CAN_PHI_UP   1696631u
#define CAN_PHI_DOWN  648055u
#define CAN_ANCHOR      144u
#define CAN_GRID_A       12u
#define CAN_GRID_B        9u
#define CAN_CYCLE       720u

/* compile-time sanity */
typedef char _wire_can_a[(CAN_GRID_A * CAN_GRID_A == CAN_ANCHOR)  ? 1 : -1];
typedef char _wire_can_b[(CAN_CYCLE  % CAN_ANCHOR  == 0)          ? 1 : -1];

/* cpu_canonicalize — inline (mirrors pogls_canonical.cu SECTION 5)
 *
 * PACK layout (64-bit):
 *   bits [62:42] = x  (21 bits, max = a_max²  = 132² = 17424  < 2^21)
 *   bits [41:21] = y  (21 bits, max = 2·a·b   = 2·132·135 = 35640 < 2^21)
 *   bits [20: 0] = z  (21 bits, max = a²+b²   = 132²+135² = 35649 < 2^21)
 *
 * Reversible: x,y,z recoverable → debug + repair + GPU verify work.
 * Invariant preserved: x²+y²=z² survives the pack unchanged.
 * Degenerate guard: a==0 && b==0 → a=12 (prevents entropy collapse 0.54%)
 */
static inline uint32_t _wire_can_f(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_UP  >> 20) % CAN_ANCHOR);
}
static inline uint32_t _wire_can_g(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_DOWN >> 20) % CAN_ANCHOR);
}

/* pack(x,y,z) → 63-bit canonical token — reversible, invariant preserved */
static inline uint64_t wire_pack(uint32_t x, uint32_t y, uint32_t z) {
    return ((uint64_t)(x & 0x1FFFFFu) << 42)
         | ((uint64_t)(y & 0x1FFFFFu) << 21)
         |  (uint64_t)(z & 0x1FFFFFu);
}
static inline void wire_unpack(uint64_t p,
                                uint32_t *x, uint32_t *y, uint32_t *z) {
    if (x) *x = (uint32_t)((p >> 42) & 0x1FFFFFu);
    if (y) *y = (uint32_t)((p >> 21) & 0x1FFFFFu);
    if (z) *z = (uint32_t)( p        & 0x1FFFFFu);
}

/* wire_canonicalize — returns 63-bit packed (x,y,z), NOT a hash
 * Caller uses uint64_t for full precision; truncated uint32_t = z only */
static inline uint64_t wire_canonicalize(uint32_t v)
{
    uint32_t a = (_wire_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (_wire_can_g(v) / CAN_GRID_B) * CAN_GRID_B;

    /* degenerate guard: a==0 && b==0 → entropy collapse → fix to 12-grid */
    if (a == 0u && b == 0u) a = CAN_GRID_A;   /* a=12, b stays 0 */

    /* Pythagorean triple requires a >= b (x = a²-b² must not underflow)
     * swap if needed — symmetric, no entropy loss                       */
    if (b > a) { uint32_t t = a; a = b; b = t; }

    uint32_t x = a*a - b*b;
    uint32_t y = 2u*a*b;
    uint32_t z = a*a + b*b;
    return wire_pack(x, y, z);
}

#include "pogls_temporal_core.h"
#include "pogls_multi_anchor.h"

/* ══════════════════════════════════════════════════════════════════════
 * COMMIT RING — lock-free output buffer
 *
 * Simple power-of-2 ring: producer writes v_snapped + metadata.
 * Consumer (snapshot / delta writer) drains at cycle boundary.
 * Size = 1024 entries = covers > 1 full 720-cycle without wrap.
 * ══════════════════════════════════════════════════════════════════════ */

#define V4X_RING_SIZE   1024u
#define V4X_RING_MASK   (V4X_RING_SIZE - 1u)

typedef struct {
    uint32_t  v_snapped;    /* canonical + soft-snapped value           */
    uint64_t  v_clean;      /* packed(x,y,z) canonical — reversible     */
    uint32_t  anchor;       /* anchor used at this step                 */
    uint8_t   core_id;      /* which virtual core processed this        */
    uint8_t   events;       /* TC_EVENT_* bitmask at this step          */
    uint8_t   alpha;        /* alpha at snap time                       */
    uint8_t   _pad;
    uint64_t  step;         /* global step number                       */
} V4xCommitEntry;           /* 32B                                      */

typedef struct {
    V4xCommitEntry  buf[V4X_RING_SIZE];
    uint64_t        write_head;     /* producer index (monotonic)       */
    uint64_t        read_head;      /* consumer index (monotonic)       */
    uint64_t        total_commits;
    uint64_t        total_overflows;/* write lapped read (warn only)    */
} V4xRing;

static inline void v4x_ring_init(V4xRing *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

/* write — always succeeds, overwrites oldest on overflow (warn only) */
static inline void v4x_ring_push(V4xRing *r, const V4xCommitEntry *e)
{
    if (!r || !e) return;
    uint64_t idx = r->write_head & V4X_RING_MASK;
    /* check for overflow (write lapped read) */
    if (r->write_head - r->read_head >= V4X_RING_SIZE)
        r->total_overflows++;
    r->buf[idx] = *e;
    r->write_head++;
    r->total_commits++;
}

/* pop — returns 1 if entry available, 0 if empty */
static inline int v4x_ring_pop(V4xRing *r, V4xCommitEntry *out)
{
    if (!r || !out) return 0;
    if (r->read_head >= r->write_head) return 0;
    uint64_t idx = r->read_head & V4X_RING_MASK;
    *out = r->buf[idx];
    r->read_head++;
    return 1;
}

static inline uint64_t v4x_ring_pending(const V4xRing *r) {
    return r ? (r->write_head - r->read_head) : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * V4xWire — top-level context
 *
 * Holds TCFabric + MAFabric + commit ring + stats.
 * Init once, call v4x_step() for every incoming value.
 * ══════════════════════════════════════════════════════════════════════ */

#define V4X_MAGIC  0x56345857u   /* "V4XW" */

typedef struct {
    uint32_t    magic;
    uint32_t    N;              /* virtual core count                   */
    TCFabric    tc;             /* temporal core scheduler              */
    MAFabric    ma;             /* multi-anchor fabric                  */
    V4xRing     ring;           /* commit output ring                   */

    /* stats */
    uint64_t    total_steps;
    uint64_t    anchor_enforces;    /* TC_EVENT_ANCHOR fires            */
    uint64_t    cycle_ends;         /* TC_EVENT_CYCLE_END fires         */
    uint64_t    canonicalize_calls; /* cpu_canonicalize() calls         */
} V4xWire;

static inline int v4x_wire_init(V4xWire *w, uint32_t N)
{
    if (!w) return -1;
    if (N == 0 || N > TC_CORES_MAX) N = 4u;  /* default 4 virtual cores */
    memset(w, 0, sizeof(*w));
    w->magic = V4X_MAGIC;
    w->N     = N;
    tc_fabric_init(&w->tc, N);
    ma_fabric_init(&w->ma, N);
    v4x_ring_init(&w->ring);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * v4x_step — MAIN ENTRY POINT
 *
 * Call once per incoming v_raw.
 * Returns v_snapped (z-component of canonical triple, soft-snapped).
 *
 * Internal sequence:
 *   1. wire_canonicalize(v_raw)  → v_clean (packed x,y,z — reversible)
 *   2. tc_dispatch(z)            → events  (720-step phase scheduler)
 *   3. TC_EVENT_ANCHOR (inline)  → re-canonicalize each core's last value
 *      if (phase % 144 == 0) state = canonicalize(state)   ← simple + stable
 *   4. ma_step(z, events)        → v_snapped (multi-anchor soft snap on z)
 *   5. commit packed v_clean + v_snapped → ring
 * ══════════════════════════════════════════════════════════════════════ */
static inline uint32_t v4x_step(V4xWire *w, uint32_t v_raw)
{
    if (!w) return v_raw;

    /* ── 1. canonicalize → packed (x,y,z) ─────────────────────────── */
    uint64_t v_clean = wire_canonicalize(v_raw);
    w->canonicalize_calls++;

    /* unpack z (= a²+b²) as the working value for routing + snap */
    uint32_t vx, vy, vz;
    wire_unpack(v_clean, &vx, &vy, &vz);

    /* ── 2. temporal dispatch on z ─────────────────────────────────── */
    uint8_t events = tc_dispatch(&w->tc, (uint64_t)vz);

    /* ── 3. anchor enforcement — inline, no callback overhead ──────── *
     * Change 3: replace tc_anchor_enforce(fn) with direct phase check.
     * if (phase % 144 == 0) → re-snap each core's last_value via canon.
     * Simpler, stable, same determinism, zero function pointer indirection. */
    if (events & TC_EVENT_ANCHOR) {
        for (uint32_t i = 0; i < w->N; i++) {
            TCCore *c = &w->tc.cores[i];
            uint32_t v32 = (uint32_t)(c->last_value & 0xFFFFFFFFu);
            uint64_t re  = wire_canonicalize(v32);
            uint32_t rz;
            wire_unpack(re, NULL, NULL, &rz);
            /* snap state_hash toward canonical z if drift detected */
            if (rz != v32)
                c->state_hash ^= (uint64_t)rz;
        }
        w->anchor_enforces++;
    }
    if (events & TC_EVENT_CYCLE_END)
        w->cycle_ends++;

    /* ── 4. multi-anchor soft snap on z ────────────────────────────── */
    uint32_t v_snapped = ma_step(&w->ma, &w->tc, vz, events);

    /* ── 5. commit to output ring ──────────────────────────────────── */
    {
        uint32_t cid = w->tc.core_id < w->N ? w->tc.core_id : 0u;
        V4xCommitEntry e;
        e.v_snapped = v_snapped;
        e.v_clean   = v_clean;        /* full packed(x,y,z) — reversible */
        e.anchor    = w->ma.ctx[cid].anchor;
        e.core_id   = (uint8_t)cid;
        e.events    = events;
        e.alpha     = (uint8_t)(w->ma.ctx[cid].alpha & 0xFFu);
        e.step      = w->tc.step;
        v4x_ring_push(&w->ring, &e);
    }

    w->total_steps++;
    return v_snapped;
}

/* ── drain ring — process all pending entries ────────────────────────
 * Callback receives each committed entry.
 * Returns number of entries drained.                                  */
typedef void (*V4xDrainFn)(const V4xCommitEntry *e, void *userdata);

static inline uint64_t v4x_drain(V4xWire *w,
                                   V4xDrainFn fn,
                                   void       *userdata)
{
    if (!w) return 0;
    uint64_t count = 0;
    V4xCommitEntry e;
    while (v4x_ring_pop(&w->ring, &e)) {
        if (fn) fn(&e, userdata);
        count++;
    }
    return count;
}

/* ══════════════════════════════════════════════════════════════════════
 * STATS
 * ══════════════════════════════════════════════════════════════════════ */
static inline void v4x_wire_stats(const V4xWire *w)
{
    if (!w) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  V4x Wire Stats                                 ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ total_steps:       %10llu                   ║\n",
           (unsigned long long)w->total_steps);
    printf("║ canonicalize:      %10llu                   ║\n",
           (unsigned long long)w->canonicalize_calls);
    printf("║ anchor_enforces:   %10llu                   ║\n",
           (unsigned long long)w->anchor_enforces);
    printf("║ cycle_ends:        %10llu                   ║\n",
           (unsigned long long)w->cycle_ends);
    printf("║ ring_pending:      %10llu                   ║\n",
           (unsigned long long)v4x_ring_pending(&w->ring));
    printf("║ ring_commits:      %10llu                   ║\n",
           (unsigned long long)w->ring.total_commits);
    printf("║ ring_overflows:    %10llu                   ║\n",
           (unsigned long long)w->ring.total_overflows);
    printf("╚══════════════════════════════════════════════════╝\n\n");
    tc_stats_print(&w->tc);
    ma_stats_print(&w->ma);
}

#endif /* POGLS_V4X_WIRE_H */
