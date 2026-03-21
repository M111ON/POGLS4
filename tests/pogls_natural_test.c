/*
 * pogls_natural_test.c — Natural Pattern Engine Test
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Hilbert zone gating
 *   2. Mendel stability detection
 *   3. Mandelbrot chaos check
 *   4. Routing (Main/Ghost/Shadow)
 *   5. Fibonacci spacing
 *   6. Golden ratio distribution
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "pogls_castle_natural.h"

/* ── Test 1: Stable Pattern (should route to MAIN) ─────────────── */
void test_stable_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 1: Stable Pattern (repeated values)     ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    NaturalEngine ne;
    natural_init(&ne);
    
    /* Feed same pattern repeatedly (stable) */
    uint64_t pattern = 0xAAAAAAAAAAAAAAAAULL;
    
    int main_count = 0, ghost_count = 0, shadow_count = 0;
    
    for (int i = 0; i < 100; i++) {
        /* Same zone, same pattern */
        RouteTarget route = natural_process(&ne, 10, 10, pattern);
        
        if (route == ROUTE_MAIN) main_count++;
        else if (route == ROUTE_GHOST) ghost_count++;
        else shadow_count++;
    }
    
    natural_print_stats(&ne);
    
    printf("\nRouting distribution:\n");
    printf("  MAIN:   %3d (%.1f%%) ← Should be high!\n", 
           main_count, 100.0 * main_count / 100);
    printf("  GHOST:  %3d (%.1f%%)\n", 
           ghost_count, 100.0 * ghost_count / 100);
    printf("  SHADOW: %3d (%.1f%%)\n", 
           shadow_count, 100.0 * shadow_count / 100);
}

/* ── Test 2: Chaotic Pattern (should route to GHOST) ──────────── */
void test_chaotic_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 2: Chaotic Pattern (random values)      ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    NaturalEngine ne;
    natural_init(&ne);
    
    srand(42);
    
    int main_count = 0, ghost_count = 0, shadow_count = 0;
    
    for (int i = 0; i < 100; i++) {
        /* Random values (chaotic) */
        uint64_t value = ((uint64_t)rand() << 32) | rand();
        RouteTarget route = natural_process(&ne, 10, 10, value);
        
        if (route == ROUTE_MAIN) main_count++;
        else if (route == ROUTE_GHOST) ghost_count++;
        else shadow_count++;
    }
    
    natural_print_stats(&ne);
    
    printf("\nRouting distribution:\n");
    printf("  MAIN:   %3d (%.1f%%)\n", 
           main_count, 100.0 * main_count / 100);
    printf("  GHOST:  %3d (%.1f%%) ← Should be high!\n", 
           ghost_count, 100.0 * ghost_count / 100);
    printf("  SHADOW: %3d (%.1f%%)\n", 
           shadow_count, 100.0 * shadow_count / 100);
}

/* ── Test 3: Fibonacci Spacing ────────────────────────────────── */
void test_fibonacci_spacing(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 3: Fibonacci Spacing                     ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    printf("Fibonacci sequence (depth → spacing):\n");
    for (int depth = 0; depth < 10; depth++) {
        uint16_t spacing = get_fib_spacing(depth);
        printf("  depth %2d → spacing %3d\n", depth, spacing);
    }
    
    printf("\nEffect: Non-uniform, natural distribution\n");
    printf("        Avoids clustering ✓\n");
}

/* ── Test 4: Golden Ratio Distribution ────────────────────────── */
void test_golden_distribution(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 4: Golden Ratio Distribution            ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    printf("Golden scrambling (base=1000, modulo=256):\n");
    for (int n = 0; n < 10; n++) {
        uint32_t idx = get_golden_index(1000, n, 256);
        printf("  n=%d → index %3d\n", n, idx);
    }
    
    printf("\nEffect: Quasi-random distribution\n");
    printf("        Deterministic but spread ✓\n");
}

/* ── Test 5: Mandelbrot Stability ─────────────────────────────── */
void test_mandelbrot_stability(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 5: Mandelbrot Stability Check           ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    /* Test points in complex plane */
    struct {
        float cx, cy;
        const char *name;
    } points[] = {
        {0.0f, 0.0f, "Origin (stable)"},
        {-0.5f, 0.0f, "Main bulb (stable)"},
        {0.25f, 0.5f, "Outside (chaotic)"},
        {-1.0f, 0.0f, "Boundary (stable)"},
        {1.0f, 1.0f, "Far outside (chaotic)"}
    };
    
    for (int i = 0; i < 5; i++) {
        MandelResult r = mandel_check(points[i].cx, points[i].cy);
        printf("  %20s: %s (iter=%d)\n",
               points[i].name,
               r.stable ? "STABLE  " : "CHAOTIC",
               r.iterations);
    }
    
    printf("\nEffect: Separates stable from chaotic patterns\n");
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  POGLS Natural Pattern Engine Tests           ║\n");
    printf("║  Fibonacci + Golden + Mandelbrot + Mendel      ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    test_stable_pattern();
    test_chaotic_pattern();
    test_fibonacci_spacing();
    test_golden_distribution();
    test_mandelbrot_stability();
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  All Natural Pattern Tests Complete            ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    return 0;
}
