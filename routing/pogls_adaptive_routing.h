/*
 * pogls_adaptive_routing.h — Signal-Based Adaptive Routing
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Design Philosophy:
 *   - Signal-based (not threshold-based)
 *   - Weighted decision (tunable)
 *   - No hard gates (smooth flow)
 *   - O(1) per input
 *   - Self-tuning ready
 *
 * Flow:
 *   INPUT → Morton → Hilbert → Mandelbrot → Signal → Route Decision
 *   → [MAIN | GHOST | SHADOW]
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_ADAPTIVE_ROUTING_H
#define POGLS_ADAPTIVE_ROUTING_H
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
 * CONFIG (single control point)
 * ══════════════════════════════════════════════════════════════════ */
#define MANDEL_MAX_ITER     32
#define ESCAPE_RADIUS       4.0f

#define MAIN_THRESHOLD      0.6f
#define GHOST_THRESHOLD     0.6f

#define SHADOW_TTL          64
#define BLACKLIST_TTL       256
#define TAIL_MASK           0xFF

/* Fast cache */
#define FAST_CACHE_SIZE     1024
#define PROMOTE_HIT_COUNT   3

/* ══════════════════════════════════════════════════════════════════
 * ROUTE TARGET
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    ROUTE_MAIN,      /* Dominant → main lane */
    ROUTE_GHOST,     /* Chaotic → ghost cache */
    ROUTE_SHADOW     /* Uncertain → shadow buffer */
} RouteTarget;

/* ══════════════════════════════════════════════════════════════════
 * SIGNAL (core metrics)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    float stable_score;   /* Mandelbrot stability (0-1) */
    float pattern_score;  /* Hilbert similarity (0-1) */
    float chaos_score;    /* Inverse of stable (0-1) */
} Signal;

/* ══════════════════════════════════════════════════════════════════
 * CONTEXT (state tracking)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t prev_hilbert;
    uint32_t prev_tail;
    uint32_t stability_count;
    
    /* Anchor system (local reference frame) */
    uint16_t anchor_x;
    uint16_t anchor_y;
    uint8_t  anchor_valid;
    
    /* Adaptive weights (self-tuning ready) */
    float weight_stable;
    float weight_pattern;
} Context;

/* ══════════════════════════════════════════════════════════════════
 * SHADOW NODE
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t value;
    uint32_t ttl;
    uint8_t  fake_flag;
    uint8_t  pad[3];
} ShadowNode;

/* ══════════════════════════════════════════════════════════════════
 * FAST CACHE (L1-friendly)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t key;
    uint32_t value;
    uint8_t  hit_count;
    uint8_t  valid;
    uint8_t  pad[2];
} FastEntry;

/* ══════════════════════════════════════════════════════════════════
 * ADAPTIVE ROUTING ENGINE
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    Context      ctx;
    FastEntry    fast_cache[FAST_CACHE_SIZE];
    ShadowNode   shadow[4096];
    uint32_t     shadow_head;
    
    /* Stats */
    uint64_t     total_ops;
    uint64_t     main_routes;
    uint64_t     ghost_routes;
    uint64_t     shadow_routes;
    uint64_t     fast_hits;
} AdaptiveRouter;

/* ══════════════════════════════════════════════════════════════════
 * SPACE MAPPING — Morton Encoding
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t morton2D(uint16_t x, uint16_t y)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) {
        m |= ((x & (1U << i)) << i) | ((y & (1U << i)) << (i + 1));
    }
    return m;
}

/* ══════════════════════════════════════════════════════════════════
 * PATTERN SENSE — Hilbert Curve
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t hilbert_index(uint16_t x, uint16_t y)
{
    /* Simplified Hilbert (Gray code approximation) */
    uint32_t morton = morton2D(x, y);
    return morton ^ (morton >> 1);
}

static inline float hilbert_similarity(uint32_t a, uint32_t b)
{
    /* Hamming distance → similarity score */
    int diff = __builtin_popcount(a ^ b);
    return 1.0f - (diff / 32.0f);
}

/* ══════════════════════════════════════════════════════════════════
 * LOCAL NORMALIZATION (context-based)
 * ══════════════════════════════════════════════════════════════════ */
static inline float normalize_local(uint16_t v, uint16_t center)
{
    /* Map to local window around anchor */
    return ((float)((int)v - (int)center) / 1024.0f);
}

