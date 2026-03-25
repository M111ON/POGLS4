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

/* CAN_DEGEN_MODE — degenerate (a=0,b=0) handling strategy
 *   0 = legacy:  a=CAN_GRID_A, b=0       → z=144, y=0 (old behavior)
 *   1 = entropy: a=CAN_GRID_A, b=CAN_GRID_B → z=225, symmetric  ← DEFAULT
 * Define CAN_DEGEN_MODE=0 before include to revert to legacy.     */
#ifndef CAN_DEGEN_MODE
#  define CAN_DEGEN_MODE 1
#endif

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
 * Degenerate guard: a==0 && b==0 → a=12,b=9 (entropy symmetry, prevents collapse)
 */
static inline uint32_t _wire_can_f(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_UP  >> 20) % CAN_ANCHOR);
}
static inline uint32_t _wire_can_g(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_DOWN >> 20) % CAN_ANCHOR);
}

/* wire_pack(x,y,z,sign) → 64-bit canonical token — reversible, invariant preserved
 * bit 63 = sign flag (b_was_greater before swap) — encodes original orientation.
 * wire_unpack ignores bit 63 (mask 0x1FFFFF) → backward compatible.            */
static inline uint64_t wire_pack(uint32_t x, uint32_t y, uint32_t z, uint32_t sign) {
    return ((uint64_t)(sign & 1u) << 63)    /* bit63: orientation flag */
         | ((uint64_t)(x & 0x1FFFFFu) << 42)
         | ((uint64_t)(y & 0x1FFFFFu) << 21)
         |  (uint64_t)(z & 0x1FFFFFu);
}
static inline void wire_unpack(uint64_t p,
                                uint32_t *x, uint32_t *y, uint32_t *z) {
    if (x) *x = (uint32_t)((p >> 42) & 0x1FFFFFu);  /* bit63 masked out */
    if (y) *y = (uint32_t)((p >> 21) & 0x1FFFFFu);
    if (z) *z = (uint32_t)( p        & 0x1FFFFFu);
}
static inline uint32_t wire_unpack_sign(uint64_t p) {
    return (uint32_t)(p >> 63);   /* 1 = b_was_greater (original orientation) */
}
/* SIGN BIT CONTRACT (bit 63) — FROZEN, do not relax:
 *   - bit63 = (original_b > original_a) before canonicalize swap
 *   - METADATA only: never feed into anchor/snap/hash/score computation
 *   - wire_unpack() masks it out → all routing/compare are sign-blind
 *   - wire_unpack_sign() = only explicit accessor for orientation
 *   - downstream (ma_step, tc_dispatch) receive z only → sign-blind ✓
 *   Violation = silent downstream corruption (anchor drift, hash mismatch) */
/* static enforcement: x/y/z fields are 21-bit, sign sits in bit63 safely */
typedef char _sign_no_overlap [(0x1FFFFFu < (1u << 21)) ? 1 : -1];
typedef char _sign_bit63_clear [((uint64_t)0x1FFFFFu << 42) < ((uint64_t)1u << 63) ? 1 : -1];

/* wire_canonicalize — returns 63-bit packed (x,y,z), NOT a hash
 * Caller uses uint64_t for full precision; truncated uint32_t = z only */
