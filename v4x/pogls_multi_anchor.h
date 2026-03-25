/*
 * pogls_multi_anchor.h — POGLS V4.x Multi-Anchor System
 * ══════════════════════════════════════════════════════════════════════
 *
 * Step 2: Dynamic anchor selection from {72, 144, 288, 360}
 *
 * All anchors are multiples of 18 (gate_18 aligned):
 *   72  = 18×4  = fast cycle  (burst mode)
 *   144 = 18×8  = Fib(12) spatial lock  ← default strong
 *   288 = 18×16 = double anchor
 *   360 = 18×20 = full half-cycle
 *
 * Score function:
 *   drift_error    = |state_hash ^ canonical_hash| normalized
 *   distortion_cost= how far v_clean is from anchor grid alignment
 *   score(a)       = drift_error × alpha + distortion_cost × (1-alpha)
 *   best_anchor    = argmin(score)
 *
 * Soft snap:
 *   alpha = 0.3 (CPU phase) → 1.0 (GPU anchor phase)
 *   v_snapped = lerp(v_clean, snap_to_anchor(v_clean, a), alpha)
 *   (integer lerp: no float needed)
 *
 * Integration:
 *   tc_dispatch() → TC_EVENT_ANCHOR → ma_select_best() → update anchor
 *   TC_EVENT_CYCLE_END → ma_decay_scores()  (slow adaptation)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_MULTI_ANCHOR_H
#define POGLS_MULTI_ANCHOR_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>

#include "pogls_temporal_core.h"   /* TCFabric, TC_ANCHOR, TC_CYCLE */

/* ── anchor set (all multiples of 18, frozen) ─────────────────────── */
#define MA_N_ANCHORS    4u
#define MA_ANCHOR_72    72u
#define MA_ANCHOR_144   144u    /* default = TC_ANCHOR */
#define MA_ANCHOR_288   288u
#define MA_ANCHOR_360   360u

static const uint32_t MA_ANCHORS[MA_N_ANCHORS] = {
    MA_ANCHOR_72, MA_ANCHOR_144, MA_ANCHOR_288, MA_ANCHOR_360
};

/* compile-time: all multiples of 18 */
typedef char _ma_a72 [(MA_ANCHOR_72  % 18u == 0) ? 1 : -1];
typedef char _ma_a144[(MA_ANCHOR_144 % 18u == 0) ? 1 : -1];
typedef char _ma_a288[(MA_ANCHOR_288 % 18u == 0) ? 1 : -1];
typedef char _ma_a360[(MA_ANCHOR_360 % 18u == 0) ? 1 : -1];
/* all fit within TC_CYCLE */
typedef char _ma_fit [(MA_ANCHOR_360 < TC_CYCLE)  ? 1 : -1];

/* alpha scale: 0..256 (integer, 256 = 1.0) */
#define MA_ALPHA_CPU    77u     /* 0.3 × 256 ≈ 77  */
#define MA_ALPHA_GPU    256u    /* 1.0 × 256 = 256 */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — ANCHOR CONTEXT (per virtual core)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  anchor;           /* current best anchor                  */
    uint32_t  anchor_idx;       /* index into MA_ANCHORS[]              */
    uint32_t  alpha;            /* current alpha (77=CPU, 256=GPU)      */
    uint64_t  score[MA_N_ANCHORS]; /* cumulative score per anchor       */
    uint64_t  select_count;     /* how many times best was chosen       */
    uint32_t  _pad;
} MAAnchorCtx;

