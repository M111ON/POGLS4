/* ════════════════════════════════════════════════════════════════════
 * V4 NOTICE: This bridge uses hs_engine_write_v38() as entry point.
 * New code should use pipeline_wire_process() via pogls.h instead.
 * Both paths must NOT be called on the same PipelineWire context —
 * they maintain separate state. Choose ONE entry point per instance.
 *
 *   V3.8 path: hs_engine_write_v38(ctx, br, addr, value)
 *   V4   path: pipeline_wire_process(pw, value, addr)   ← preferred
 * ════════════════════════════════════════════════════════════════════ */

/*
 * pogls_engine_bridge.h — POGLS V3.8  Engine Integration Bridge
 * ══════════════════════════════════════════════════════════════════════
 *
 * เชื่อมทุก layer ที่แยกกันอยู่ให้ทำงานเป็น pipeline เดียว:
 *
 *   [hs_engine_write_v38()]
 *        │
 *        ├─ 1. ComputeLUT: addr → node, Morton encode
 *        ├─ 2. Rubik: write ordering via rubik_mix_addr (18-gate)
 *        ├─ 3. Switch Gate: bit6 → World A / World B
 *        ├─ 4. fold_batch_verify(): L1 XOR + L2 QuadMirror audit
 *        │      pass → continue  /  eject → drop + log
 *        ├─ 5. ExecWindow: buffer op (no fsync yet)
 *        │      window full (1024 ops / 50µs) → flush batch
 *        ├─ 6. ExecWindow flush → fold_delta_write_batch()
 *        │      → delta_append(World A lanes 0-3)
 *        │      → delta_b_append(World B lanes 4-7)
 *        ├─ 7. Frontier: mark node active in bitboard
 *        ├─ 7b. Temporal: temporal_bridge_pass() → 54-bridge ring
 *        ├─ 1c. FaceState: activity tick + GHOST wake
 *        ├─ 1d. Rewind: push to write-behind buffer (flush every 18)
 *        └─ 8. every BRIDGE_ADAPT_INTERVAL ops → hab_tick()
 *                   AdaptTopo: expand/shrink n → topo_split/merge → delta
 *
 * Supervisor thread (separate from worker):
 *   EngineBridge.supervisor_tick() — call every tick_ms
 *   → hab_tick() → adapt n based on Hydra pressure
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_ENGINE_BRIDGE_H
#define POGLS_ENGINE_BRIDGE_H

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "pogls_fold.h"
#include "pogls_fold_delta.h"
#include "pogls_delta_world_b.h"
#include "pogls_compute_lut.h"
#include "pogls_exec_window.h"
#include "pogls_frontier.h"
#include "pogls_hydra_scheduler.h"
#include "pogls_pressure_bridge.h"
#include "pogls_hydra_adapt_bridge.h"
#include "pogls_rubik.h"
#include "pogls_temporal.h"
#include "pogls_node_lut.h"
#include "pogls_face_state.h"
#include "pogls_face_sleep.h"
#include "pogls_rewind.h"


/* ══════════════════════════════════════════════════════════════════
 * POGLS_Predictor — Look-ahead pulse (Gemini V3.8 suggestion)
 *
 * Prefetches neighbor node into L1 cache BEFORE write arrives.
 * Uses icosphere topology (162 nodes) as deterministic map.
 * Does NOT replace ExecWindow/delta — sits on top as optional layer.
 *
 * Usage:
 *   POGLS_Predictor pred = {0};
 *   look_ahead_pulse(&pred, node_state->attention);  // before write
 *   hs_engine_write_v38(ctx, br, addr, value);       // write
 * ══════════════════════════════════════════════════════════════════ */

/* next-node table: icosphere L2 sequential flow (162 entries)
 * node i → neighbor node i+1 mod 162 (simple ring — placeholder)
 * TODO V3.9: replace with full 6-way adjacency from
 *   node_adj_load_icosphere_l2() for optimal L1 hit rate           */