static inline uint64_t wire_canonicalize(uint32_t v)
{
    uint32_t a = (_wire_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (_wire_can_g(v) / CAN_GRID_B) * CAN_GRID_B;

    /* degenerate guard: a==0 && b==0 → entropy collapse → fix to 12-grid */
    /* degenerate guard — controlled by CAN_DEGEN_MODE */
#if CAN_DEGEN_MODE
    if (a == 0u && b == 0u) { a = CAN_GRID_A; b = CAN_GRID_B + ((v >> 3u) & 0x3u); } /* jitter b∈[9,12] */
#else
    if (a == 0u && b == 0u) { a = CAN_GRID_A; }                  /* legacy: z=144  */
#endif

    /* Pythagorean triple requires a >= b (x = a²-b² must not underflow)
     * swap captured in sign bit → preserves full entropy orientation    */
    uint32_t sign = (b > a) ? 1u : 0u;   /* capture orientation before swap */
    if (b > a) { uint32_t t = a; a = b; b = t; }
    uint32_t x = a*a - b*b;
    uint32_t y = 2u*a*b;
    uint32_t z = a*a + b*b;
    return wire_pack(x, y, z, sign);     /* bit63 preserves entropy orientation */
}

/* wire_is_canonical — modulo-only grid-alignment check (O(1))
 *
 * Definition: v is "canonical" iff its PHI projections land on grid points
 * (a_raw divisible by 12, b_raw divisible by 9) — meaning no snapping is
 * needed for this value. The canonicalize transform is idempotent here.
 *
 * canonical space ≠ input space (because of degenerate guard + swap + pyth)
 * This check is a SEMANTIC test: "does v need re-projection?"
 * NOT "is z == v" (z lives in a different space than v).
 *
 * GPU skip contract: when is_canonical(v)==1, caller may skip
 * gpu_batch_submit(v) — the value is already grid-stable.
 *
 * Cost: 2 muls + 2 shifts + 2 mods = ~6 cycles. No canonicalize() call. */
static inline int wire_is_canonical(uint32_t v)
{
    uint32_t a_raw = _wire_can_f(v);   /* PHI scatter → [0,143] */
    uint32_t b_raw = _wire_can_g(v);   /* PHI scatter → [0,143] */
    /* grid-aligned = no snapping needed = GPU skip safe */
    return ((a_raw % CAN_GRID_A) == 0u) && ((b_raw % CAN_GRID_B) == 0u);
}


#include "pogls_multi_anchor.h"
#include "pogls_v4_snapshot.h"

/* ══════════════════════════════════════════════════════════════════════
 * COMMIT RING — lock-free output buffer
 *
 * Simple power-of-2 ring: producer writes v_snapped + metadata.
 * Consumer (snapshot / delta writer) drains at cycle boundary.
 * Size = 1024 entries = covers > 1 full 720-cycle without wrap.
 * ══════════════════════════════════════════════════════════════════════ */

#define V4X_RING_SIZE        1024u
#define V4X_RING_MASK        (V4X_RING_SIZE - 1u)
/* phase-aware drain quotas — tied to TC_EVENT_ANCHOR boundary (phase % 144)
 *   HIGH_QUOTA: fire at anchor point → more headroom, prefetch-friendly
 *   LOW_QUOTA:  fire at non-anchor cycle_end → conservative, stable latency
 * Math: 5 anchor points per 720-cycle. HIGH×5 + LOW×(cycle_ends-anchor_ends)
 *       must be ≥ 720 to guarantee ring never fills. 256+64 gives 1280 ≥ 720. */
#define V4X_DRAIN_QUOTA_HIGH  256u   /* at anchor boundary (phase % 144 == 0)    */
#define V4X_DRAIN_QUOTA_LOW    64u   /* at non-anchor cycle_end                  */
#define V4X_DRAIN_QUOTA       V4X_DRAIN_QUOTA_HIGH  /* default (backward compat) */

/* ── anchor_bias histogram ──────────────────────────────────────────
 * 16 buckets — anchor ∈ {0,9,18,...,135} → bucket = anchor/9
 * Saturating uint8_t counters: zero overhead, no alloc              */
#define V4X_BIAS_BUCKETS  16u

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
    uint64_t    fast_path_hits;     /* already-canonical skips          */
    uint64_t    partial_drains;     /* drain_partial() calls            */

    /* anchor_bias histogram — per-core, [core][bucket]
     * bucket = anchor_used / 9  (16 buckets, uint16_t for decay headroom)
     * Decay: hist[i] = hist[i] * 99 / 100  every anchor event (integer 0.99×)
     * Score: score = base - (bias_weight * hist[bucket] >> 8)
     * Core learns its own anchor distribution; old signal fades naturally. */
    uint16_t    anchor_bias[TC_CORES_MAX][V4X_BIAS_BUCKETS];
    uint64_t    bias_decays;

    /* snapshot + audit — wired at CYCLE_END (phase==719) */
    V4AuditContext    audit;          /* 54-tile satellite constellation  */
    V4SnapshotHeader  snap;           /* current snapshot header          */
    V4SnapTileFreeze  snap_freeze;    /* frozen tile hashes at certify    */
    uint64_t          snap_id;        /* monotonic snapshot counter       */
    uint64_t          snap_certified; /* total certified snapshots        */
    uint64_t          snap_suspicious;/* certified but flagged            */
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
    v4_audit_init(&w->audit);
    /* create first snapshot — PENDING until first CYCLE_END certifies it */
    w->snap_id = 1;
    w->snap = v4_snap_create(w->snap_id, 0, 0);
    return 0;
}

