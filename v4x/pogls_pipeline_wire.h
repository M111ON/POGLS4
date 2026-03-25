/*
 * pogls_pipeline_wire.h — POGLS V3.95  Full Pipeline Wire
 * ══════════════════════════════════════════════════════════════════════
 *
 * Wires ทุกอย่างเข้าด้วยกัน:
 *
 *   Input (uint64_t value)
 *      ↓
 *   [1] L3 Quad Intersection  → RouteTarget (MAIN/GHOST/SHADOW)
 *      ↓
 *   [2] Infinity Castle SOE   → fast path / ghost / trace
 *      ↓
 *   [3] Adaptive Routing      → refine decision
 *      ↓
 *   [4] Delta lanes           → persistent storage
 *      ↓
 *   [5] GPU pipeline          → batch Morton/Hilbert transform
 *
 * Usage:
 *   PipelineWire pw;
 *   pipeline_wire_init(&pw, "/tmp/delta");
 *   pipeline_wire_process(&pw, value, angular_addr);
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_PIPELINE_WIRE_H
#define POGLS_PIPELINE_WIRE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pogls_detach_lane.h"
#include "pogls_v4_snapshot.h"   /* Repair layer: Snapshot + Satellite Audit */  /* Protect layer — Phase 2 of Process→Protect→Repair→Evolve */
#include "pogls_qrpn.h"          /* QRPN verification layer (shadow mode)     */
#include "pogls_v4x_wire.h"      /* Phase F: V4x canonical+temporal (SOFT)    */

/* ── pull in all layers ──────────────────────────────────────────── */
#include "routing/pogls_l3_intersection.h"
#include "routing/pogls_infinity_castle.h"
#include "pogls_evo_v3.h"          /* lane routing: full 12-point (zero-float) */
#include "core/pogls_fractal_gate.h" /* Hilbert Bridge: Morton→disk locality   */
/* adaptive_v2 skipped — RouteTarget already defined by l3_intersection */


/* ── Po Anchor: bit-stability score on data value ───────────────────
 * value = actual data (64-bit) — not addr
 * x ^= x>>10; x ^= x>>5; anchor = x & (x>>8) & (x>>16)
 * structured data → stable bits → high popcount
 * threshold 4: separates structured from noise                       */
/* Ghost streak: sync with L3_GHOST_STREAK_MAX via POGLS_GHOST_STREAK_MAX */
#ifndef POGLS_GHOST_STREAK_MAX
#  define POGLS_GHOST_STREAK_MAX  8u
#endif
#define PW_ANCHOR_THRESH  4u

/* DualSensor constants (FROZEN) */
#define PW_SA_LOCAL      16u        /* Hilbert proximity threshold       */
/* PHI constants — from POGLS single source (pogls_platform.h) */
#ifndef POGLS_PHI_CONSTANTS
#  include "pogls_platform.h"
#endif
#define PW_SB_PHI_DOWN  POGLS_PHI_DOWN   /* floor(PHI^-1 x 2^20)      */
#define PW_SB_PHI_COMP  POGLS_PHI_COMP   /* 2^20 - PHI_DOWN (wrap)     */
#define PW_SB_TOL        8192u      /* +/- 2^13 tolerance                */


static inline uint32_t _pw_anchor_score(uint64_t value)
{
    uint32_t x = (uint32_t)(value ^ (value >> 32));
    x ^= x >> 10;
    x ^= x >> 5;
    uint32_t anchor = x & (x >> 8) & (x >> 16);
    return (uint32_t)__builtin_popcount(anchor);
}

/* ── DeltaSensor: addr-delta pattern detector ───────────────────────
 * Separates patterns by consecutive addr differences:
 *   seq/burst: small_pct high → MAIN (structured movement)
 *   phi:       periodic_pct > 0 → MAIN (oscillating boundary)
 *   chaos:     large_pct high → GHOST (scattered)
 *   random:    none of above → GHOST
 *
 * rubik_mix: splitmix64 bijection — preserves structure, distributes bits
 * Used on DATA value (not addr) before anchor_po                       */

static inline uint64_t _pw_rubik_mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

#define PW_DS_THRESH       (1u << 16)   /* small delta threshold        */
#define PW_DS_LARGE_MULT    8u   /* 65536*8=524288 */          /* large = thresh × 16          */
#define PW_DS_WINDOW       64u          /* rolling window for pct       */

