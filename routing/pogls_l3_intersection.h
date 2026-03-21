/*
 * pogls_l3_intersection.h — Hybrid Field Router V2
 * ══════════════════════════════════════════════════════════════════════
 *
 * 3-stage pipeline:
 *   STAGE 0: scatter addr → (a, b) via PHI
 *   STAGE 1: GEO  — geometry decides fast (World A/B/Twin/Invalid)
 *   STAGE 2: L3   — score confirms (World A only)
 *
 * GeoGate:
 *   valid   = a²+b² < 2^41   (integer unit circle, bit-shift check)
 *   twin    = |a-b| < EPS    (backbone/diagonal line)
 *   World A = b < a          (stable field   → MAIN candidate)
 *   World B = b > a          (dispersive     → GHOST fast)
 *
 * Phase flip:
 *   world crossed y=x last step AND structured_flip > random_flip → MAIN boost
 *
 * L3 probes (non-PHI offsets — root cause fix):
 *   L3_PROBE_A = fib(610)×2⁹  = 312,320   safe from PHI harmonics
 *   L3_PROBE_B = fib(89)×2¹³  = 729,088   safe from PHI harmonics
 *
 * Anchor (v38 pattern — prevents score=0):
 *   anchor = (value>>0)^(value>>16)^(value>>32)^(value>>48)  per axis
 *
 * Ghost streak guard: ghost_streak > 8 → force MAIN (anti-drift)
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_L3_INTERSECTION_H
#define POGLS_L3_INTERSECTION_H

#include <stdint.h>
#include <string.h>

/* ── PHI scatter constants (FROZEN) ─────────────────────────────── */
#define L3_PHI_SCALE        (1u << 20)
/* PHI constants — from POGLS single source (pogls_platform.h) */
#ifndef POGLS_PHI_CONSTANTS
#  include "../pogls_platform.h"
#endif
#define L3_PHI_UP    POGLS_PHI_UP
#define L3_PHI_DOWN  POGLS_PHI_DOWN
#define L3_FP_SHIFT         20u

/* ── Unit circle ─────────────────────────────────────────────────── */
#define L3_CIRCLE_SHIFT     41u          /* bit42+: a²+b² >> 41 > 0 → INVALID */

/* ── Twin tolerance ──────────────────────────────────────────────── */
#define L3_TWIN_EPS         1024u        /* |a-b| < 1024 → twin zone */

/* ── L3 non-PHI probe offsets (fib×2^k, no harmonic with PHI) ───── */
#define L3_PROBE_A          312320u      /* fib(610)×2⁹  0.2979×scale */
#define L3_PROBE_B          729088u      /* fib(89)×2¹³  0.6953×scale */

/* ── L3 decision threshold ───────────────────────────────────────── */
#define L3_SCORE_MAIN       24u  /* C+: seq/burst peak 26-38, chaos<22 */

/* ── Ghost streak guard ──────────────────────────────────────────── */
/* GHOST_STREAK_MAX: shared constant — sync with V4_STREAK_MAX in pogls_v4_final.h */
#ifndef POGLS_GHOST_STREAK_MAX
#  define POGLS_GHOST_STREAK_MAX  8u
#endif
#define L3_GHOST_STREAK_MAX  POGLS_GHOST_STREAK_MAX  /* anti-drift: streak > 8 → MAIN */

/* ── Wolfram Rule 30 ─────────────────────────────────────────────── */
static inline uint8_t wolfram_rule30(uint8_t l, uint8_t m, uint8_t r)
{
    static const uint8_t rule30[8] = {0,1,1,1,1,0,0,0};
    return rule30[((l<<2)|(m<<1)|r) & 7];
}

/* ══════════════════════════════════════════════════════════════════
 * STAGE 0: SCATTER  addr → (a, b)
 * ══════════════════════════════════════════════════════════════════ */
static inline void l3_scatter(uint64_t addr, uint32_t *a, uint32_t *b)
{
    uint32_t m = L3_PHI_SCALE - 1u;
    *a = (uint32_t)(((addr & m) * L3_PHI_UP)   >> L3_FP_SHIFT) & m;
    *b = (uint32_t)(((addr & m) * L3_PHI_DOWN)  >> L3_FP_SHIFT) & m;
}