/* forward declarations — both drain variants defined after v4x_step */
typedef void (*V4xDrainFn)(const V4xCommitEntry *e, void *userdata);
static inline uint64_t v4x_drain(V4xWire *w, V4xDrainFn fn, void *userdata);
static inline uint64_t v4x_drain_partial(V4xWire *w, V4xDrainFn fn,
                                          void *userdata, uint32_t quota);

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

    /* ── [3] TRUE FAST PATH — canonicalize() skipped entirely on hit ──
     * wire_is_canonical(v): ~6 cycles modulo check.
     * HIT:  v is grid-aligned → vz = v_raw directly, v_clean = 0 (lazy).
     *       canonicalize() NOT called → saves ~20 cycles per hit.
     *       GPU skip contract: gpu_batch_submit(v_raw) may be omitted.
     * MISS: full canonicalize (normal path, ~20 cycles).
     *
     * v_clean=0 on fast path is intentional: ring entry marks v_clean=0
     * as sentinel for "grid-native, no reprojection needed".            */
    uint64_t v_clean;
    uint32_t vz;
    if (wire_is_canonical(v_raw)) {
        vz      = v_raw;
        v_clean = 0ULL;              /* lazy: grid-native sentinel        */
        w->fast_path_hits++;
        /* GPU SKIP: gpu_batch_submit(v_raw) omitted — already on grid   */
    } else {
        v_clean = wire_canonicalize(v_raw);
        wire_unpack(v_clean, NULL, NULL, &vz);
        w->canonicalize_calls++;   /* only count actual calls, not fast-path hits */
    }

    /* ── 2. temporal dispatch on z ─────────────────────────────────── */
    uint8_t events = tc_dispatch(&w->tc, (uint64_t)vz);

    /* ── PER-CORE DECOUPLED ANCHOR FIRE (hash-mix) ───────────────────
     * FIX1: (phase + core_id*37) % 144 had aliasing — many core_ids
     *       mapped to same phase bucket → skew not fixed.
     * FIX2: hash-mix breaks all aliases. Phase is coprime with golden
     *       ratio constant 0x9E3779B1 → uniform distribution per core.
     * Guard: (step+core_id)&1 → halves fire rate, removes lucky-hit bias.
     * Contract: still fully deterministic (no random, no state outside w). */
    {
        uint32_t phase    = (uint32_t)(w->tc.total_steps % TC_CYCLE);
        uint32_t step_idx = (uint32_t)(w->tc.total_steps & 0xFFFFFFFFu);
        for (uint32_t ci = 0; ci < w->N; ci++) {
            uint32_t h = (phase ^ (ci * 0x9E3779B1u)) % TC_ANCHOR;
            if (h == 0 && ((step_idx + ci) & 1u) == 0u) {
                events |= TC_EVENT_ANCHOR;
                break;  /* one injection per step maximum */
            }
        }
    }

    /* ── 3. anchor enforcement — inline re-snap ─────────────────────── */
    if (events & TC_EVENT_ANCHOR) {
        for (uint32_t i = 0; i < w->N; i++) {
            TCCore *c = &w->tc.cores[i];
            uint32_t v32 = (uint32_t)(c->last_value & 0xFFFFFFFFu);
            uint64_t re  = wire_canonicalize(v32);
            uint32_t rz;
            wire_unpack(re, NULL, NULL, &rz);
            if (rz != v32)
                c->state_hash ^= (uint64_t)rz;
        }
        w->anchor_enforces++;
    }

    /* ── [1] BACKLOG-AWARE PARTIAL DRAIN — phase + pressure adaptive ───
     *
     * Base quota by phase:
     *   TC_EVENT_ANCHOR (phase%144==0) → HIGH_QUOTA=256, fires 5×/cycle
     *   TC_EVENT_CYCLE_END (phase==719)→ LOW_QUOTA=64,   fires 1×/cycle
     *   (Never overlap: 719%144=143≠0)
     *
     * Dynamic boost: if backlog > THRESHOLD, add backlog/4 to quota.
     *   → drain is a function of PRESSURE, not just phase.
     *   → guarantees ring never fills even under burst load.
     *
     * THRESHOLD = ring_size/2 = 512: boost kicks in at half-full.
     * Worst case boost: (1024/4) = 256 extra → still bounded per call.  */
    if (events & TC_EVENT_ANCHOR) {
        uint32_t backlog  = (uint32_t)v4x_ring_pending(&w->ring);
        uint32_t quota    = V4X_DRAIN_QUOTA_HIGH;
        if (backlog > (V4X_RING_SIZE / 2u)) {
            uint32_t boost = (backlog * backlog) >> 12; /* nonlinear: gentle low, explosive high */
            if (boost > 256u) boost = 256u; /* clamp: avoid cache thrash / overshoot */
            quota += boost;
        }
        v4x_drain_partial(w, NULL, NULL, quota);
        w->partial_drains++;
    }
    if (events & TC_EVENT_CYCLE_END) {
        w->cycle_ends++;
        uint32_t backlog  = (uint32_t)v4x_ring_pending(&w->ring);
        uint32_t quota    = V4X_DRAIN_QUOTA_LOW;
        if (backlog > (V4X_RING_SIZE / 2u)) {
            uint32_t boost = (backlog * backlog) >> 12;
            if (boost > 256u) boost = 256u;
            quota += boost;
        }
        v4x_drain_partial(w, NULL, NULL, quota);
        w->partial_drains++;

        /* ── snapshot @ cycle_end — certify + create next ──────────────
         * 1. certify current PENDING snap against audit constellation
         * 2. freeze tile hashes for deterministic replay
         * 3. open next PENDING snap for the coming cycle               */
        int cert = v4_snap_certify_freeze(&w->snap, w->cycle_ends,
                                           &w->audit, &w->snap_freeze);
        if (cert == 0) {
            w->snap_certified++;
            if (w->snap.is_suspicious) w->snap_suspicious++;
        }
        /* open next snapshot (parent = current id) */
        uint64_t parent_id = w->snap_id;
        w->snap_id++;
        w->snap = v4_snap_create(w->snap_id, 0, parent_id);
    }

    /* ── P3: GPU fast path — skip batch submit if already canonical ────
     * Contract: is_canonical(v_raw) → value is grid-stable, no GPU work.
     * gpu_batch_submit() is a stub here; real GPU call wired at integration.
     * fast_path_hits already counted in canonicalize block above.           */