/* ══════════════════════════════════════════════════════════════════
 * CHAOS DETECTOR — Mandelbrot
 * ══════════════════════════════════════════════════════════════════ */
static inline float mandel_score(float cx, float cy)
{
    float zx = 0.0f, zy = 0.0f;
    
    for (int i = 0; i < MANDEL_MAX_ITER; i++) {
        float zx2 = zx * zx;
        float zy2 = zy * zy;
        
        if (zx2 + zy2 > ESCAPE_RADIUS) {
            /* Escaped → chaotic (with clamping) */
            float raw_val = (float)i;
            
            /* Clamp chaos sensitivity */
            if (raw_val < 4.0f) {
                return 0.0f;  /* Very chaotic */
            } else if (raw_val > 20.0f) {
                return 1.0f;  /* Very stable */
            } else {
                return raw_val / MANDEL_MAX_ITER;
            }
        }
        
        float xt = zx2 - zy2 + cx;
        zy = 2.0f * zx * zy + cy;
        zx = xt;
    }
    
    /* Did not escape → stable */
    return 1.0f;
}

/* ══════════════════════════════════════════════════════════════════
 * TAIL EXTRACTION (evolution axis)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t extract_tail(uint64_t value)
{
    return (uint32_t)(value & TAIL_MASK);
}

/* ══════════════════════════════════════════════════════════════════
 * SIGNAL BUILDER (heart of system)
 * ══════════════════════════════════════════════════════════════════ */
static inline Signal build_signal(uint32_t hilbert,
                                    uint32_t prev_hilbert,
                                    float mandel_val)
{
    Signal s;
    
    /* Stability from Mandelbrot */
    s.stable_score = mandel_val;
    
    /* Pattern from Hilbert continuity */
    s.pattern_score = hilbert_similarity(hilbert, prev_hilbert);
    
    /* Chaos = inverse of stable */
    s.chaos_score = 1.0f - s.stable_score;
    
    return s;
}

/* ══════════════════════════════════════════════════════════════════
 * MENDEL ROUTER (weighted decision)
 * ══════════════════════════════════════════════════════════════════ */
static inline RouteTarget route_decide(const Signal *s, const Context *ctx)
{
    (void)ctx;  /* reserved for future context-aware routing */
    /* Rebalanced weights: Pattern leads, Mandel supports */
    float main_score = (s->pattern_score * 0.5f) +
                       (s->stable_score * 0.3f) +
                       ((1.0f - s->chaos_score) * 0.2f);
    
    float ghost_score = (s->chaos_score * 0.7f) +
                        ((1.0f - s->pattern_score) * 0.3f);
    
    /* Decision (smooth, no hard gate) */
    if (main_score > MAIN_THRESHOLD) {
        return ROUTE_MAIN;
    }
    
    if (ghost_score > GHOST_THRESHOLD) {
        return ROUTE_GHOST;
    }
    
    return ROUTE_SHADOW;
}

/* ══════════════════════════════════════════════════════════════════
 * FAST CACHE (17ns path)
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

static inline void fast_store(AdaptiveRouter *ar, uint32_t key, uint32_t value)
{
    uint32_t idx = fast_hash(key);
    FastEntry *e = &ar->fast_cache[idx];
    
    e->key = key;
    e->value = value;
    e->hit_count = 1;
    e->valid = 1;
}

static inline void fast_promote(AdaptiveRouter *ar, uint32_t key, uint32_t value)
{
    uint32_t idx = fast_hash(key);
    FastEntry *e = &ar->fast_cache[idx];
    
    /* Initialize or increment */
    if (e->key == key && e->hit_count > 0) {
        /* Existing entry — increment */
        e->hit_count++;
    } else {
        /* New entry — initialize */
        e->key = key;
        e->value = value;
        e->hit_count = 1;
        e->valid = 0;  /* Not promoted yet */
    }
    
    /* Promote to fast cache after N hits */
    if (e->hit_count >= PROMOTE_HIT_COUNT) {
        e->valid = 1;  /* Now accessible via fast path! */
    }
}

/* ══════════════════════════════════════════════════════════════════
 * SHADOW BUFFER
 * ══════════════════════════════════════════════════════════════════ */
static inline void shadow_push(AdaptiveRouter *ar, uint64_t value)
{
    uint32_t idx = ar->shadow_head % 4096;
    ShadowNode *node = &ar->shadow[idx];
    
    node->value = value;
    node->ttl = SHADOW_TTL;
    node->fake_flag = 0;
    
    ar->shadow_head++;
}

