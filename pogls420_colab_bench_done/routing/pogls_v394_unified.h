/*
 * pogls_v394_unified.h — POGLS V3.94 Unified System
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Combines:
 *   - Adaptive Routing (signal-based, context-aware)
 *   - L3 Quad Intersection (multi-view consensus)
 *   - Fast Path Optimization
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_V394_UNIFIED_H
#define POGLS_V394_UNIFIED_H
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

/* CONFIG */
#define MANDEL_MAX_ITER     32
#define ESCAPE_RADIUS       4.0f
#define OVERLAP_THRESHOLD   0.9f
#define MAIN_THRESHOLD      0.6f

/* ROUTE TARGET */
typedef enum {
    ROUTE_MAIN,
    ROUTE_GHOST,
    ROUTE_SHADOW
} RouteTarget;

/* SIGNAL */
typedef struct {
    float stable_score;
    float pattern_score;
    float chaos_score;
    float overlap_score;
} Signal;

/* CONTEXT */
typedef struct {
    uint32_t prev_hilbert;
    int16_t  anchor_x;
    int16_t  anchor_y;
    uint8_t  anchor_valid;
} Context;

/* QUAD VIEW */
typedef struct {
    float v[4];
    float intersection;
    float overlap;
    float residual;
} QuadView;

/* ENGINE */
typedef struct {
    Context ctx;
    uint64_t total_ops;
    uint64_t main_routes;
    uint64_t ghost_routes;
    uint64_t shadow_routes;
    uint64_t fast_skips;
} V394Engine;

/* MORTON */
static inline uint32_t morton2D(uint16_t x, uint16_t y) {
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) {
        m |= ((x & (1U << i)) << i) | ((y & (1U << i)) << (i + 1));
    }
    return m;
}

/* HILBERT */
static inline uint32_t hilbert_index(uint16_t x, uint16_t y) {
    uint32_t morton = morton2D(x, y);
    return morton ^ (morton >> 1);
}

/* LOCAL NORMALIZE */
static inline float normalize_local(int16_t v, int16_t center) {
    return ((float)(v - center) / 1024.0f);
}

/* MANDELBROT */
static inline float mandel_score(float cx, float cy) {
    float zx = 0.0f, zy = 0.0f;
    for (int i = 0; i < MANDEL_MAX_ITER; i++) {
        float zx2 = zx * zx, zy2 = zy * zy;
        if (zx2 + zy2 > ESCAPE_RADIUS) {
            float raw = (float)i;
            if (raw < 4.0f) return 0.0f;
            if (raw > 20.0f) return 1.0f;
            return raw / MANDEL_MAX_ITER;
        }
        float xt = zx2 - zy2 + cx;
        zy = 2.0f * zx * zy + cy;
        zx = xt;
    }
    return 1.0f;
}

/* QUAD PROBE */
static inline QuadView quad_probe(int16_t x, int16_t y,
                                   int16_t anchor_x, int16_t anchor_y) {
    QuadView q;
    
    q.v[0] = mandel_score(normalize_local(x + 1, anchor_x),
                          normalize_local(y, anchor_y));
    q.v[1] = mandel_score(normalize_local(x - 1, anchor_x),
                          normalize_local(y, anchor_y));
    q.v[2] = mandel_score(normalize_local(x, anchor_x),
                          normalize_local(y + 1, anchor_y));
    q.v[3] = mandel_score(normalize_local(x, anchor_x),
                          normalize_local(y - 1, anchor_y));
    
    q.intersection = q.v[0];
    for (int i = 1; i < 4; i++) {
        if (q.v[i] < q.intersection) q.intersection = q.v[i];
    }
    
    float mean = (q.v[0] + q.v[1] + q.v[2] + q.v[3]) * 0.25f;
    float variance = 0.0f;
    for (int i = 0; i < 4; i++) {
        float diff = q.v[i] - mean;
        variance += diff * diff;
    }
    variance = sqrtf(variance / 4.0f);
    
    q.overlap = (1.0f - fminf(variance * 2.0f, 1.0f)) * fminf(mean + 0.3f, 1.0f);
    q.residual = 1.0f - q.overlap;
    
    return q;
}