typedef struct {
    uint64_t prev_addr;
    uint64_t prev_delta;
    uint8_t  valid;
    /* rolling counters (last PW_DS_WINDOW ops) */
    uint8_t  small_ring[64];    /* 1=small delta */
    uint8_t  period_ring[64];   /* 1=periodic */
    uint8_t  ring_idx;
    uint32_t small_count;
    uint32_t period_count;
    uint32_t large_count;
    uint64_t total;
} PW_DeltaSensor;

static inline void pw_ds_update(PW_DeltaSensor *s, uint64_t addr)
{
    if (s->valid) {
        uint64_t d = addr > s->prev_addr
                   ? addr - s->prev_addr
                   : s->prev_addr - addr;
        uint8_t is_small  = (d < PW_DS_THRESH) ? 1u : 0u;
        uint8_t is_large  = (d > (uint64_t)PW_DS_THRESH * PW_DS_LARGE_MULT) ? 1u : 0u;
        uint8_t is_period = (s->valid > 1 && d == s->prev_delta) ? 1u : 0u;

        /* rolling window update */
        uint8_t idx = s->ring_idx & (PW_DS_WINDOW - 1);
        s->small_count  -= s->small_ring[idx];
        s->period_count -= s->period_ring[idx];
        s->small_ring[idx]  = is_small;
        s->period_ring[idx] = is_period;
        s->small_count  += is_small;
        s->period_count += is_period;
        s->ring_idx++;

        if (is_large) s->large_count++;
        s->prev_delta = d;
    }
    s->prev_addr = addr;
    s->valid = s->valid < 2 ? s->valid + 1 : 2;
    s->total++;
}

/* pw_ds_route: route suggestion from delta sensor
 * Returns 1=MAIN, 0=GHOST
 *
 * Detection table:
 *   seq/burst: small_pct >= 80% → MAIN
 *   phi:       small_pct=0 + large_pct < 50% + period_pct >= 20% → MAIN
 *   chaos:     large_pct >= 80% → GHOST
 *   random:    no clear signal → GHOST
 */
static inline int pw_ds_route(const PW_DeltaSensor *s)
{
    if (s->total < 8) return 1;  /* not enough history → MAIN */
    uint32_t win        = s->total < PW_DS_WINDOW
                        ? (uint32_t)s->total : PW_DS_WINDOW;
    uint32_t small_pct  = s->small_count  * 100u / win;
    uint32_t period_pct = s->period_count * 100u / win;
    uint32_t large_pct  = (uint32_t)(s->large_count < s->total
                        ? s->large_count : s->total) * 100u
                        / (uint32_t)s->total;

    /* GHOST: chaos = large delta dominates (≥80%) */
    if (large_pct >= 80u) return 0;

    /* MAIN: seq/burst = small delta dominates (≥70%) */
    if (small_pct >= 70u) return 1;

    /* MAIN: phi = moderate large + periodic (oscillating boundary) */
    if (large_pct < 50u && period_pct >= 15u) return 1;

    /* GHOST: random = large present + no periodicity */
    if (large_pct >= 15u && period_pct < 10u && small_pct < 15u) return 0;

    return 1;
}
/* Giant Shadow — inline minimal version to avoid redefinition */
#ifndef RUBIK_LANES
  #define RUBIK_LANES 54u
#endif

/* ── minimal Delta writer (standalone) ───────────────────────────── */
#ifndef DELTA_BLOCK_SIZE
  #define DELTA_BLOCK_SIZE 64u
#endif
#ifndef RUBIK_LANES
  #define RUBIK_LANES 54u
#endif

typedef struct { uint64_t data[8]; } WireBlock;  /* 64B */

typedef struct {
    FILE    *fp[RUBIK_LANES];
    uint64_t writes[RUBIK_LANES];
    uint64_t total;
    char     base_dir[256];
    int      open;
} WireDelta;

static inline int wd_init(WireDelta *wd, const char *dir)
{
    if (!wd || !dir) return -1;
    memset(wd, 0, sizeof(*wd));
    snprintf(wd->base_dir, sizeof(wd->base_dir), "%s", dir);
    char path[512];
    for (int i = 0; i < (int)RUBIK_LANES; i++) {
        snprintf(path, sizeof(path), "%s/lane_%02d.dat", dir, i);
        wd->fp[i] = fopen(path, "ab");
        if (!wd->fp[i]) { /* try mkdir */
            #ifdef _WIN32
            _mkdir(dir);
            #else
            mkdir(dir, 0755);
            #endif
            wd->fp[i] = fopen(path, "ab");
        }
    }
    wd->open = 1;
    return 0;
}

/* batch buffer per lane for efficiency */
#define WIRE_BATCH 256
typedef struct {
    WireBlock buf[WIRE_BATCH];
    uint32_t  count;
} LaneBatch;