static inline void shadow_tick(AdaptiveRouter *ar)
{
    /* Age all shadow entries */
    for (int i = 0; i < 4096; i++) {
        if (ar->shadow[i].ttl > 0) {
            ar->shadow[i].ttl--;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN PIPELINE (process one value)
 * ══════════════════════════════════════════════════════════════════ */
static inline RouteTarget adaptive_process(AdaptiveRouter *ar, uint64_t value)
{
    ar->total_ops++;
    
    /* Extract coordinates */
    uint16_t x = value & 0xFFFF;
    uint16_t y = (value >> 16) & 0xFFFF;
    
    /* Initialize anchor on first run */
    if (!ar->ctx.anchor_valid) {
        ar->ctx.anchor_x = x;
        ar->ctx.anchor_y = y;
        ar->ctx.anchor_valid = 1;
    }
    
    /* Generate key */
    uint32_t morton = morton2D(x, y);
    
    /* ── FAST PATH ──────────────────────────────────────────────── */
    uint32_t cached;
    if (fast_lookup(ar, morton, &cached)) {
        return ROUTE_MAIN;  /* Fast hit = main route */
    }
    
    /* ── SLOW PATH (signal-based) ───────────────────────────────── */
    
    /* Map to Hilbert space */
    uint32_t hilbert = hilbert_index(x, y);
    
    /* Map to LOCAL complex plane (relative to anchor) */
    float cx = normalize_local(x, ar->ctx.anchor_x);
    float cy = normalize_local(y, ar->ctx.anchor_y);
    
    /* Mandelbrot chaos check (local context) */
    float mandel = mandel_score(cx, cy);
    
    /* Build signal */
    Signal sig = build_signal(hilbert, ar->ctx.prev_hilbert, mandel);
    
    /* Route decision */
    RouteTarget route = route_decide(&sig, &ar->ctx);
    
    /* Update anchor if stable pattern detected */
    if (route == ROUTE_MAIN && sig.pattern_score > 0.8f) {
        /* Pattern is stable → update anchor to center */
        ar->ctx.anchor_x = (ar->ctx.anchor_x + x) / 2;
        ar->ctx.anchor_y = (ar->ctx.anchor_y + y) / 2;
    }
    
    /* Update context */
    ar->ctx.prev_hilbert = hilbert;
    ar->ctx.prev_tail = extract_tail(value);
    
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
            shadow_push(ar, value);
            break;
    }
    
    return route;
}

/* ══════════════════════════════════════════════════════════════════
 * INITIALIZATION
 * ══════════════════════════════════════════════════════════════════ */
static inline void adaptive_init(AdaptiveRouter *ar)
{
    memset(ar, 0, sizeof(*ar));
    
    /* Default weights */
    ar->ctx.weight_stable = 0.6f;
    ar->ctx.weight_pattern = 0.4f;
}

/* ══════════════════════════════════════════════════════════════════
 * SELF-TUNING (adaptive weights)
 * ══════════════════════════════════════════════════════════════════ */
static inline void adaptive_tune(AdaptiveRouter *ar, float feedback)
{
    /* Feedback: +1 = good route, -1 = bad route, 0 = neutral */
    
    if (feedback > 0) {
        /* Reinforce current weights */
        ar->ctx.weight_stable *= 1.01f;
    } else if (feedback < 0) {
        /* Reduce current weights */
        ar->ctx.weight_stable *= 0.99f;
    }
    
    /* Keep weights bounded */
    if (ar->ctx.weight_stable > 0.9f) ar->ctx.weight_stable = 0.9f;
    if (ar->ctx.weight_stable < 0.1f) ar->ctx.weight_stable = 0.1f;
    
    /* Pattern weight = complement */
    ar->ctx.weight_pattern = 1.0f - ar->ctx.weight_stable;
}

/* ══════════════════════════════════════════════════════════════════
 * STATS
 * ══════════════════════════════════════════════════════════════════ */
static inline void adaptive_print_stats(const AdaptiveRouter *ar)
{
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Adaptive Routing Stats                        ║\n");
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
    printf("║ Weights:                                       ║\n");
    printf("║   stable:  %.2f                                ║\n",
           ar->ctx.weight_stable);
    printf("║   pattern: %.2f                                ║\n",
           ar->ctx.weight_pattern);
    printf("╚════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_ADAPTIVE_ROUTING_H */