/* BUILD SIGNAL */
static inline Signal build_signal(uint32_t hilbert, uint32_t prev_hilbert,
                                   QuadView *quad) {
    Signal s;
    int diff = __builtin_popcount(hilbert ^ prev_hilbert);
    s.pattern_score = 1.0f - (diff / 32.0f);
    s.stable_score = quad->intersection;
    s.chaos_score = 1.0f - s.stable_score;
    s.overlap_score = quad->overlap;
    return s;
}

/* ROUTE DECIDE */
static inline RouteTarget route_decide(const Signal *s) {
    if (s->overlap_score > OVERLAP_THRESHOLD) {
        return ROUTE_MAIN;  /* Fast path */
    }
    
    float main_score = (s->overlap_score * 0.40f) +
                       (s->pattern_score * 0.35f) +
                       (s->stable_score * 0.25f);
    
    float ghost_score = s->chaos_score;
    
    if (main_score > MAIN_THRESHOLD) return ROUTE_MAIN;
    if (ghost_score > 0.6f) return ROUTE_GHOST;
    return ROUTE_SHADOW;
}

/* PROCESS */
static inline RouteTarget v394_process(V394Engine *eng, uint64_t value) {
    eng->total_ops++;
    
    int16_t x = (int16_t)(value & 0xFFFF);
    int16_t y = (int16_t)((value >> 16) & 0xFFFF);
    
    if (!eng->ctx.anchor_valid) {
        eng->ctx.anchor_x = x;
        eng->ctx.anchor_y = y;
        eng->ctx.anchor_valid = 1;
    }
    
    uint32_t hilbert = hilbert_index(x, y);
    QuadView quad = quad_probe(x, y, eng->ctx.anchor_x, eng->ctx.anchor_y);
    
    if (quad.overlap > OVERLAP_THRESHOLD) {
        eng->fast_skips++;
        eng->main_routes++;
        return ROUTE_MAIN;
    }
    
    Signal sig = build_signal(hilbert, eng->ctx.prev_hilbert, &quad);
    RouteTarget route = route_decide(&sig);
    
    eng->ctx.prev_hilbert = hilbert;
    
    if (route == ROUTE_MAIN && sig.pattern_score > 0.8f) {
        eng->ctx.anchor_x = (eng->ctx.anchor_x + x) / 2;
        eng->ctx.anchor_y = (eng->ctx.anchor_y + y) / 2;
    }
    
    switch (route) {
        case ROUTE_MAIN:   eng->main_routes++; break;
        case ROUTE_GHOST:  eng->ghost_routes++; break;
        case ROUTE_SHADOW: eng->shadow_routes++; break;
    }
    
    return route;
}

/* INIT */
static inline void v394_init(V394Engine *eng) {
    memset(eng, 0, sizeof(*eng));
}

/* STATS */
static inline void v394_print_stats(const V394Engine *eng) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  POGLS V3.94 Stats                             ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Total ops:       %10llu                    ║\n",
           (unsigned long long)eng->total_ops);
    printf("║ Fast skips:      %10llu (%.1f%%)            ║\n",
           (unsigned long long)eng->fast_skips,
           100.0 * eng->fast_skips / (eng->total_ops + 1));
    printf("║                                                ║\n");
    printf("║ MAIN routes:     %10llu (%.1f%%)            ║\n",
           (unsigned long long)eng->main_routes,
           100.0 * eng->main_routes / (eng->total_ops + 1));
    printf("║ GHOST routes:    %10llu (%.1f%%)            ║\n",
           (unsigned long long)eng->ghost_routes,
           100.0 * eng->ghost_routes / (eng->total_ops + 1));
    printf("║ SHADOW routes:   %10llu (%.1f%%)            ║\n",
           (unsigned long long)eng->shadow_routes,
           100.0 * eng->shadow_routes / (eng->total_ops + 1));
    printf("╚════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_V394_UNIFIED_H */