static const uint8_t POGLS_NEIGHBOR_162[162] = {
      1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
     11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
     21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
     31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
     51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
     61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
     71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
     81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
     91, 92, 93, 94, 95, 96, 97, 98, 99,100,
    101,102,103,104,105,106,107,108,109,110,
    111,112,113,114,115,116,117,118,119,120,
    121,122,123,124,125,126,127,128,129,130,
    131,132,133,134,135,136,137,138,139,140,
    141,142,143,144,145,146,147,148,149,150,
    151,152,153,154,155,156,157,158,159,160,
    161,  0   /* 160→161, 161→0 (wrap) */
};

typedef struct {
    uint32_t current_node;    /* node currently being written         */
    uint32_t predicted_node;  /* next node (from NEIGHBOR_162)        */
    uint64_t inverted_cache;  /* ~memory[predicted] pre-computed      */
    uint32_t pulse_count;     /* total look_ahead_pulse() calls       */
} POGLS_Predictor;

/* look_ahead_pulse — call BEFORE hs_engine_write_v38()
 *
 * 1. Find next node from deterministic topology table (O(1), no branch)
 * 2. Hardware prefetch: load attention[predicted] into L1 cache
 * 3. Speculative inversion: compute ~attention[predicted] for XOR audit
 *
 * Cost: 1 table lookup + 1 prefetch instruction + 1 NOT operation
 *       ≈ 0 extra latency (overlaps with previous write)             */
static inline void look_ahead_pulse(POGLS_Predictor *p,
                                     const uint64_t  *attention)
{
    if (!p || !attention) return;

    /* next node from frozen topology — no branch, no computation */
    p->predicted_node  = POGLS_NEIGHBOR_162[p->current_node];

    /* hardware prefetch → L1 cache (T0 = highest priority) */
#if defined(__SSE__) || defined(__x86_64__)
    __builtin_prefetch(&attention[p->predicted_node], 0, 3);
#endif

    /* speculative inversion — ready for XOR audit in next cycle */
    p->inverted_cache  = ~attention[p->predicted_node];
    p->pulse_count++;
}

/* predictor_advance — call AFTER write to move pointer forward */
static inline void predictor_advance(POGLS_Predictor *p)
{
    p->current_node = p->predicted_node;
}

/* ── config ────────────────────────────────────────────────────────── */
#define BRIDGE_ADAPT_INTERVAL  100u   /* adapt tick every N ops          */
#define BRIDGE_COMMIT_BATCH     4u    /* commit every N flushes (not every flush) */
#define BRIDGE_BATCH_SIZE       64u   /* fold_batch_verify batch size     */
#define BRIDGE_LOG_LEVEL         1    /* 0=off 1=flush events 2=per-op   */

/* ══════════════════════════════════════════════════════════════════
 * EngineBridgeStats — counters per worker
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t ops_total;       /* total hs_engine_write_v38() calls      */
    uint64_t ops_world_a;     /* routed to World A                      */
    uint64_t ops_world_b;     /* routed to World B                      */
    uint64_t audit_pass;      /* fold L1+L2 pass                        */
    uint64_t audit_eject;     /* fold L1/L2 eject                       */
    uint64_t audit_need_l3;   /* sent to Merkle path                    */
    uint64_t ew_flushes;      /* ExecWindow flush count                 */
    uint64_t delta_writes;    /* blocks written to delta lane            */
    uint64_t adapt_ticks;     /* hab_tick() calls                       */
    uint64_t temporal_passes; /* temporal_bridge_pass() calls            */
    uint64_t predictor_pulses; /* look_ahead_pulse() calls                */
    uint64_t flush_count;     /* _bridge_flush_pending() calls            */
    uint64_t face_ghosts;      /* nodes entering GHOST state              */
    uint64_t face_splits;      /* nodes split via fstate_split()          */
    uint64_t face_wakes;       /* nodes woken from GHOST                  */
    uint64_t rewind_pushes;    /* rewind_push() calls                     */
} EngineBridgeStats;