static inline void ma_ctx_init(MAAnchorCtx *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    /* default: anchor_144 (index 1) */
    ctx->anchor     = MA_ANCHOR_144;
    ctx->anchor_idx = 1u;
    ctx->alpha      = MA_ALPHA_CPU;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — SCORE FUNCTIONS
 *
 * distortion_cost(v, a):
 *   d = v % a   (distance from nearest anchor grid point)
 *   cost = min(d, a - d)   ← nearest-neighbor distance on circle
 *   normalized: cost × 1024 / a  (fixed-point, max = 512)
 *
 * drift_error(state_hash, v_clean, a):
 *   expected = (v_clean / a) * a   ← floor to anchor grid
 *   error = |state_hash_low - expected|
 *   normalized: error & 0xFFFF     ← 16-bit window
 *
 * score = drift × alpha + distortion × (256 - alpha)   / 256
 *       all integer, no float
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t ma_distortion(uint32_t v, uint32_t anchor)
{
    if (anchor == 0) return 0;
    uint32_t d = v % anchor;
    uint32_t near = (d < anchor - d) ? d : (anchor - d);
    /* normalize to 0..512 */
    return (near * 1024u) / anchor;
}

static inline uint32_t ma_drift_error(uint64_t state_hash,
                                       uint32_t v_clean,
                                       uint32_t anchor)
{
    if (anchor == 0) return 0;
    uint32_t expected = (v_clean / anchor) * anchor;
    uint32_t lo = (uint32_t)(state_hash & 0xFFFFu);
    uint32_t diff = (lo > expected) ? (lo - expected) : (expected - lo);
    return diff & 0xFFFFu;
}

static inline uint64_t ma_score(uint64_t state_hash,
                                 uint32_t v_clean,
                                 uint32_t anchor,
                                 uint32_t alpha)
{
    uint32_t drift = ma_drift_error(state_hash, v_clean, anchor);
    uint32_t dist  = ma_distortion(v_clean, anchor);

    /* weighted sum: alpha/256 × drift + (256-alpha)/256 × dist */
    uint64_t s = ((uint64_t)drift * alpha +
                  (uint64_t)dist  * (256u - alpha)) >> 8;
    return s;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — BEST ANCHOR SELECTION
 *
 * ma_select_best():
 *   compute score for all 4 anchors
 *   return index of minimum
 *   update ctx->anchor + ctx->score (EMA decay)
 * ══════════════════════════════════════════════════════════════════════ */

/* EMA decay: score[i] = score[i] × 7/8 + new_score / 8  (bit-shift) */
static inline void ma_ema_update(uint64_t *score, uint64_t new_val)
{
    *score = (*score * 7u >> 3u) + (new_val >> 3u);
}

static inline uint32_t ma_select_best(MAAnchorCtx *ctx,
                                       uint64_t     state_hash,
                                       uint32_t     v_clean)
{
    if (!ctx) return MA_ANCHOR_144;

    uint64_t best_score = UINT64_MAX;
    uint32_t best_idx   = ctx->anchor_idx;  /* default: keep current */

    for (uint32_t i = 0; i < MA_N_ANCHORS; i++) {
        uint64_t s = ma_score(state_hash, v_clean,
                              MA_ANCHORS[i], ctx->alpha);
        ma_ema_update(&ctx->score[i], s);
        if (ctx->score[i] < best_score) {
            best_score = ctx->score[i];
            best_idx   = i;
        }
    }

    ctx->anchor_idx = best_idx;
    ctx->anchor     = MA_ANCHORS[best_idx];
    ctx->select_count++;
    return ctx->anchor;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — SOFT SNAP
 *
 * snap_to_anchor(v, a):
 *   floor_a = (v / a) * a          ← lower grid point
 *   ceil_a  = floor_a + a          ← upper grid point
 *   nearest = closer one
 *
 * soft_snap(v_clean, a, alpha):
 *   v_snapped = (v_clean × (256-alpha) + nearest × alpha) >> 8
 *   integer lerp — no float
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t ma_snap(uint32_t v, uint32_t anchor)
{
    if (anchor == 0) return v;
    uint32_t floor_a = (v / anchor) * anchor;
    uint32_t ceil_a  = floor_a + anchor;
    uint32_t d_floor = v - floor_a;
    uint32_t d_ceil  = ceil_a - v;
    return (d_floor <= d_ceil) ? floor_a : ceil_a;
}

static inline uint32_t ma_soft_snap(uint32_t v_clean,
                                     uint32_t anchor,
                                     uint32_t alpha)
{
    uint32_t nearest = ma_snap(v_clean, anchor);
    /* integer lerp: v × (256-alpha) + nearest × alpha >> 8 */
    uint32_t snapped = (uint32_t)(
        ((uint64_t)v_clean * (256u - alpha) +
         (uint64_t)nearest *  alpha) >> 8u
    );
    return snapped;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — ALPHA TRANSITION (CPU → GPU phase)
 *
 * During TC_EVENT_ANCHOR: bump alpha toward MA_ALPHA_GPU
 * During TC_EVENT_CYCLE_END: decay alpha back toward MA_ALPHA_CPU
 *
 * Rate: +32 per anchor (256 → target in ~6 anchor events)
 *       -8  per cycle  (decay when not enforced)
 * ══════════════════════════════════════════════════════════════════════ */

#define MA_ALPHA_RISE   32u
#define MA_ALPHA_DECAY  8u

static inline void ma_alpha_rise(MAAnchorCtx *ctx)
{
    if (!ctx) return;
    ctx->alpha += MA_ALPHA_RISE;
    if (ctx->alpha > MA_ALPHA_GPU) ctx->alpha = MA_ALPHA_GPU;
}

static inline void ma_alpha_decay(MAAnchorCtx *ctx)
{
    if (!ctx) return;
    if (ctx->alpha > MA_ALPHA_CPU + MA_ALPHA_DECAY)
        ctx->alpha -= MA_ALPHA_DECAY;
    else
        ctx->alpha = MA_ALPHA_CPU;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — MULTI-ANCHOR FABRIC (extends TCFabric)
 *
 * One MAAnchorCtx per virtual core — isolated, no sharing
 * ══════════════════════════════════════════════════════════════════════ */

#define MA_MAGIC  0x4D414E43u   /* "MANC" */

typedef struct {
    uint32_t     magic;
    uint32_t     N;             /* must match TCFabric.N                */
    MAAnchorCtx  ctx[TC_CORES_MAX];
    uint64_t     total_snaps;   /* total soft-snaps applied             */
    uint64_t     anchor_changes;/* times best anchor switched           */
} MAFabric;

static inline void ma_fabric_init(MAFabric *mf, uint32_t N)
{
    if (!mf) return;
    if (N == 0 || N > TC_CORES_MAX) N = TC_CORES_MAX;
    memset(mf, 0, sizeof(*mf));
    mf->magic = MA_MAGIC;
    mf->N     = N;
    for (uint32_t i = 0; i < N; i++)
        ma_ctx_init(&mf->ctx[i]);
}

/*
 * ma_step — main multi-anchor dispatch
 *
 * Call after tc_dispatch() when events contain TC_EVENT_ANCHOR
 * or every step (lightweight path for non-anchor steps)
 *
 * Returns v_snapped (soft-snapped canonical value)
 */
static inline uint32_t ma_step(MAFabric *mf,
                                 TCFabric *tf,
                                 uint32_t  v_clean,
                                 uint8_t   events)
{
    if (!mf || !tf) return v_clean;

    uint32_t core_id = tf->core_id;
    if (core_id >= mf->N) return v_clean;

    MAAnchorCtx *ctx = &mf->ctx[core_id];
    TCCore      *tc  = &tf->cores[core_id];

    /* anchor event: select best + raise alpha */
    if (events & TC_EVENT_ANCHOR) {
        uint32_t prev = ctx->anchor;
        ma_select_best(ctx, tc->state_hash, v_clean);
        ma_alpha_rise(ctx);
        if (ctx->anchor != prev) mf->anchor_changes++;
    }

    /* cycle end: decay alpha */
    if (events & TC_EVENT_CYCLE_END)
        ma_alpha_decay(ctx);

    /* soft snap with current best anchor + alpha */
    uint32_t v_snapped = ma_soft_snap(v_clean, ctx->anchor, ctx->alpha);
    if (v_snapped != v_clean) mf->total_snaps++;

    return v_snapped;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — STATS
 * ══════════════════════════════════════════════════════════════════════ */

static inline void ma_stats_print(const MAFabric *mf)
{
    if (!mf) return;
    static const char *anchor_names[] = {" 72"," 144"," 288"," 360"};
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Multi-Anchor Fabric Stats                      ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ N=%u cores  snaps=%-8llu changes=%-8llu    ║\n",
           mf->N,
           (unsigned long long)mf->total_snaps,
           (unsigned long long)mf->anchor_changes);
    printf("╠══════════════════════════════════════════════════╣\n");
    for (uint32_t i = 0; i < mf->N; i++) {
        const MAAnchorCtx *c = &mf->ctx[i];
        printf("║ core[%2u] anchor=%s alpha=%3u selected=%-6llu ║\n",
               i,
               anchor_names[c->anchor_idx],
               c->alpha,
               (unsigned long long)c->select_count);
    }
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_MULTI_ANCHOR_H */