/* ── GeoWorld enum ───────────────────────────────────────────────── */
typedef enum { GEO_A=0, GEO_B=1, GEO_TWIN=2, GEO_INVALID=3 } GeoWorld;

/* ── route enum ──────────────────────────────────────────────────── */
typedef enum { ROUTE_MAIN, ROUTE_GHOST, ROUTE_SHADOW } RouteTarget;

/* ══════════════════════════════════════════════════════════════════
 * STAGE 2: L3 SCORE  (non-PHI probes + v38 anchor)
 * Called only for World A — skipped for World B (fast ghost)
 * ══════════════════════════════════════════════════════════════════ */
/* l3_score — addr entropy detector
 * low-addr data (seq/burst): anc[1] sparse → inter≈0  → score=0 → MAIN
 * high-addr data (chaos):    anc[1] dense  → inter>0  → score>0 → GHOST
 * Inverting the usual: score=0 means "looks sequential" = trusted
 *
 * Returns: 0 = low entropy (structured/sequential) → MAIN candidate
 *         >0 = high entropy (chaotic/scattered)   → GHOST candidate
 */
/* l3_score_rel — 2-step relation anchor
 * rel2 = addr ^ prev ^ prev2  captures trajectory, not just step.
 * seq:   rel2 near-zero (small, regular steps) → score = 0  → MAIN
 * phi:   rel2 has predictable pattern          → score low  → MAIN
 * chaos: rel2 fully scattered 20-bit           → score > 2  → GHOST
 * rand:  rel2 random but different from chaos  → score > 2  → GHOST
 *
 * Two-step catches patterns that single-step misses:
 *   e.g. alternating +4/-4: step=4,8,4,8 but rel2=addr^prev^prev2≈0
 */
static inline uint32_t l3_score_rel(uint32_t addr, uint32_t prev, uint32_t prev2)
{
    uint32_t rel  = addr ^ prev ^ (prev >> 7);
    rel ^= (rel >> 11);                           /* self-mix: reduces prev drift */
    uint32_t rel2 = addr ^ prev ^ prev2;          /* 2-step trajectory */
    rel2 ^= (rel2 >> 11);                         /* self-mix on trajectory too  */
    uint32_t anchor = (rel & (rel >> 8) & (rel >> 16))
                    & (rel2 & (rel2 >> 4));
    return (uint32_t)__builtin_popcount(anchor);
}

/* ══════════════════════════════════════════════════════════════════
 * Hilbert helpers (unchanged)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t morton2D(uint16_t x, uint16_t y)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++)
        m |= ((x & (1U<<i)) << i) | ((y & (1U<<i)) << (i+1));
    return m;
}
static inline uint32_t hilbert_index(uint16_t x, uint16_t y)
{
    uint32_t mo = morton2D(x, y); return mo ^ (mo >> 1);
}

/* ══════════════════════════════════════════════════════════════════
 * L3 ENGINE
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* phase flip state */
    GeoWorld prev_world;
    uint8_t  prev_flip;         /* was last step a flip? */
    uint32_t structured_flip;   /* rhythmic A↔B crossings */
    uint32_t random_flip;       /* erratic crossings */

    /* phase flip state */
    uint8_t  phase_flip;
    uint8_t  steps_since_flip;
    /* ghost streak guard */
    uint32_t flip_count;
    uint32_t ghost_streak;

    /* hilbert */
    uint32_t prev_hilbert;
    uint32_t prev_addr;     /* for relation anchor (step-1)  */
    uint32_t prev2_addr;    /* for 2-step relation (step-2)  */
    uint8_t  anchor_valid;

    /* stats */
    uint64_t total_ops;
    uint64_t main_routes;
    uint64_t ghost_routes;
    uint64_t shadow_routes;
    uint64_t fast_skips;        /* geo fast-path (B→ghost, twin→main) */
    uint64_t l3_calls;          /* times L3 score was computed */
    uint64_t streak_resets;     /* ghost streak forced MAIN */
} L3Engine;

static inline void l3_init(L3Engine *eng) { memset(eng, 0, sizeof(*eng)); }

