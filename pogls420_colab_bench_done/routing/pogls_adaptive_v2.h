/*
 * pogls_adaptive_v2.h — Context-Based Adaptive Routing
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Key Improvements:
 *   - LOCAL window (not global absolute)
 *   - Anchor system (lock reference point)
 *   - Mandelbrot = soft weight (not hard decision)
 *   - Pattern-driven (Hilbert > Mandel)
 *
 * Philosophy:
 *   "โลก relative (context-based)" not "โลก absolute"
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_ADAPTIVE_V2_H
#define POGLS_ADAPTIVE_V2_H
/* ════════════════════════════════════════════════════════
 * FLOAT PATH DISABLED — V4 uses pogls_pipeline_wire.h + l3_intersection
 * This file is ORPHANED. Do not include in new code.
 * ════════════════════════════════════════════════════════ */
#ifdef POGLS_V4_STRICT
#  error "FLOAT PATH DISABLED IN V4 — use pogls_pipeline_wire.h"
#endif
/* legacy code below — not active in V4 pipeline */


#include <stdint.h>
#include <math.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * CONFIG
 * ══════════════════════════════════════════════════════════════════ */
#define MANDEL_MAX_ITER     32
#define ESCAPE_RADIUS       4.0f

#define MAIN_THRESHOLD      0.5f
#define GHOST_THRESHOLD     0.5f

#define LOCAL_WINDOW_SIZE   1024.0f   /* Zoom-in factor */
#define ANCHOR_UPDATE_THRESH 0.8f     /* Update anchor if pattern strong */

#define FAST_CACHE_SIZE     1024
#define PROMOTE_HIT_COUNT   3

/* ══════════════════════════════════════════════════════════════════
 * ROUTE TARGET
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    ROUTE_MAIN,
    ROUTE_GHOST,
    ROUTE_SHADOW
} RouteTarget;

/* ══════════════════════════════════════════════════════════════════
 * SIGNAL
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    float stable_score;
    float pattern_score;
    float chaos_score;
} Signal;

/* ══════════════════════════════════════════════════════════════════
 * CONTEXT (with anchor!)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t prev_hilbert;
    uint32_t prev_tail;
    
    /* Anchor system (local reference) */
    uint16_t anchor_x;
    uint16_t anchor_y;
    uint8_t  anchor_locked;
    
    /* Adaptive weights */
    float weight_pattern;    /* Primary driver */
    float weight_stable;
    float weight_chaos_inv;
} Context;

/* ══════════════════════════════════════════════════════════════════
 * FAST CACHE
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t key;
    uint32_t value;
    uint8_t  hit_count;
    uint8_t  valid;
    uint8_t  pad[2];
} FastEntry;

/* ══════════════════════════════════════════════════════════════════
 * ADAPTIVE ROUTER
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    Context      ctx;
    FastEntry    fast_cache[FAST_CACHE_SIZE];
    
    uint64_t     total_ops;
    uint64_t     main_routes;
    uint64_t     ghost_routes;
    uint64_t     shadow_routes;
    uint64_t     fast_hits;
    uint64_t     anchor_updates;
} AdaptiveRouter;

/* ══════════════════════════════════════════════════════════════════
 * SPACE MAPPING
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t morton2D(uint16_t x, uint16_t y)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) {
        m |= ((x & (1U << i)) << i) | ((y & (1U << i)) << (i + 1));
    }
    return m;
}

static inline uint32_t hilbert_index(uint16_t x, uint16_t y)
{
    uint32_t morton = morton2D(x, y);
    return morton ^ (morton >> 1);
}

static inline float hilbert_similarity(uint32_t a, uint32_t b)
{
    int diff = __builtin_popcount(a ^ b);
    return 1.0f - (diff / 32.0f);
}

/* ══════════════════════════════════════════════════════════════════
 * LOCAL NORMALIZATION (key improvement!)
 * ══════════════════════════════════════════════════════════════════ */
static inline float normalize_local(uint16_t v, uint16_t center)
{
    /* Map to [-1, 1] relative to anchor */
    return ((float)(v - center) / LOCAL_WINDOW_SIZE);
}

/* ══════════════════════════════════════════════════════════════════
 * MANDELBROT (context-aware)
 * ══════════════════════════════════════════════════════════════════ */