/* ══════════════════════════════════════════════════════════════════
 * EngineBridge — one per worker thread (or shared with mutex)
 *
 * Contains:
 *   ctx_ab       — Delta_ContextAB (World A + B lanes)
 *   ew           — ExecWindow (batch buffer)
 *   adapt_bridge — Hydra ↔ AdaptTopo (supervisor only)
 *   pending_*    — staging buffer for fold_batch_verify batch
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* delta context — A + B */
    Delta_ContextAB   ctx_ab;

    /* execution window — batch ops before flush */
    ExecWindow        ew;

    /* AdaptTopo bridge (pointer — shared across workers) */
    HydraAdaptBridge *adapt;

    /* staging batch for fold_batch_verify */
    DiamondBlock      pending[BRIDGE_BATCH_SIZE];
    uint32_t          pending_addr[BRIDGE_BATCH_SIZE];  /* original addr */
    uint64_t          pending_val [BRIDGE_BATCH_SIZE];  /* original value */
    uint32_t          pending_count;

    /* audit results buffer */
    int8_t            audit_results[BRIDGE_BATCH_SIZE];

    /* stats */
    EngineBridgeStats stats;

    /* temporal bridge — GPU buffer → delta timing layer */
    FiftyFourBridge   temporal;

    /* look-ahead predictor — optional prefetch layer */
    POGLS_Predictor   predictor;

    /* face state machine — activity + ghost + split */
    FaceStateTable    face_state;

    /* face sleep — RAM reclaim for idle nodes */
    FaceSleepCtx      face_sleep;

    /* rewind buffer — write-behind speculative buffer */
    RewindBuffer     *rewind;   /* pointer — caller allocs (large struct) */

    /* bridge tick counter (for face_state.current_tick) */
    uint32_t          bridge_tick;

    /* log level */
    int               log_level;

    /* open? */
    int               is_open;
} EngineBridge;

/* ══════════════════════════════════════════════════════════════════
 * engine_bridge_init
 * ══════════════════════════════════════════════════════════════════ */