/* ══════════════════════════════════════════════════════════════════
 * l3_process — 3-stage hybrid field router
 * ══════════════════════════════════════════════════════════════════ */
static inline RouteTarget l3_process(L3Engine *eng, uint64_t value)
{
    eng->total_ops++;

    /* ── STAGE 0: SCATTER ────────────────────────────────────────── */
    uint32_t a, b;
    l3_scatter(value, &a, &b);

    /* ── STAGE 1: GEO ────────────────────────────────────────────── */

    /* 1a. Unit circle check (bit-shift, zero divide) */
    uint64_t sq = (uint64_t)a * a + (uint64_t)b * b;
    if (sq >> L3_CIRCLE_SHIFT) {
        eng->shadow_routes++;
        eng->prev_world  = GEO_INVALID;
        eng->phase_flip  = 0;
        eng->ghost_streak = 0;
        return ROUTE_SHADOW;
    }

    /* 1b. Classify world */
    uint32_t absdiff = (a >= b) ? (a - b) : (b - a);
    GeoWorld world = (absdiff < L3_TWIN_EPS) ? GEO_TWIN
                   : (a > b)                 ? GEO_A
                   :                           GEO_B;

    /* 1c. Phase flip detection (structured vs random) */
    uint8_t flip = 0;
    if (eng->anchor_valid &&
        eng->prev_world != GEO_INVALID &&
        eng->prev_world != GEO_TWIN    &&
        world           != GEO_TWIN    &&
        world           != eng->prev_world) {
        flip = 1;
        /* rhythmic = steps since last flip <= 4
         * PHI: A-A-B-A-A-B pattern = gap ~2-3 → structured
         * random/chaos: gaps vary widely → not rhythmic */
        eng->flip_count++;
        /* rhythmic ≤ 4 gap: PHI ~0-1, random ~0-8 */
        if (eng->steps_since_flip <= 2u) eng->structured_flip++;
        else                             eng->random_flip++;
        eng->steps_since_flip = 0;
    }
    if (!flip) {
        /* no world-crossing this step — advance the counter */
        if (eng->steps_since_flip < 255u) eng->steps_since_flip++;
    }
    eng->prev_flip  = flip;
    eng->prev_world = world;

    /* structured: high flip density (>55%) AND rhythmic (s>r)
     * phi ~76% density + rhythmic; random ~47% lower density */
    uint32_t total_A = eng->total_ops > 0 ? eng->total_ops : 1u;
    uint8_t flip_dense = (eng->flip_count * 100u / total_A) > 55u;
    uint8_t structured = flip_dense && (eng->structured_flip > eng->random_flip);

    /* 1d. Route via geometry */
    RouteTarget route;

    if (world == GEO_TWIN) {
        /* backbone diagonal → always MAIN */
        route = ROUTE_MAIN;
        eng->fast_skips++;
    }
    else if (world == GEO_B) {
        /* dispersive field: structured(phi-like) → MAIN, else GHOST */
        if (structured) { route = ROUTE_MAIN; }
        else { route = ROUTE_GHOST; eng->fast_skips++; }
    }
    else {
        /* World A — Collapsed L2/L3: single confidence model
         *
         * conf = f_phi + f_local + (score <= 2)
         *   conf >= 1 → MAIN  (any one signal is enough)
         *   conf == 0 → GHOST (no signal at all)
         *
         * Replaces hard threshold: gated by at least one positive signal,
         * so edge cases (phi at boundary, burst at local edge) still pass.
         */
        uint32_t addr20 = (uint32_t)(value & 0xFFFFFu);

        /* f_phi: PHI delta on addr (exact ± tol, already checked by DualSensor
         * at pipeline level, but recheck here using prev_addr for L3 context) */
        uint32_t delta_l3 = (addr20 >= eng->prev_addr)
                          ? (addr20 - eng->prev_addr)
                          : (eng->prev_addr - addr20);
        uint32_t f_phi_l3 = (delta_l3 >= POGLS_PHI_DOWN - 8192u &&
                              delta_l3 <= POGLS_PHI_DOWN + 8192u) ||
                             (delta_l3 >= POGLS_PHI_COMP - 8192u &&
                              delta_l3 <= POGLS_PHI_COMP + 8192u) ? 1u : 0u;

        /* f_local: small Hilbert step in addr space */
        uint16_t hx_cur, hy_cur, hx_prv, hy_prv;
        uint16_t _hxc = (uint16_t)(addr20 & 0x3FFu);
        uint16_t _hyc = (uint16_t)((addr20 >> 10) & 0x3FFu);
        uint16_t _hxp = (uint16_t)(eng->prev_addr & 0x3FFu);
        uint16_t _hyp = (uint16_t)((eng->prev_addr >> 10) & 0x3FFu);
        (void)hx_cur; (void)hy_cur; (void)hx_prv; (void)hy_prv;
        uint32_t _dx  = (_hxc > _hxp) ? (_hxc - _hxp) : (_hxp - _hxc);
        uint32_t _dy  = (_hyc > _hyp) ? (_hyc - _hyp) : (_hyp - _hyc);
        uint32_t f_local_l3 = ((_dx + _dy) < 32u) ? 1u : 0u;

        /* relation score */
        uint32_t score = l3_score_rel(addr20, eng->prev_addr, eng->prev2_addr);
        eng->l3_calls++;
        uint32_t score_ok = (score <= 2u) ? 1u : 0u;

        /* confidence: any one positive signal → MAIN */
        uint32_t conf = f_phi_l3 + f_local_l3 + score_ok + (structured ? 1u : 0u);
        route = (conf >= 1u) ? ROUTE_MAIN : ROUTE_GHOST;
    }

    /* ── Ghost streak guard (anti-drift) ─────────────────────────── */
    if (route == ROUTE_GHOST) {
        eng->ghost_streak++;
        if (eng->ghost_streak > L3_GHOST_STREAK_MAX) {
            route = ROUTE_MAIN;
            eng->ghost_streak = 0;
            eng->streak_resets++;
        }
    } else {
        eng->ghost_streak = 0;
    }

    /* ── Hilbert update ──────────────────────────────────────────── */
    uint16_t x = (uint16_t)(value & 0xFFFF);
    uint16_t y = (uint16_t)((value >> 16) & 0xFFFF);
    eng->prev_hilbert = hilbert_index(x, y);
    eng->prev2_addr   = eng->prev_addr;                /* shift history */
    eng->prev_addr    = (uint32_t)(value & 0xFFFFFu);  /* update for next cycle */
    eng->anchor_valid = 1;

    switch (route) {
        case ROUTE_MAIN:   eng->main_routes++;   break;
        case ROUTE_GHOST:  eng->ghost_routes++;  break;
        case ROUTE_SHADOW: eng->shadow_routes++; break;
    }
    return route;
}