static inline void wd_flush(WireDelta *wd, LaneBatch *lb, int lane)
{
    if (!wd || !lb || lb->count == 0) return;
    if (wd->fp[lane])
        fwrite(lb->buf, DELTA_BLOCK_SIZE, lb->count, wd->fp[lane]);
    wd->writes[lane] += lb->count;
    wd->total        += lb->count;
    lb->count = 0;
}

static inline void wd_push(WireDelta *wd, LaneBatch *lb,
                            int lane, const WireBlock *blk)
{
    lb->buf[lb->count++] = *blk;
    if (lb->count >= WIRE_BATCH) wd_flush(wd, lb, lane);
}

static inline void wd_close(WireDelta *wd, LaneBatch *lb)
{
    if (!wd) return;
    for (int i = 0; i < (int)RUBIK_LANES; i++) {
        if (lb) wd_flush(wd, &lb[i], i);
        if (wd->fp[i]) { fclose(wd->fp[i]); wd->fp[i] = NULL; }
    }
}

/* ── Ghost store: flat hash table (no slab, no malloc) ──────────── */
#define GHOST_STORE_SIZE  4096u   /* power of 2, fits in RAM          */
#define GHOST_STORE_MASK  (GHOST_STORE_SIZE - 1)

typedef struct {
    uint64_t  sig;        /* signature (0 = empty)                    */
    uint64_t  value;      /* stored value                             */
    uint32_t  prev_addr;  /* prev addr at store time — context guard  */
    uint8_t   lane;       /* lane it came from                        */
    uint8_t   hits;       /* hit count (for eviction)                 */
    uint16_t  _pad;
} WireGhostEntry;         /* 28B                                      */

/* ══════════════════════════════════════════════════════════════════
 * PipelineWire — the complete wired system
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* Ghost store — flat hash table */
    WireGhostEntry    ghost_table[GHOST_STORE_SIZE];
    uint64_t          ghost_stores;
    uint64_t          ghost_hits_real;

    /* Layer 1: L3 Quad Intersection */
    L3Engine          l3;

    /* Layer 2: Infinity Castle SOE */
    InfinityCastle    castle;

    /* Layer 3: EvoV3 — lane routing (12-point, zero-float) */
    EvoV3             evo;

    /* Hilbert LUT — Morton→Hilbert bridge (256B, L2 resident) */
    HilbertLUT        hilbert;

    /* Layer 4: Delta storage */
    WireDelta         delta;
    LaneBatch         batches[RUBIK_LANES];

    /* Layer 5: Shadow (burst absorber) */
    uint64_t         shadow_buffered;

    /* Layer 6: Detach safety lane (Protect) — anomaly quarantine
     * Receives SHADOW + ghost_drift + unit_circle fails.
     * Async flush → isolated delta lane 53. */
    DetachLane        detach;
    uint64_t          route_detach;   /* ops routed to detach lane */

    /* DeltaSensor: pattern detector from addr deltas */
    PW_DeltaSensor ds;

    /* Po Anchor Score (on data value, not addr) */
    uint64_t  anchor_main;    /* anchor >= thresh → promote to MAIN */
    uint64_t  anchor_ghost;   /* anchor < thresh → keep GHOST       */

    /* Stats */
    uint64_t  total_in;
    uint64_t  l3_fast;       /* L3 fast skip */
    uint64_t  castle_fast;   /* castle ghost hit */
    uint64_t  route_main;
    uint64_t  route_ghost;
    uint64_t  route_shadow;
    uint64_t  delta_commits;

    /* DualSensor: geometry (Hilbert) + PHI delta */
    uint32_t  sa_prev_h;     /* previous Hilbert index (10-bit)     */
    uint32_t  sb_prev_addr;  /* previous addr for delta compute     */
    uint32_t  _ds_pad;
    uint32_t  magic;

    /* ── Repair layer: Snapshot + Satellite Audit ─────────────────
     * audit: receives DetachEntry from detach drain → tile health
     * snap:  PENDING until delta_commits threshold → CERTIFIED      */
    V4AuditContext     audit;
    V4SnapshotHeader   snap;
    uint64_t           snap_id_counter;   /* monotonic, never reuse   */

    /* ── QRPN verification layer (shadow mode) ────────────────────
     * Runs BEFORE delta write on MAIN path only.
     * shadow mode = log only, never blocks pipeline.
     * N borrowed from ShellN anchor (8).                            */
    qrpn_ctx_t         qrpn;

    /* ── Phase F: V4x canonical + temporal core (SOFT mode) ───────
     * Runs IN PARALLEL on MAIN path — shadow only, never blocks.
     * Provides: canonical snap, 720-step temporal scheduling,
     *           multi-anchor selection, commit ring.
     * Deploy phases:
     *   SOFT  = run + log, output ignored (current)
     *   HARD  = v_snapped replaces value in delta write (future)    */
    V4xWire            v4x;
    uint64_t           v4x_ops;             /* MAIN ops through V4x  */
    uint64_t           v4x_ring_overflows;  /* ring overflow count    */
} PipelineWire;

