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
    uint32_t  streak;           /* consecutive same-anchor picks        */
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

/* bias_row: per-core histogram row (may be NULL)
 * bias_k:   penalty multiplier (MA_BIAS_K_DEFAULT/AGGR/EXPLORE)
 * penalty = (bias_row[bkt] * bias_k) >> 8   → 0..511 max (k=4, sat=32767)
 * core_id + step_hint: for deterministic noise (prevents tie collapse)  */
static inline uint32_t ma_select_best(MAAnchorCtx   *ctx,
                                       uint64_t       state_hash,
                                       uint32_t       v_clean,
                                       const uint16_t *bias_row,
                                       uint32_t        bias_k,
                                       uint32_t        core_id,
                                       uint32_t        step_hint)
{
    if (!ctx) return MA_ANCHOR_144;

    uint64_t best_score = UINT64_MAX;
    uint32_t best_idx   = ctx->anchor_idx;

    for (uint32_t i = 0; i < MA_N_ANCHORS; i++) {
        uint64_t s = ma_score(state_hash, v_clean, MA_ANCHORS[i], ctx->alpha);
        ma_ema_update(&ctx->score[i], s);

        uint64_t penalized = ctx->score[i];
        if (bias_row && bias_k) {
            uint32_t bkt = (MA_ANCHORS[i] / 9u) & 15u;
            /* score += (bias * k) >> 4 — over-used anchor scores worse */
            penalized += ((uint64_t)bias_row[bkt] * bias_k) >> 4u;
        }

        /* ── deterministic signed noise — prevents tie-collapse ─────────
         * Signed range [-8,+7] (4 bits): strong enough to break local
         * minima, small enough to never override real signal (scores
         * are typically in thousands). Fully deterministic: no rand().  */
        int32_t noise = (int32_t)((((core_id * 2654435761u)
                          ^ (step_hint  * 1013904223u)
                          ^ (i          * 2246822519u)) & 15u)) - 8;
        penalized = (uint64_t)((int64_t)penalized + noise);

        /* ── streak penalty: same anchor picked N times → penalize ──────
         * After 2 consecutive picks of same idx, add escalating penalty.
         * streak=2 → +256, streak=3 → +512, streak=4+ → +1024
         * Forces diversity without overriding strong signal.            */
        if (i == ctx->anchor_idx && ctx->streak >= 2u) {
            uint32_t sp = ctx->streak >= 4u ? 1024u :
                          ctx->streak == 3u ?  512u : 256u;
            penalized += sp;
        }

        /* ── diversity tiebreak: prefer different anchor on exact tie ──
         * If score ties, pick anchor_idx != current to spread load.    */
        if (penalized < best_score ||
            (penalized == best_score && i != ctx->anchor_idx)) {
            best_score = penalized;
            best_idx   = i;
        }
    }

    /* update streak counter */
    if (best_idx == ctx->anchor_idx)
        ctx->streak = (ctx->streak < 255u) ? ctx->streak + 1u : 255u;
    else
        ctx->streak = 0u;

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

/* bias_k modes — runtime tunable, no recompile needed
 *   MA_BIAS_K_DEFAULT = 1 → soft   score -= bias*1>>8  (0..127)
 *   MA_BIAS_K_AGGR    = 2 → normal score -= bias*2>>8  (0..255)
 *   MA_BIAS_K_EXPLORE = 4 → strong score -= bias*4>>8  (0..511) */
#define MA_BIAS_K_DEFAULT  1u
#define MA_BIAS_K_AGGR     2u
#define MA_BIAS_K_EXPLORE  4u

typedef struct {
    uint32_t     magic;
    uint32_t     N;             /* must match TCFabric.N                */
    uint32_t     bias_k;        /* penalty multiplier (1/2/4), runtime  */
    uint32_t     _pad;
    MAAnchorCtx  ctx[TC_CORES_MAX];
    uint64_t     total_snaps;
    uint64_t     anchor_changes;
    /* ── adaptive bias_k tracking (per-cycle window) ────────────────── */
    uint64_t     cycle_changes;     /* anchor_changes in current cycle  */
    uint64_t     cycle_events;      /* anchor events fired this cycle   */
} MAFabric;

static inline void ma_fabric_init(MAFabric *mf, uint32_t N)
{
    if (!mf) return;
    if (N == 0 || N > TC_CORES_MAX) N = TC_CORES_MAX;
    memset(mf, 0, sizeof(*mf));
    mf->magic  = MA_MAGIC;
    mf->N      = N;
    mf->bias_k = MA_BIAS_K_DEFAULT;   /* runtime tunable: 1/2/4 */
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
static inline uint32_t ma_step(MAFabric       *mf,
                                 TCFabric       *tf,
                                 uint32_t        v_clean,
                                 uint8_t         events,
                                 const uint16_t *bias_row)  /* P2: bias penalty, may be NULL */
{
    if (!mf || !tf) return v_clean;

    uint32_t core_id = tf->core_id;
    if (core_id >= mf->N) return v_clean;

    MAAnchorCtx *ctx = &mf->ctx[core_id];
    (void)ctx; /* used in soft-snap below; anchor select now iterates all cores */

    /* anchor event: select best for ALL cores → proper load distribution
     * Root cause fix: TC_EVENT_ANCHOR always fires on core[0] (phase%N==0)
     * so only updating current core_id creates permanent skew (5-0-0-0).
     * Solution: on anchor event, run ma_select_best() for every core.
     * Each core uses its own state_hash → independent, deterministic.    */
    if (events & TC_EVENT_ANCHOR) {
        uint32_t step_hint = (uint32_t)(tf->total_steps & 0xFFFFFFFFu);
        for (uint32_t ci = 0; ci < mf->N; ci++) {
            MAAnchorCtx *cctx = &mf->ctx[ci];
            TCCore      *ctc  = &tf->cores[ci];
            const uint16_t *crow = (ci == core_id) ? bias_row : NULL;
            uint32_t prev = cctx->anchor;
            ma_select_best(cctx, ctc->state_hash, v_clean, crow, mf->bias_k,
                           ci, step_hint);
            ma_alpha_rise(cctx);
            if (cctx->anchor != prev) {
                mf->anchor_changes++;
                mf->cycle_changes++;
            }
        }
        mf->cycle_events += mf->N;  /* N cores updated per anchor event */
    }

    /* cycle end: decay alpha + adaptive bias_k auto-tune
     * ── Adaptive bias_k rules ────────────────────────────────────────
     * change_rate = cycle_changes / cycle_events (this cycle window)
     * rate > 70%: too much jitter  → lower bias_k (reduce exploration)
     * rate < 20%: too static/stuck → raise bias_k (increase exploration)
     * rate 20-70%: healthy range   → no change
     * bias_k clamped to [1, 4] = {DEFAULT, AGGR, EXPLORE}              */

    if (events & TC_EVENT_CYCLE_END) {
        ma_alpha_decay(ctx);
        /* adaptive bias_k: evaluate this cycle's change rate */
        if (mf->cycle_events > 0) {
            uint32_t rate_pct = (uint32_t)((mf->cycle_changes * 100u)
                                            / mf->cycle_events);
            if (rate_pct > 75u && mf->bias_k > MA_BIAS_K_DEFAULT)
                mf->bias_k--;          /* too jittery  → soften penalty */
            else if (rate_pct < 10u && mf->bias_k < MA_BIAS_K_EXPLORE)
                mf->bias_k++;          /* too stuck    → push explore   */
            /* hard clamp [1,4] — no runaway in either direction        */
            if (mf->bias_k < 1u) mf->bias_k = 1u;
            if (mf->bias_k > 4u) mf->bias_k = 4u;
        }
        /* reset window for next cycle */
        mf->cycle_changes = 0;
        mf->cycle_events  = 0;
    }

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