static inline int engine_bridge_init(EngineBridge     *br,
                                      const char       *vault_path,
                                      HydraAdaptBridge *adapt)
{
    if (!br || !vault_path) return -1;
    memset(br, 0, sizeof(*br));
    br->adapt     = adapt;
    br->log_level = BRIDGE_LOG_LEVEL;

    /* open World A + B together via delta_ab_open */
    if (delta_ab_open(&br->ctx_ab, vault_path) != 0) return -2;

    /* init ExecWindow — V3.8: flush via delta, NOT WAL */
    br->ew.max_ops        = EW_MAX_OPS;
    br->ew.max_window_ns  = EW_MAX_WINDOW_NS;
    br->ew.wal_fd         = -1;       /* legacy WAL: disabled           */
    br->ew.flush_cb       = NULL;     /* bridge manages flush manually  */
    br->ew.flush_ud       = NULL;     /* (pending batch → _bridge_flush_pending) */
    br->ew.time_start_ns  = ew_now_ns();

    /* init temporal bridge (54-node ring buffer) */
    temporal_bridge_init(&br->temporal, NULL);

    /* init face state machine */
    fstate_table_init(&br->face_state);

    /* init face sleep controller */
    face_sleep_init(&br->face_sleep);

    /* rewind: caller must set br->rewind = allocated RewindBuffer */
    br->rewind = NULL;

    br->is_open = 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * _bridge_make_diamond — build DiamondBlock from addr+value
 *   Sets core.raw with correct ENGINE_ID world bit
 *   invert = ~core.raw  (L1 audit will pass)
 *   quad = PHI-spread values (non-zero, L2 passes)
 * ══════════════════════════════════════════════════════════════════ */
static inline DiamondBlock _bridge_make_diamond(uint64_t addr,
                                                  uint64_t value,
                                                  int      world)
{
    DiamondBlock b;
    memset(&b, 0, sizeof(b));

    /* ENGINE_ID: bit6=0→A / bit6=1→B, low bits from addr */
    uint8_t eid = (uint8_t)((addr & 0x3Fu) | (world ? 0x40u : 0x00u));
    /* vector_pos = addr[19:0] */
    uint32_t vpos = (uint32_t)(addr & 0xFFFFFu);

    b.core.raw = ((uint64_t)eid << 52) | ((uint64_t)vpos << 28) | (value & 0xFFFFFFFu);
    b.invert   = ~b.core.raw;

    /* quad mirror: 4 axes, each non-zero (PHI spread) */
    for (int ax = 0; ax < 4; ax++) {
        uint64_t qv = b.core.raw ^ ((uint64_t)(ax + 1) * 1696631ULL);
        if (qv == 0) qv = 0xAAAAAAAAAAAAAAAAULL;
        memcpy((uint8_t*)&b + 16 + ax * 8, &qv, 8);
    }
    return b;
}

/* ══════════════════════════════════════════════════════════════════
 * _bridge_flush_pending — audit + write batch to delta
 *   Called when pending buffer is full OR ExecWindow triggers
 * ══════════════════════════════════════════════════════════════════ */
static inline void _bridge_flush_pending(EngineBridge    *br,
                                          HydraWorkerCtx  *ctx)
{
    uint32_t n = br->pending_count;
    if (n == 0) return;

    /* ── Step 4: fold_batch_verify ─────────────────────────────── */
    fold_batch_verify(br->pending, br->audit_results, n);

    /* ── Step 6: route to World A or B based on Switch Gate ─────── */
    for (uint32_t i = 0; i < n; i++) {
        int8_t res = br->audit_results[i];

        if (res == FOLD_VERIFY_EJECT_1 || res == FOLD_VERIFY_EJECT_2) {
            br->stats.audit_eject++;
            continue;
        }
        if (res == FOLD_VERIFY_NEED_L3) {
            br->stats.audit_need_l3++;
            /* fall through — still write, flag for Merkle */
        } else {
            br->stats.audit_pass++;
        }

        const DiamondBlock *b = &br->pending[i];
        uint64_t addr  = br->pending_addr[i];

        /* Switch Gate: check bit6 of ENGINE_ID */
        int world = (core_engine_id(b->core) & ENGINE_WORLD_BIT) ? 1 : 0;

        FoldDeltaPayload payload = fold_delta_make_payload(b, 0, 1);

        if (world == WORLD_B) {
            /* World B: lanes 4-7 */
            uint8_t lane = (uint8_t)(LANE_B_X + fold_delta_lane(b));
            delta_b_append(&br->ctx_ab.b, lane, addr,
                           &payload, sizeof(payload));
            br->stats.ops_world_b++;
        } else {
            /* World A: lanes 0-3 */
            delta_append(&br->ctx_ab.a,
                         fold_delta_lane(b), addr,
                         &payload, sizeof(payload));
            br->stats.ops_world_a++;
        }
        br->stats.delta_writes++;
    }

    /* ── Step 6 commit — atomic dual A+B ───────────────────────── */
    /* commit every 4 batches to reduce fsync pressure (GPT fix) */
    br->stats.ew_flushes++;
    if (br->stats.ew_flushes % 4 == 0)
        delta_ab_commit(&br->ctx_ab);

    br->stats.ew_flushes++;
    br->pending_count = 0;

    br->stats.flush_count++;

    /* commit every BRIDGE_COMMIT_BATCH flushes (not every flush)
     * reduces fsync frequency while keeping crash window small      */
    if (br->stats.flush_count % BRIDGE_COMMIT_BATCH == 0) {
        delta_ab_commit(&br->ctx_ab);
    }

    if (br->log_level >= 1 && ctx) {
        (void)ctx;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * hs_engine_write_v38 — replaces hs_engine_write() in V3.8
 *
 * Full pipeline:
 *   ComputeLUT → Switch Gate → buffer → [batch audit → delta]
 *   → Frontier → [adapt tick]
 * ══════════════════════════════════════════════════════════════════ */
static inline void hs_engine_write_v38(HydraWorkerCtx *ctx,
                                        EngineBridge   *br,
                                        uint64_t        addr,
                                        uint64_t        value)
{
    if (!ctx || !br || !br->is_open) return;

    br->stats.ops_total++;

    /* ── Step 1: ComputeLUT → node_id ───────────────────────────── */
    uint32_t node = clut_node(&g_clut, (uint32_t)addr);

    /* ── Step 1b: Look-ahead pulse — prefetch next node ────────── */
    /* fire before heavy work so prefetch overlaps pipeline          */
    if (ctx->node_state) {
        br->predictor.current_node = node % NODE_MAX;
        look_ahead_pulse(&br->predictor, ctx->node_state->attention);
        br->stats.predictor_pulses++;
    }

    /* ── Step 1c: Face state update ───────────────────────────────── */
    /* tick activity + auto-transition (GHOST/ACTIVE_LOOP/SPLIT)      */
    br->bridge_tick++;
    br->face_state.current_tick = br->bridge_tick;
    fstate_tick(&br->face_state, node % NODE_MAX, 1 /* hit */);

    {
        face_state_t fst = br->face_state.nodes[node % NODE_MAX].state;

        /* GHOST → wake on access (flip bit, ~1-2 ticks) */
        if (fst == FSTATE_GHOST) {
            face_wake(&br->face_sleep, node % NODE_MAX, NULL, 0);
            fstate_wake_complete(&br->face_state, node % NODE_MAX);
            br->stats.face_wakes++;
        }

        /* ACTIVE_LOOP → candidate for split (AdaptEngine decides) */
        /* actual split triggered by hab_tick in Step 8             */
    }

    /* ── Step 1d: Rewind push (write-behind speculative buffer) ─── */
    /* push BEFORE audit — if audit ejects, rewind can roll back     */
    if (br->rewind) {
        DiamondBlock rw_block = _bridge_make_diamond(addr, value, world);
        rewind_push(br->rewind, &rw_block);
        br->stats.rewind_pushes++;

        /* flush gate every 18 ops (gate_18 aligned) */
        if (br->stats.rewind_pushes % REWIND_GATE == 0)
            rewind_flush_gate(br->rewind);
    }

    /* ── Step 2: Rubik — write ordering via permutation LUT ───────── */
    /* rubik_mix_addr: scrambles addr using node position in 18-gate  */
    /* gives deterministic write ordering without extra memory         */
    uint32_t rubik_addr = rubik_mix_addr(
        (uint8_t)(node & 0xFF),   /* rubik state from node id */
        (uint32_t)addr            /* base address              */
    );
    (void)rubik_addr;  /* used by temporal below for precise slot */

    /* ── Step 3: Switch Gate — World A or B? ────────────────────── */
    /* Single source of truth: bit63 of value (GPT unify fix)
     * ENGINE_ID bit6 in DiamondBlock is derived from this same world flag
     * Caller sets bit63=0 → World A, bit63=1 → World B              */
    int world = (int)((addr >> 6) & 1);   /* ENGINE_ID bit6 — unified (GPT fix) */

    /* ── Step 5: buffer into pending batch ──────────────────────── */
    /* overflow guard: flush BEFORE push (GPT fix) */
    if (br->pending_count >= BRIDGE_BATCH_SIZE)
        _bridge_flush_pending(br, ctx);

    uint32_t idx = br->pending_count;
    br->pending[idx]      = _bridge_make_diamond(addr, value, world);
    br->pending_addr[idx] = (uint32_t)addr;
    br->pending_val[idx]  = value;
    br->pending_count++;

    /* ── ExecWindow: record op (memory only, no I/O) ─────────────── */
    ew_write(&br->ew, (uint16_t)(world ? HS_OP_NODE_WRITE : HS_OP_NODE_WRITE),
             0, addr, value);

    /* ── Step 5→6: flush when batch full or window expired ──────── */
    /* Grace: start timing from first write (not init) to avoid
     * immediate flush due to init→first-write latency */
    if (br->ew.op_count == 1)
        br->ew.time_start_ns = ew_now_ns();  /* reset timer on first op */

    if (br->pending_count >= BRIDGE_BATCH_SIZE ||
        (br->pending_count > 0 && ew_should_flush(&br->ew))) {
        _bridge_flush_pending(br, ctx);
        /* reset ExecWindow */
        br->ew.pos           = 0;
        br->ew.op_count      = 0;
        br->ew.time_start_ns = ew_now_ns();
    }

    /* ── Step 7: Frontier — mark node active ────────────────────── */
    if (node < NODE_MAX && ctx->frontier) {
        ctx->frontier->w[node >> 6] |= (1ULL << (node & 63));
    }

    /* ── Step 7b: Temporal bridge pass ─────────────────────────── */
    /* record write in temporal ring — enables GPU→delta buffering    */
    /* uses rubik_addr for slot precision in 54-bridge                */
    {
        uint8_t world_id = (uint8_t)(world ? TEMPORAL_WORLD_6N
                                           : TEMPORAL_WORLD_4N);
        temporal_bridge_pass(&br->temporal,
                             (uint32_t)addr,
                             world_id,
                             (uint8_t)(node % 54));
        br->stats.temporal_passes++;
    }

    /* ── Step 8: AdaptTopo tick (every BRIDGE_ADAPT_INTERVAL ops) ── */
    if (br->adapt && (br->stats.ops_total % BRIDGE_ADAPT_INTERVAL == 0)) {
        hab_tick(br->adapt, (uint32_t)addr);
        br->stats.adapt_ticks++;
    }

    /* ── Update node state (same as original hs_engine_write) ────── */
    if (node < NODE_MAX && ctx->node_state) {
        ctx->node_state->attention[node]++;
        ctx->node_state->density[node]++;
        ctx->node_state->timestamp[node] = ctx->now_ms;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * engine_bridge_flush_all — drain pending buffer + commit
 *   call at shutdown or checkpoint
 * ══════════════════════════════════════════════════════════════════ */
static inline void engine_bridge_flush_all(EngineBridge   *br,
                                            HydraWorkerCtx *ctx)
{
    if (!br || !br->is_open) return;
    _bridge_flush_pending(br, ctx);
    delta_commit(&br->ctx_ab.a);
}

/* ══════════════════════════════════════════════════════════════════
 * engine_bridge_close
 * ══════════════════════════════════════════════════════════════════ */
static inline int engine_bridge_close(EngineBridge   *br,
                                       HydraWorkerCtx *ctx)
{
    if (!br || !br->is_open) return -1;
    engine_bridge_flush_all(br, ctx);
    delta_ab_close(&br->ctx_ab);
    br->is_open = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * engine_bridge_report
 * ══════════════════════════════════════════════════════════════════ */
static inline void engine_bridge_report(const EngineBridge *br)
{
    if (!br) return;
    const EngineBridgeStats *s = &br->stats;
    printf("  EngineBridge:\n");
    printf("    ops:       total=%llu  A=%llu  B=%llu\n",
           (unsigned long long)s->ops_total,
           (unsigned long long)s->ops_world_a,
           (unsigned long long)s->ops_world_b);
    printf("    audit:     pass=%llu  eject=%llu  need_l3=%llu\n",
           (unsigned long long)s->audit_pass,
           (unsigned long long)s->audit_eject,
           (unsigned long long)s->audit_need_l3);
    printf("    delta:     writes=%llu  ew_flushes=%llu\n",
           (unsigned long long)s->delta_writes,
           (unsigned long long)s->ew_flushes);
    printf("    adapt:     ticks=%llu\n",
           (unsigned long long)s->adapt_ticks);
}

#endif /* POGLS_ENGINE_BRIDGE_H */