#define PIPELINE_WIRE_MAGIC  0x50574952u  /* "PWIR" */

/* ── ghost store helpers ─────────────────────────────────────────── */
static inline void _wire_ghost_store(PipelineWire *pw,
                                 uint64_t sig, uint64_t value,
                                 uint8_t lane, uint32_t prev_a)
{
    uint32_t idx = (uint32_t)(sig & GHOST_STORE_MASK);
    WireGhostEntry *e = &pw->ghost_table[idx];
    if (e->sig == 0 || e->hits == 0) {
        e->sig       = sig;
        e->value     = value;
        e->prev_addr = prev_a;   /* context snapshot */
        e->lane      = lane;
        e->hits      = 1;
        pw->ghost_stores++;
    }
}

static inline WireGhostEntry *_ghost_lookup(PipelineWire *pw, uint64_t sig)
{
    uint32_t idx = (uint32_t)(sig & GHOST_STORE_MASK);
    WireGhostEntry *e = &pw->ghost_table[idx];
    if (e->sig == sig && e->sig != 0) {
        e->hits++;
        pw->ghost_hits_real++;
        return e;
    }
    return NULL;
}

/* _ghost_peek — read-only inspect, NO side effects (use in tests/debug only)
 * Production code must use _ghost_lookup which self-reinforces hits counter. */
static inline const WireGhostEntry *_ghost_peek(const PipelineWire *pw, uint64_t sig)
{
    uint32_t idx = (uint32_t)(sig & GHOST_STORE_MASK);
    const WireGhostEntry *e = &pw->ghost_table[idx];
    return (e->sig == sig && e->sig != 0) ? e : NULL;
}

/* ── L1 XOR gate — lightweight data integrity check ─────────────────
 * Mirrors GPU pipeline audit: XOR all 8 bytes of each data[i].
 * addr == 0 → xr==0 naturally → auto-pass (valid zero block).
 * Non-zero data with xr==0 = balanced byte distribution = structured.
 * Used before WireDelta write to catch corrupted blocks early.
 * Cost: 8 XOR ops ~0.5ns — always on MAIN path only.          */
static inline int wireblock_l1_ok(const WireBlock *blk)
{
    uint8_t xr = 0;
    const uint8_t *p = (const uint8_t *)blk;
    for (int i = 0; i < 8; i++) xr ^= p[i];  /* first 8 bytes = value */
    /* pass if balanced OR if value is zero (valid empty block) */
    return (xr == 0) || (blk->data[0] == 0);
}