static inline void l3_print_stats(const L3Engine *eng)
{
    uint64_t t = eng->total_ops ? eng->total_ops : 1;
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  L3 Hybrid Field Router Stats                 ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Total ops:    %10llu                       ║\n",
           (unsigned long long)eng->total_ops);
    printf("║ Geo fast:     %10llu (%llu%%)               ║\n",
           (unsigned long long)eng->fast_skips,
           (unsigned long long)(eng->fast_skips*100/t));
    printf("║ L3 calls:     %10llu (%llu%%)               ║\n",
           (unsigned long long)eng->l3_calls,
           (unsigned long long)(eng->l3_calls*100/t));
    printf("║ MAIN:         %10llu (%llu%%)               ║\n",
           (unsigned long long)eng->main_routes,
           (unsigned long long)(eng->main_routes*100/t));
    printf("║ GHOST:        %10llu (%llu%%)               ║\n",
           (unsigned long long)eng->ghost_routes,
           (unsigned long long)(eng->ghost_routes*100/t));
    printf("║ SHADOW:       %10llu (%llu%%)               ║\n",
           (unsigned long long)eng->shadow_routes,
           (unsigned long long)(eng->shadow_routes*100/t));
    printf("║ s_flip/r_flip %10u / %-10u           ║\n",
           eng->structured_flip, eng->random_flip);
    printf("║ streak_resets:%10llu                       ║\n",
           (unsigned long long)eng->streak_resets);
    printf("╚════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_L3_INTERSECTION_H */