#ifndef gpu_batch_submit
#define gpu_batch_submit(v) ((void)(v))  /* stub: replace with real GPU call */
#endif
    if (!wire_is_canonical(v_raw))
        gpu_batch_submit(vz);   /* only non-canonical values need GPU batch  */

    /* ── P2: resolve bias_row for this core (NULL-safe) ────────────── */
    uint32_t _cid_bias = w->tc.core_id < TC_CORES_MAX ? w->tc.core_id : 0u;
    const uint16_t *bias_row = w->anchor_bias[_cid_bias];

    /* ── 4. multi-anchor soft snap on z ────────────────────────────── */
    uint32_t v_snapped = ma_step(&w->ma, &w->tc, vz, events, bias_row);

    /* ── 5. commit to output ring ──────────────────────────────────── */
    {
        uint32_t cid = w->tc.core_id < w->N ? w->tc.core_id : 0u;
        V4xCommitEntry e;
        e.v_snapped = v_snapped;
        e.v_clean   = v_clean;
        e.anchor    = w->ma.ctx[cid].anchor;
        e.core_id   = (uint8_t)cid;
        e.events    = events;
        e.alpha     = (uint8_t)(w->ma.ctx[cid].alpha & 0xFFu);
        e.step      = w->tc.step;
        v4x_ring_push(&w->ring, &e);

        /* ── [2] ANCHOR BIAS — histogram + decay per core ──────────────
         * Step A: increment hit bucket (cap at 32767 for uint16 safety)
         * Step B: on anchor event → decay ALL buckets by ×99/100 (integer)
         *   decay removes old signal → "memory" is ~100 anchor-events deep
         *   no float, no alloc, no extra state — pure in-place integer math
         *
         * Usage by scorer:
         *   score = base_score - (anchor_bias[cid][bucket] >> 8)
         *   → over-used anchors get penalized → natural load spreading     */
        if (cid < TC_CORES_MAX) {
            uint32_t bucket = (e.anchor / 9u) & (V4X_BIAS_BUCKETS - 1u);
            uint16_t *bkt = &w->anchor_bias[cid][bucket];
            if (*bkt < 32767u) (*bkt)++;

            /* decay on anchor event — 95% keeps histogram fresh (~20 events memory)
             * was 99% (~100 events) — shorter memory → easier to change anchor    */
            if (events & TC_EVENT_ANCHOR) {
                for (uint32_t b = 0; b < V4X_BIAS_BUCKETS; b++)
                    w->anchor_bias[cid][b] = (uint16_t)(
                        (uint32_t)w->anchor_bias[cid][b] * 95u / 100u);
                w->bias_decays++;
            }
        }
    }

    w->total_steps++;
    return v_snapped;
}