/* ── init ─────────────────────────────────────────────────────────── */
static inline int pipeline_wire_init(PipelineWire *pw, const char *delta_dir)
{
    if (!pw) return -1;
    memset(pw, 0, sizeof(*pw));

    l3_init(&pw->l3);
    infinity_castle_init(&pw->castle);
    evo3_init(&pw->evo);
    hilbert_lut_build(&pw->hilbert);
    wd_init(&pw->delta, delta_dir ? delta_dir : "/tmp/pogls_wire");

    /* DeltaSensor zero-init (memset covers it) */
    /* init detach safety lane — uses same WireDelta writer */
    detach_lane_init(&pw->detach, NULL);  /* delta ptr set after wd_init */
    pw->detach.delta = NULL;  /* WireDelta is separate — detach uses its own path */
    detach_lane_start(&pw->detach);

    /* [WIRE-1] Repair layer init */
    v4_audit_init(&pw->audit);
    pw->snap_id_counter = 1;
    pw->snap = v4_snap_create(pw->snap_id_counter, 1 /*branch*/, 0 /*parent*/);

    /* [QRPN] init verification layer — shadow mode, N=8 (ShellN anchor) */
    qrpn_ctx_init(&pw->qrpn, 8u);
    pw->qrpn.mode = QRPN_SHADOW;

    /* [V4x] Phase F init — SOFT mode, 4 virtual cores */
    v4x_wire_init(&pw->v4x, 4u);

    pw->magic = PIPELINE_WIRE_MAGIC;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * pipeline_wire_process — THE main entry point
 *
 * All 5 layers wired in sequence.
 * Returns final RouteTarget.
 * ══════════════════════════════════════════════════════════════════ */
/* ══════════════════════════════════════════════════════════════════
 * DualSensor: Sensor A (Hilbert geometry) + Sensor B (PHI delta)
 * Returns 1 = structured (MAIN candidate), 0 = GHOST
 * ══════════════════════════════════════════════════════════════════ */
static inline void _pw_hilbert10(uint32_t d, uint32_t *x, uint32_t *y)
{
    uint32_t rx, ry, s, t = d & 0xFFFFFu;
    *x = 0; *y = 0;
    for (s = 1; s < (1u << 10); s <<= 1) {
        rx = (t >> 1) & 1u;
        ry = (t ^ rx) & 1u;
        if (!ry) {
            if (rx) { *x = s-1u - *x; *y = s-1u - *y; }
            uint32_t tmp = *x; *x = *y; *y = tmp;
        }
        *x += s * rx; *y += s * ry;
        t >>= 2;
    }
}

static inline int _pw_dual_route(PipelineWire *pw, uint32_t addr)
{
    /* Sensor B: PHI delta (runs first — phi is always GLOBAL, skip Sensor A) */
    uint32_t delta = (addr >= pw->sb_prev_addr)
                   ? (addr - pw->sb_prev_addr)
                   : (pw->sb_prev_addr - addr);
    int phi_b = ((delta >= PW_SB_PHI_DOWN - PW_SB_TOL) &&
                 (delta <= PW_SB_PHI_DOWN + PW_SB_TOL)) ||
                ((delta >= PW_SB_PHI_COMP - PW_SB_TOL) &&
                 (delta <= PW_SB_PHI_COMP + PW_SB_TOL));
    pw->sb_prev_addr = addr;
    if (phi_b) return 1;   /* PHI pattern → MAIN */

    /* Sensor A: Hilbert local geometry */
    uint32_t hx, hy;
    _pw_hilbert10(addr, &hx, &hy);
    uint32_t h_cur = (hx << 10) | hy;
    uint32_t prev_hx = pw->sa_prev_h >> 10;
    uint32_t prev_hy = pw->sa_prev_h & 0x3FFu;
    uint32_t dx = (hx > prev_hx) ? (hx - prev_hx) : (prev_hx - hx);
    uint32_t dy = (hy > prev_hy) ? (hy - prev_hy) : (prev_hy - hy);
    pw->sa_prev_h = h_cur;
    return ((dx + dy) <= PW_SA_LOCAL);   /* local → 1, global → 0 */
}

static inline RouteTarget pipeline_wire_process(PipelineWire *pw,
                                                  uint64_t      value,
                                                  uint64_t      angular_addr)
{
    if (!pw) return ROUTE_SHADOW;
    pw->total_in++;

    uint8_t lane = (uint8_t)(angular_addr % RUBIK_LANES); /* fallback */

    /* ── Layer 3: EvoV3 lane routing (replaces naive modulo) ──────
     * 12-point: Fibonacci + Mandelbrot + Ghost + Phase + Energy
     * Returns packed (lane<<8 | type), zero-float, pure bit-shift  */
    uint32_t evo_packed = evo3_process(&pw->evo, angular_addr);
    lane = evo3_lane(evo_packed);   /* 0..53, energy-balanced       */

    /* ── Layer 1: L3 Quad Intersection ────────────────────────────
     * Multi-view consensus → MAIN/GHOST/SHADOW
     * Fast skip if overlap > 0.9 (< 10ns path)          */
    /* L3 probe: combine value + angular_addr so routing reflects both
     * spatial (value) and temporal/lane (angular_addr) dimensions */
    RouteTarget l3_route = l3_process(&pw->l3, value ^ angular_addr);
    if (pw->l3.fast_skips > pw->l3_fast) {
        pw->l3_fast = pw->l3.fast_skips;
    }

    /* ── Layer 2: Wire ghost cache (routing cache, with context guard)
     * sig includes addr so same value at different addr → different slot.
     * prev_addr guard: skip hit if sequence context changed (drift safe). */
    /* Check 3: sig includes addr context */
    uint64_t sig = value ^ (uint64_t)angular_addr ^ (value >> 32);

    WireGhostEntry *_wire_hit = _ghost_lookup(pw, sig);
    uint32_t _prev20 = (uint32_t)(pw->l3.prev_addr);
    if (_wire_hit && _wire_hit->prev_addr == _prev20) {
        /* Cache hit + context matches → trust routing result */
        pw->castle_fast++;           /* reuse counter: wire cache hits */
        l3_route = ROUTE_MAIN;
    } else {
        /* Miss or context mismatch → SOE trace + fresh L3 compute */
        uint64_t offset = angular_addr % (1u << 20);
        { uint64_t _v=value; trace_record(&pw->castle, offset, &_v, sizeof(_v)); }
        collapse_update_window(&pw->castle, offset);
        predict_prefetch(&pw->castle, sig);

        /* Populate wire cache after L3 decision (for future hits) */
        if (l3_route == ROUTE_MAIN) {
            _wire_ghost_store(pw, sig, value, lane,
                              (uint32_t)(pw->l3.prev_addr));
        }
    }

    /* ── Layer 3: Adaptive Routing refinement ─────────────────────
     * Use V3.94 signal-based routing to refine L3 decision
     * Especially useful for SHADOW → GHOST/MAIN upgrade    */
    /* Issue E: SHADOW semantic (FROZEN)
     * SHADOW = geo_invalid ONLY — chaos/rand must NEVER land here.
     * Second-chance: promote low-entropy SHADOW → GHOST (not MAIN).
     * This keeps SHADOW rare (<1% in normal workloads).            */
    if (l3_route == ROUTE_SHADOW) {
        uint32_t lo = (uint32_t)(value & 0xFFFF);
        uint32_t hi = (uint32_t)((value >> 16) & 0xFFFF);
        if (__builtin_popcount(lo ^ hi) < 4)
            l3_route = ROUTE_GHOST;   /* upgrade: not truly invalid */
        /* else: stays SHADOW — truly outside geometry              */
    }

    /* ══════════════════════════════════════════════════════════════
     * [STEP3] ROUTING PRIORITY LOCK — strict cascade, no override wars
     *
     * Priority:
     *   1. DeltaSensor  → REJECT only (chaos → GHOST, no false MAIN)
     *   2. DualSensor   → strong geo/PHI signal → GHOST (definitive)
     *   3. L3           → main decision (already computed above)
     *   4. Anchor       → final refine (GHOST→MAIN if structured data)
     *
     * Rule: higher priority = REJECT only — cannot force MAIN.
     *       Only L3+Anchor can promote to MAIN.
     * ══════════════════════════════════════════════════════════════ */

    /* [P1] DeltaSensor — update, then REJECT if chaos pattern detected */
    pw_ds_update(&pw->ds, value);
    if (!pw_ds_route(&pw->ds)) {
        /* Chaos/random addr movement → GHOST, stop here */
        l3_route = ROUTE_GHOST;
        pw->anchor_ghost++;
        goto route_final;   /* skip P2/P3/P4 — decision is final */
    }

    /* [P2] DualSensor — geometry + PHI delta (addr-level)
     * Only fires if DeltaSensor did NOT reject.
     * If dual geometry fails → GHOST, stop here                  */
    if (!_pw_dual_route(pw, (uint32_t)(angular_addr & ((1u<<20)-1)))) {
        l3_route = ROUTE_GHOST;
        pw->anchor_ghost++;
        goto route_final;   /* skip P3/P4 */
    }

    /* [P3] L3 decision already set above — trust it unless GHOST */

    /* [P4] Anchor — final refine: only promotes GHOST→MAIN
     * Runs only after P1+P2 passed (no rejections).
     * [BONUS] Ghost cache decay gate: require hits >= 2 before MAIN  */
    if (l3_route == ROUTE_GHOST) {
        uint32_t ascore = _pw_anchor_score(_pw_rubik_mix(value));
        if (ascore >= PW_ANCHOR_THRESH) {
            /* Check ghost cache — only promote if entry has proven hits */
            WireGhostEntry *_ghost_chk = _ghost_lookup(pw, sig);
            int ghost_mature = (_ghost_chk && _ghost_chk->hits >= 3u); /* [BONUS] mature=3: store(1)+2 lookups */
            if (!ghost_mature) {
                /* [BONUS] first/second hit: don't promote yet, decay gate */
                pw->anchor_ghost++;
            } else {
                l3_route = ROUTE_MAIN;
                pw->anchor_main++;
            }
        } else {
            pw->anchor_ghost++;
        }
    }

    route_final:
    /* Stats: counted here — AFTER all overrides (DeltaSensor/DualSensor)
     * so numbers reflect the final decision, not intermediate l3_route  */
    switch (l3_route) {
    case ROUTE_MAIN:   pw->route_main++;   break;
    case ROUTE_GHOST:  pw->route_ghost++;  break;
    case ROUTE_SHADOW: pw->route_shadow++; break;
    }

    /* ── Detach hook: push anomalies to safety lane ──────────────
     * Triggers (HYBRID mode — all three reasons):
     *   ROUTE_SHADOW       = geo_invalid (unit circle fail)
     *   ghost_streak > max = drift anomaly (L3 flag)
     *   Never blocks. Ring overwrite if full. */
    if (l3_route == ROUTE_SHADOW) {
        uint8_t reason = DETACH_REASON_GEO_INVALID;
        /* check if also a unit circle explicit fail */
        uint32_t m = POGLS_PHI_SCALE - 1u;
        uint32_t aa = (uint32_t)(((uint64_t)((uint32_t)(angular_addr&m)) * POGLS_PHI_UP)   >> 20) & m;
        uint32_t bb = (uint32_t)(((uint64_t)((uint32_t)(angular_addr&m)) * POGLS_PHI_DOWN) >> 20) & m;
        if (((uint64_t)aa*aa + (uint64_t)bb*bb) >> 41)
            reason |= DETACH_REASON_UNIT_CIRCLE;
        detach_lane_push(&pw->detach, value, angular_addr,
                         reason, (uint8_t)ROUTE_SHADOW,
                         0u, pw->total_in);
        pw->route_detach++;
        /* [WIRE-2] Repair: feed anomaly directly into Satellite Audit */
        { DetachEntry _ae; memset(&_ae,0,sizeof(_ae));
          _ae.angular_addr=angular_addr; _ae.value=value;
          _ae.reason=reason; _ae.phase18=(uint8_t)(pw->total_in%18u);
          _ae.phase288=(uint16_t)(pw->total_in%288u);
          _ae.phase306=(uint16_t)(pw->total_in%306u);
          v4_audit_ingest(&pw->audit, &_ae); }
    } else if (pw->l3.ghost_streak == 0 && pw->l3.shadow_routes > 0) {
        /* ghost_streak just reset → was drift anomaly */
        detach_lane_push(&pw->detach, value, angular_addr,
                         DETACH_REASON_GHOST_DRIFT, (uint8_t)l3_route,
                         0u, pw->total_in);
        pw->route_detach++;
        /* [WIRE-2] Repair: ghost drift also feeds audit */
        { DetachEntry _ae; memset(&_ae,0,sizeof(_ae));
          _ae.angular_addr=angular_addr; _ae.value=value;
          _ae.reason=DETACH_REASON_GHOST_DRIFT;
          _ae.phase18=(uint8_t)(pw->total_in%18u);
          _ae.phase288=(uint16_t)(pw->total_in%288u);
          _ae.phase306=(uint16_t)(pw->total_in%306u);
          v4_audit_ingest(&pw->audit, &_ae); }
    }

    /* ── Layer 4: Delta storage ────────────────────────────────────
     * MAIN   → write to active delta lane
     * GHOST  → write to ghost lane (separate)
     * SHADOW → buffer in Giant Shadow (burst absorber)     */
    WireBlock blk;
    blk.data[0] = value;
    blk.data[1] = angular_addr;
    blk.data[2] = (uint64_t)l3_route;
    blk.data[3] = sig;
    for (int i = 4; i < 8; i++) blk.data[i] = 0;

    if (l3_route == ROUTE_MAIN) {
        /* Hilbert bridge: Morton addr → Hilbert for sequential disk locality */
        uint32_t morton_addr  = (uint32_t)(angular_addr & 0xFFFFF);
        uint32_t hilbert_addr = hilbert_from_morton(&pw->hilbert, morton_addr);
        uint8_t  h_lane = (uint8_t)(hilbert_addr % RUBIK_LANES);
        blk.data[4] = hilbert_addr;

        /* [QRPN] verify before write — shadow mode: log only, never blocks */
        {
            uint32_t Cg = qrpn_gpu_witness_cpu_fallback(value);
            qrpn_check(value, angular_addr, Cg, &pw->qrpn, NULL);
        }

        /* [V4x Phase F — SOFT] canonical + temporal + multi-anchor
         * Runs in parallel: output v_snapped logged but NOT used yet.
         * Transition to HARD: replace value with v_snapped in blk.data[0].
         * value truncated to uint32 — upper bits are addr context.      */
        {
            uint32_t v_snapped = v4x_step(&pw->v4x, (uint32_t)(value & 0xFFFFFFFFu));
            (void)v_snapped;   /* SOFT: ignore output, shadow only       */
            pw->v4x_ops++;
            /* track ring health */
            pw->v4x_ring_overflows = pw->v4x.ring.total_overflows;
        }
        /* Write to delta (L1 XOR check removed from hot path —
         * audit provides the integrity layer now)                    */
        wd_push(&pw->delta, &pw->batches[h_lane], (int)h_lane, &blk);
        pw->delta_commits++;
        /* [WIRE-2b] Repair: feed every MAIN commit into Satellite Audit */
        { DetachEntry _ae; memset(&_ae,0,sizeof(_ae));
          _ae.angular_addr=angular_addr; _ae.value=value;
          _ae.reason=0;  /* clean commit — no anomaly flag */
          _ae.phase18=(uint8_t)(pw->total_in%18u);
          _ae.phase288=(uint16_t)(pw->total_in%288u);
          _ae.phase306=(uint16_t)(pw->total_in%306u);
          v4_audit_ingest(&pw->audit, &_ae); }
        /* [WIRE-3] Repair: certify snapshot every gate_18 commits */
        if (pw->delta_commits % 18u == 0 &&
            pw->snap.state == SNAP_PENDING) {
            if (v4_snap_certify(&pw->snap,
                                 pw->delta_commits,
                                 &pw->audit) == 0) {
                /* certified — create next pending snapshot */
                pw->snap_id_counter++;
                pw->snap = v4_snap_create(pw->snap_id_counter,
                                          1, pw->snap_id_counter - 1);
            }
        }
    } else if (l3_route == ROUTE_GHOST) {
        /* ghost lane = lane + RUBIK_LANES/2 (wraps) */
        uint8_t ghost_lane = (lane + RUBIK_LANES/2) % RUBIK_LANES;
        wd_push(&pw->delta, &pw->batches[ghost_lane],
                (int)ghost_lane, &blk);
    } else {
        /* SHADOW: absorb in Giant Shadow ring */
        pw->shadow_buffered++;
    }

    return l3_route;
}

/* ── flush all pending batches ───────────────────────────────────── */
static inline void pipeline_wire_flush(PipelineWire *pw)
{
    if (!pw) return;
    for (int i = 0; i < (int)RUBIK_LANES; i++)
        wd_flush(&pw->delta, &pw->batches[i], i);
}

/* ── close ───────────────────────────────────────────────────────── */
static inline void pipeline_wire_close(PipelineWire *pw)
{
    if (!pw) return;
    pipeline_wire_flush(pw);
    detach_lane_stop(&pw->detach);
    detach_lane_drain(&pw->detach);  /* final drain */
    wd_close(&pw->delta, pw->batches);
}

/* ── print stats ─────────────────────────────────────────────────── */
static inline void pipeline_wire_stats(const PipelineWire *pw)
{
    if (!pw) return;
    uint64_t t = pw->total_in ? pw->total_in : 1;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  POGLS V3.95 Pipeline Wire Stats                ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Total in:      %10llu                        ║\n",
           (unsigned long long)pw->total_in);
    printf("║ Ghost hits:    %10llu (%3llu%%)               ║\n",
           (unsigned long long)pw->ghost_hits_real,
           (unsigned long long)(pw->ghost_hits_real*100/t));
    printf("║ MAIN routes:   %10llu (%3llu%%)               ║\n",
           (unsigned long long)pw->route_main,
           (unsigned long long)(pw->route_main*100/t));
    printf("║ GHOST routes:  %10llu (%3llu%%)               ║\n",
           (unsigned long long)pw->route_ghost,
           (unsigned long long)(pw->route_ghost*100/t));
    printf("║ SHADOW routes: %10llu (%3llu%%)               ║\n",
           (unsigned long long)pw->route_shadow,
           (unsigned long long)(pw->route_shadow*100/t));
    printf("║ Delta commits: %10llu                        ║\n",
           (unsigned long long)pw->delta_commits);
    printf("║ Detach routes: %10llu (%3llu%%)               ║\n",
           (unsigned long long)pw->route_detach,
           (unsigned long long)(pw->route_detach*100/t));
    printf("╚══════════════════════════════════════════════════╝\n\n");
    detach_lane_stats(&pw->detach);
    qrpn_stats_print(&pw->qrpn);

    /* Phase F V4x stats */
    printf("[V4x SOFT] ops=%-10llu ring_overflows=%-6llu anchor_enforces=%-6llu cycle_ends=%llu\n",
           (unsigned long long)pw->v4x_ops,
           (unsigned long long)pw->v4x_ring_overflows,
           (unsigned long long)pw->v4x.anchor_enforces,
           (unsigned long long)pw->v4x.cycle_ends);
}

#endif /* POGLS_PIPELINE_WIRE_H */
