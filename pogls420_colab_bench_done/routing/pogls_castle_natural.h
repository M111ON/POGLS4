/*
 * pogls_castle_natural.h — Infinity Castle + Natural Pattern Engine
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Combines:
 *   - Infinity Castle (SOE — trace/collapse/ghost)
 *   - Natural Patterns (Fibonacci/Golden/Mandelbrot/Hilbert)
 *   - Mendel Selection (dominant/recessive routing)
 *
 * Philosophy:
 *   "ใช้ธรรมชาติเป็น heuristic engine"
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_CASTLE_NATURAL_H
#define POGLS_CASTLE_NATURAL_H
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
 * Natural Constants
 * ══════════════════════════════════════════════════════════════════ */
#define PHI             1.618033988749895   /* Golden ratio */
#define PHI_INV         0.618033988749895   /* 1/φ */

#define MANDEL_MAX_ITER 8
#define MANDEL_ESCAPE   4.0f

#define FIB_MAX         14                  /* indices 0..13 = 14 elements */

/* ══════════════════════════════════════════════════════════════════
 * Fibonacci LUT (precomputed)
 * ══════════════════════════════════════════════════════════════════ */
static const uint16_t FIB_LUT[FIB_MAX] = {
    0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233
};

static inline uint16_t fib(int n) {
    return (n >= 0 && n < FIB_MAX) ? FIB_LUT[n] : 0;
}

/* ══════════════════════════════════════════════════════════════════
 * Hilbert Curve (Morton → Hilbert transformation)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t morton_encode(uint16_t x, uint16_t y)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) {
        m |= ((x & (1U << i)) << i) | ((y & (1U << i)) << (i + 1));
    }
    return m;
}

/* Simplified Hilbert (XOR approximation for speed) */
static inline uint32_t hilbert_approx(uint32_t morton)
{
    return morton ^ (morton >> 1);  /* Gray code approximation */
}

/* ══════════════════════════════════════════════════════════════════
 * Mandelbrot Stability Check
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t stable;       /* 1 = stable (in set), 0 = chaotic */
    uint8_t iterations;   /* escape time */
} MandelResult;

static inline MandelResult mandel_check(float cx, float cy)
{
    MandelResult r = {0, 0};
    
    float zx = 0.0f, zy = 0.0f;
    
    for (int i = 0; i < MANDEL_MAX_ITER; i++) {
        float zx2 = zx * zx;
        float zy2 = zy * zy;
        
        if (zx2 + zy2 > MANDEL_ESCAPE) {
            r.stable = 0;      /* Chaotic (escaped) */
            r.iterations = i;
            return r;
        }
        
        float xt = zx2 - zy2 + cx;
        zy = 2.0f * zx * zy + cy;
        zx = xt;
    }
    
    r.stable = 1;              /* Stable (in set) */
    r.iterations = MANDEL_MAX_ITER;
    return r;
}

/* ══════════════════════════════════════════════════════════════════
 * Mendel Selection (Dominant/Recessive routing)
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    ROUTE_MAIN,      /* Dominant → main lane */
    ROUTE_GHOST,     /* Recessive → ghost cache */
    ROUTE_SHADOW     /* Unstable → shadow buffer */
} RouteTarget;

/* Extract tail signature (for Mendel comparison) */
static inline uint32_t extract_tail(uint64_t value)
{
    uint32_t v32 = (uint32_t)(value ^ (value >> 32));
    return v32 ^ (v32 >> 16);
}

/* Mendel routing decision */
static inline RouteTarget mendel_route(MandelResult mandel, 
                                        uint32_t stability_score)
{
    if (mandel.stable && stability_score > 2) {
        return ROUTE_MAIN;      /* Dominant trait */
    } else if (!mandel.stable || stability_score == 1) {
        return ROUTE_GHOST;     /* Recessive trait */
    } else {
        return ROUTE_SHADOW;    /* Unstable */
    }
}

/* ══════════════════════════════════════════════════════════════════
 * Golden Ratio Scrambling (anti-collision)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t golden_scramble(uint64_t addr, uint8_t depth)
{
    /* φ-based quasi-random distribution */
    double scaled = (double)addr * pow(PHI, depth % 8);
    return (uint64_t)scaled;
}

/* ══════════════════════════════════════════════════════════════════
 * Pattern Group (4n accumulator)
 * ══════════════════════════════════════════════════════════════════ */
#define GROUP_SIZE 4

typedef struct {
    uint64_t patterns[GROUP_SIZE];
    uint8_t  count;
} PatternGroup;

/* Extract common core (bitwise AND) */
static inline uint64_t extract_core(const PatternGroup *group)
{
    if (group->count == 0) return 0;
    
    uint64_t core = group->patterns[0];
    for (int i = 1; i < group->count; i++) {
        core &= group->patterns[i];  /* Common bits */
    }
    
    return core;
}