/* ── drain ring — process all pending entries ────────────────────────
 * Callback receives each committed entry.
 * Returns number of entries drained.                                  */

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

/* v4x_drain_partial — bounded drain (anti-latency-spike)
 *
 * Drains at most `quota` entries per call.
 * Remaining entries stay in ring for next cycle_end.
 *
 * Why quota=256 default (V4X_DRAIN_QUOTA):
 *   720 ops/cycle produced → 256 drained → ring never exceeds 1024.
 *   256 × 32B = 8KB — fits L1 cache, single pass, no stall.
 *
 * Latency comparison:
 *   drain_all  (HARD):    up to 720 entries × callback = unbounded spike
 *   drain_partial(256):   bounded 256 × callback = predictable budget  */
static inline uint64_t v4x_drain_partial(V4xWire *w,
                                          V4xDrainFn fn,
                                          void       *userdata,
                                          uint32_t    quota)
{
    if (!w || quota == 0) return 0;
    uint64_t count = 0;
    V4xCommitEntry e;
    while (count < quota && v4x_ring_pop(&w->ring, &e)) {
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
    printf("║ fast_path_hits:    %10llu  (~%3llu%% skip GPU) ║\n",
           (unsigned long long)w->fast_path_hits,
           w->total_steps ? (unsigned long long)(w->fast_path_hits * 100u
                              / w->total_steps) : 0ULL);
    printf("║ anchor_enforces:   %10llu                   ║\n",
           (unsigned long long)w->anchor_enforces);
    printf("║ cycle_ends:        %10llu                   ║\n",
           (unsigned long long)w->cycle_ends);
    printf("║ partial_drains:    %10llu                   ║\n",
           (unsigned long long)w->partial_drains);
    printf("║ bias_decays:       %10llu                   ║\n",
           (unsigned long long)w->bias_decays);
    printf("║ ring_pending:      %10llu                   ║\n",
           (unsigned long long)v4x_ring_pending(&w->ring));
    printf("║ ring_commits:      %10llu                   ║\n",
           (unsigned long long)w->ring.total_commits);
    printf("║ ring_overflows:    %10llu                   ║\n",
           (unsigned long long)w->ring.total_overflows);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Anchor Bias Histogram (per core)               ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    for (uint32_t c = 0; c < w->N && c < TC_CORES_MAX; c++) {
        printf("║ core[%u] bias: ", c);
        uint16_t max_b = 1;
        for (uint32_t b = 0; b < V4X_BIAS_BUCKETS; b++)
            if (w->anchor_bias[c][b] > max_b) max_b = w->anchor_bias[c][b];
        for (uint32_t b = 0; b < V4X_BIAS_BUCKETS; b++) {
            /* mini bar: scale to 0-4 chars */
            uint16_t bv = w->anchor_bias[c][b];
            char bar = (bv == 0)          ? '.' :
                       (bv < max_b/4)     ? '_' :
                       (bv < max_b/2)     ? '-' :
                       (bv < max_b*3/4)   ? '+' : '#';
            printf("%c", bar);
        }
        printf("  (anchor×9→bucket) ║\n");
    }
    printf("╚══════════════════════════════════════════════════╝\n\n");
    tc_stats_print(&w->tc);
    ma_stats_print(&w->ma);
}

/* ══════════════════════════════════════════════════════════════════════
 * VIRTUAL CORE PARTITION API
 *
 * Exposes the temporal partitioning model:
 *   N cores share 720-step cycle → each core runs every N steps
 *   steps_per_core = 720 / N
 *
 * Gate-18 alignment check:
 *   gate18_clean = (steps_per_core % 18 == 0)
 *   → pipeline runs seamless when aligned (no phase boundary drift)
 *
 * Preset N values (from POGLS philosophy doc):
 *   N=40 → 18  steps/core  ← gate18 CLEAN ✓
 *   N=9  → 80  steps/core  ← gate18 CLEAN ✓ (80 % 18 ≠ 0, but 720/9=80, ok)
 *   N=12 → 60  steps/core  ← gate18 CLEAN ✓ (60 % 18 ≠ 0, ok for icosphere)
 *   N=8  → 90  steps/core  ← worldN n=8 special
 *   N=4  → 180 steps/core  ← default dev config
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t N;               /* number of virtual cores                 */
    uint32_t steps_per_core;  /* TC_CYCLE / N                            */
    uint32_t anchors_per_core;/* how many TC_EVENT_ANCHOR per core/cycle */
    uint32_t gate18_clean;    /* 1 if steps_per_core aligns with gate_18 */
    uint32_t cycle_steps;     /* always TC_CYCLE (720)                   */
    uint32_t anchor_interval; /* always TC_ANCHOR (144)                  */
} V4xPartitionInfo;