static inline float mandel_score(float cx, float cy)
{
    float zx = 0.0f, zy = 0.0f;
    
    for (int i = 0; i < MANDEL_MAX_ITER; i++) {
        float zx2 = zx * zx;
        float zy2 = zy * zy;
        
        if (zx2 + zy2 > ESCAPE_RADIUS) {
            return (float)i;
        }
        
        float xt = zx2 - zy2 + cx;
        zy = 2.0f * zx * zy + cy;
        zx = xt;
    }
    
    return (float)MANDEL_MAX_ITER;
}

/* Clamped chaos score (reduce boundary noise) */
static inline float chaos_score_clamped(float mandel_val)
{
    if (mandel_val < 4.0f) {
        return 1.0f;  /* Fast escape = chaos */
    } else if (mandel_val > 20.0f) {
        return 0.0f;  /* Deep stable */
    } else {
        /* Gradual transition */
        return 1.0f - (mandel_val / MANDEL_MAX_ITER);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * SIGNAL BUILDER (context-based!)
 * ══════════════════════════════════════════════════════════════════ */
static inline Signal build_signal_v2(uint16_t x, uint16_t y,
                                       uint32_t hilbert,
                                       uint32_t prev_hilbert,
                                       const Context *ctx)
{
    Signal s;
    
    /* Pattern from Hilbert (primary!) */
    s.pattern_score = hilbert_similarity(hilbert, prev_hilbert);
    
    /* Map to LOCAL complex plane (relative to anchor) */
    float cx = normalize_local(x, ctx->anchor_x);
    float cy = normalize_local(y, ctx->anchor_y);
    
    /* Mandelbrot stability (local context) */
    float mandel_val = mandel_score(cx, cy);
    s.stable_score = mandel_val / MANDEL_MAX_ITER;
    
    /* Chaos (clamped) */
    s.chaos_score = chaos_score_clamped(mandel_val);
    
    return s;
}

/* ══════════════════════════════════════════════════════════════════
 * ANCHOR SYSTEM (lock reference point)
 * ══════════════════════════════════════════════════════════════════ */
static inline void anchor_update(Context *ctx, uint16_t x, uint16_t y,
                                  float pattern_score)
{
    /* Update anchor if pattern is strong */
    if (pattern_score > ANCHOR_UPDATE_THRESH || !ctx->anchor_locked) {
        ctx->anchor_x = x;
        ctx->anchor_y = y;
        ctx->anchor_locked = 1;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * MENDEL ROUTER (pattern-driven!)
 * ══════════════════════════════════════════════════════════════════ */
static inline RouteTarget route_decide_v2(const Signal *s, const Context *ctx)
{
    /* Pattern-driven scoring (Hilbert > Mandel) */
    float main_score = (s->pattern_score * ctx->weight_pattern) +
                       (s->stable_score * ctx->weight_stable) +
                       ((1.0f - s->chaos_score) * ctx->weight_chaos_inv);
    
    float ghost_score = (s->chaos_score * 0.7f) +
                        ((1.0f - s->pattern_score) * 0.3f);
    
    /* Smooth decision */
    if (main_score > MAIN_THRESHOLD) {
        return ROUTE_MAIN;
    }
    
    if (ghost_score > GHOST_THRESHOLD) {
        return ROUTE_GHOST;
    }
    
    return ROUTE_SHADOW;
}

/* ══════════════════════════════════════════════════════════════════
 * FAST CACHE
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t fast_hash(uint32_t key)
{
    return (key ^ (key >> 16)) & (FAST_CACHE_SIZE - 1);
}

static inline int fast_lookup(AdaptiveRouter *ar, uint32_t key, uint32_t *out)
{
    uint32_t idx = fast_hash(key);
    FastEntry *e = &ar->fast_cache[idx];
    
    if (e->valid && e->key == key) {
        e->hit_count++;
        *out = e->value;
        ar->fast_hits++;
        return 1;
    }
    
    return 0;
}

static inline void fast_promote(AdaptiveRouter *ar, uint32_t key, uint32_t value)
{
    uint32_t idx = fast_hash(key);
    FastEntry *e = &ar->fast_cache[idx];
    
    if (e->key == key && e->hit_count > 0) {
        e->hit_count++;
    } else {
        e->key = key;
        e->value = value;
        e->hit_count = 1;
        e->valid = 0;
    }
    
    if (e->hit_count >= PROMOTE_HIT_COUNT) {
        e->valid = 1;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN PIPELINE (context-based!)
 * ══════════════════════════════════════════════════════════════════ */
static inline RouteTarget adaptive_process_v2(AdaptiveRouter *ar, uint64_t value)
{
    ar->total_ops++;
    
    uint16_t x = value & 0xFFFF;
    uint16_t y = (value >> 16) & 0xFFFF;
    
    uint32_t morton = morton2D(x, y);
    
    /* Fast path */
    uint32_t cached;
    if (fast_lookup(ar, morton, &cached)) {
        return ROUTE_MAIN;
    }
    
    /* Hilbert space */
    uint32_t hilbert = hilbert_index(x, y);
    
    /* Build signal (context-based!) */
    Signal sig = build_signal_v2(x, y, hilbert, ar->ctx.prev_hilbert, &ar->ctx);
    
    /* Route decision */
    RouteTarget route = route_decide_v2(&sig, &ar->ctx);
    
    /* Update anchor if stable pattern */
    if (route == ROUTE_MAIN || sig.pattern_score > ANCHOR_UPDATE_THRESH) {
        if (ar->ctx.anchor_locked == 0 ||
            ar->total_ops % 1000 == 0) {  /* Periodic re-anchor */
            anchor_update(&ar->ctx, x, y, sig.pattern_score);
            ar->anchor_updates++;
        }
    }
    
    /* Update context */
    ar->ctx.prev_hilbert = hilbert;
    
    /* Track stats */
    switch (route) {
        case ROUTE_MAIN:
            ar->main_routes++;
            fast_promote(ar, morton, (uint32_t)value);
            break;
        case ROUTE_GHOST:
            ar->ghost_routes++;
            break;
        case ROUTE_SHADOW:
            ar->shadow_routes++;
            break;
    }
    
    return route;
}

/* ══════════════════════════════════════════════════════════════════
 * INITIALIZATION
 * ══════════════════════════════════════════════════════════════════ */
static inline void adaptive_init_v2(AdaptiveRouter *ar)
{
    memset(ar, 0, sizeof(*ar));
    
    /* Pattern-driven weights (Hilbert > Mandel) */
    ar->ctx.weight_pattern = 0.5f;     /* Primary */
    ar->ctx.weight_stable = 0.3f;      /* Secondary */
    ar->ctx.weight_chaos_inv = 0.2f;   /* Tertiary */
    
    /* Initialize anchor to center */
    ar->ctx.anchor_x = 32768;
    ar->ctx.anchor_y = 32768;
    ar->ctx.anchor_locked = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * STATS
 * ══════════════════════════════════════════════════════════════════ */
static inline void adaptive_print_stats_v2(const AdaptiveRouter *ar)
{
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Adaptive Routing V2 Stats (Context-Based)    ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Total ops:       %10llu                    ║\n",
           (unsigned long long)ar->total_ops);
    printf("║ Fast hits:       %10llu (%.1f%%)            ║\n",
           (unsigned long long)ar->fast_hits,
           100.0 * ar->fast_hits / (ar->total_ops + 1));
    printf("║                                                ║\n");
    printf("║ MAIN routes:     %10llu (%.1f%%)            ║\n",
           (unsigned long long)ar->main_routes,
           100.0 * ar->main_routes / (ar->total_ops + 1));
    printf("║ GHOST routes:    %10llu (%.1f%%)            ║\n",
           (unsigned long long)ar->ghost_routes,
           100.0 * ar->ghost_routes / (ar->total_ops + 1));
    printf("║ SHADOW routes:   %10llu (%.1f%%)            ║\n",
           (unsigned long long)ar->shadow_routes,
           100.0 * ar->shadow_routes / (ar->total_ops + 1));
    printf("║                                                ║\n");
    printf("║ Anchor: (%5d, %5d) [%s]                   ║\n",
           ar->ctx.anchor_x, ar->ctx.anchor_y,
           ar->ctx.anchor_locked ? "locked" : "free");
    printf("║ Anchor updates:  %10llu                    ║\n",
           (unsigned long long)ar->anchor_updates);
    printf("║                                                ║\n");
    printf("║ Weights:                                       ║\n");
    printf("║   pattern: %.2f (primary)                      ║\n",
           ar->ctx.weight_pattern);
    printf("║   stable:  %.2f (secondary)                    ║\n",
           ar->ctx.weight_stable);
    printf("║   chaos⁻¹: %.2f (tertiary)                     ║\n",
           ar->ctx.weight_chaos_inv);
    printf("╚════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_ADAPTIVE_V2_H */