/* ══════════════════════════════════════════════════════════════════
 * Natural Pattern Engine State
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* Pattern tracking */
    PatternGroup group;
    uint32_t     prev_tail;
    uint8_t      stability_count;
    uint8_t      frequency;
    
    /* Zone tracking (Hilbert gate) */
    uint32_t     prev_zone;
    
    /* Stats */
    uint64_t     patterns_seen;
    uint64_t     stable_patterns;
    uint64_t     chaotic_patterns;
    uint64_t     ghost_stores;
} NaturalEngine;

/* ══════════════════════════════════════════════════════════════════
 * Full Pipeline: Natural Pattern → Mendel → Route
 * ══════════════════════════════════════════════════════════════════ */

/* Initialize natural engine */
static inline void natural_init(NaturalEngine *ne)
{
    memset(ne, 0, sizeof(*ne));
}

/* Process one value through natural pattern pipeline */
static inline RouteTarget natural_process(NaturalEngine *ne,
                                           uint16_t x,
                                           uint16_t y,
                                           uint64_t value)
{
    ne->patterns_seen++;
    
    /* ── STEP 1: Hilbert Zone Gate ──────────────────────────────── */
    uint32_t morton = morton_encode(x, y);
    uint32_t hilbert = hilbert_approx(morton);
    uint32_t zone = hilbert >> 4;  /* 16-zone grid */
    
    if (zone != ne->prev_zone) {
        ne->prev_zone = zone;
        return ROUTE_SHADOW;  /* Zone transition → shadow */
    }
    
    /* ── STEP 2: Tail Signature (Mendel) ────────────────────────── */
    uint32_t tail = extract_tail(value);
    
    ne->frequency++;
    
    if (tail == ne->prev_tail) {
        ne->stability_count++;
    } else {
        ne->stability_count = 0;
        ne->prev_tail = tail;
    }
    
    /* Need minimum frequency & stability */
    if (ne->frequency < 3 || ne->stability_count < 2) {
        return ROUTE_SHADOW;  /* Not ready */
    }
    
    /* ── STEP 3: Group Accumulation (4n) ────────────────────────── */
    if (ne->group.count < GROUP_SIZE) {
        ne->group.patterns[ne->group.count++] = value;
        return ROUTE_SHADOW;  /* Still accumulating */
    }
    
    /* ── STEP 4: Extract Core ───────────────────────────────────── */
    uint64_t core = extract_core(&ne->group);
    
    /* ── STEP 5: Mandelbrot Chaos Check ─────────────────────────── */
    /* Map core to complex plane [-2, 2] */
    float cx = ((float)(core & 0xFFFF) / 65535.0f) * 4.0f - 2.0f;
    float cy = ((float)((core >> 16) & 0xFFFF) / 65535.0f) * 4.0f - 2.0f;
    
    MandelResult mandel = mandel_check(cx, cy);
    
    if (mandel.stable) {
        ne->stable_patterns++;
    } else {
        ne->chaotic_patterns++;
    }
    
    /* ── STEP 6: Mendel Routing Decision ────────────────────────── */
    RouteTarget route = mendel_route(mandel, ne->stability_count);
    
    /* ── STEP 7: Reset Group ────────────────────────────────────── */
    ne->group.count = 0;
    
    if (route == ROUTE_GHOST) {
        ne->ghost_stores++;
    }
    
    return route;
}

/* ── Stats ────────────────────────────────────────────────────── */
static inline void natural_print_stats(const NaturalEngine *ne)
{
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Natural Pattern Engine Stats                  ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Patterns seen:   %10llu                    ║\n",
           (unsigned long long)ne->patterns_seen);
    printf("║ Stable (Main):   %10llu (%.1f%%)            ║\n",
           (unsigned long long)ne->stable_patterns,
           100.0 * ne->stable_patterns / (ne->patterns_seen + 1));
    printf("║ Chaotic (Ghost): %10llu (%.1f%%)            ║\n",
           (unsigned long long)ne->chaotic_patterns,
           100.0 * ne->chaotic_patterns / (ne->patterns_seen + 1));
    printf("║ Ghost stores:    %10llu                    ║\n",
           (unsigned long long)ne->ghost_stores);
    printf("╚════════════════════════════════════════════════╝\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Integration Helpers (for POGLS pipeline)
 * ══════════════════════════════════════════════════════════════════ */

/* Get Fibonacci spacing for depth */
static inline uint16_t get_fib_spacing(uint8_t depth)
{
    return fib(depth % FIB_MAX);
}

/* Get golden-ratio scattered index */
static inline uint32_t get_golden_index(uint32_t base, uint8_t n, uint32_t modulo)
{
    return (uint32_t)(base * pow(PHI, n)) % modulo;
}

#endif /* POGLS_CASTLE_NATURAL_H */