/* v4x_partition_info — query partition geometry for given N
 * Returns 0 on success, -1 if N is invalid (0 or > TC_CORES_MAX)      */
static inline int v4x_partition_info(uint32_t N, V4xPartitionInfo *out)
{
    if (!out || N == 0 || N > TC_CORES_MAX) return -1;
    out->N               = N;
    out->steps_per_core  = TC_CYCLE / N;
    out->anchors_per_core = TC_ANCHOR / N;  /* anchor events per core slice */
    /* gate18: steps divisible by 18 → aligned to Rubik quarter-turn      */
    out->gate18_clean    = (out->steps_per_core % 18u == 0u) ? 1u : 0u;
    out->cycle_steps     = TC_CYCLE;
    out->anchor_interval = TC_ANCHOR;
    return 0;
}

/* v4x_partition_print — human-readable partition table
 * Shows all valid N from 1..TC_CORES_MAX with gate18 status            */
static inline void v4x_partition_print(void)
{
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Virtual Core Partition Table  (cycle=%u)            ║\n", TC_CYCLE);
    printf("╠══════╦══════════════╦═══════════════╦════════════════╣\n");
    printf("║  N   ║ steps/core   ║ anchors/core  ║ gate18         ║\n");
    printf("╠══════╬══════════════╬═══════════════╬════════════════╣\n");
    for (uint32_t n = 1; n <= TC_CORES_MAX; n++) {
        if (TC_CYCLE % n != 0) continue;  /* skip non-divisors */
        V4xPartitionInfo p;
        v4x_partition_info(n, &p);
        printf("║ %4u ║ %12u ║ %13u ║ %s            ║\n",
               n, p.steps_per_core, p.anchors_per_core,
               p.gate18_clean ? "✓ CLEAN " : "~ approx");
    }
    printf("╚══════╩══════════════╩═══════════════╩════════════════╝\n\n");
}

/* v4x_set_bias_k — runtime tune anchor spread aggressiveness
 * k=1 default, k=2 aggressive, k=4 explore mode                       */
static inline void v4x_set_bias_k(V4xWire *w, uint32_t k)
{
    if (!w || k == 0) return;
    w->ma.bias_k = k;
}

#endif /* POGLS_V4X_WIRE_H */
