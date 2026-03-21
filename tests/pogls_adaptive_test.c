/*
 * pogls_adaptive_test.c — Adaptive Routing System Test
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Stable pattern → MAIN routing
 *   2. Chaotic pattern → GHOST routing
 *   3. Fast path promotion
 *   4. Self-tuning weights
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "pogls_adaptive_routing.h"

/* ── Test 1: Stable Pattern ───────────────────────────────────── */
void test_stable_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 1: Stable Pattern (structured grid)     ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init(&ar);
    
    /* Structured 4x4 grid pattern (repeated) */
    uint16_t base_x = 1000;
    uint16_t base_y = 1000;
    
    for (int rep = 0; rep < 50; rep++) {
        for (int i = 0; i < 16; i++) {
            uint16_t x = base_x + (i % 4) * 10;
            uint16_t y = base_y + (i / 4) * 10;
            uint64_t value = ((uint64_t)y << 16) | x;
            
            RouteTarget route = adaptive_process(&ar, value);
            (void)route;
        }
    }
    
    adaptive_print_stats(&ar);
    
    printf("\nExpected: High MAIN routing (structured pattern)\n");
}

/* ── Test 2: Chaotic Pattern ──────────────────────────────────── */
void test_chaotic_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 2: Chaotic Pattern (random coords)      ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init(&ar);
    
    srand(42);
    
    /* Random coordinates (chaotic) */
    for (int i = 0; i < 1000; i++) {
        uint16_t x = rand() & 0xFFFF;
        uint16_t y = rand() & 0xFFFF;
        uint64_t value = ((uint64_t)y << 16) | x;
        
        RouteTarget route = adaptive_process(&ar, value);
        (void)route;
    }
    
    adaptive_print_stats(&ar);
    
    printf("\nExpected: Mixed routing (no clear pattern)\n");
}

/* ── Test 3: Fast Path Promotion ──────────────────────────────── */
void test_fast_promotion(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 3: Fast Path Promotion                   ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init(&ar);
    
    /* Structured pattern (4x2 grid) */
    uint64_t pattern[8];
    uint16_t base = 2000;
    for (int i = 0; i < 8; i++) {
        uint16_t x = base + (i % 4) * 5;
        uint16_t y = base + (i / 4) * 5;
        pattern[i] = ((uint64_t)y << 16) | x;
    }
    
    printf("Phase 1: Initial passes (learning)...\n");
    
    /* First 100 iterations (learning) */
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 8; j++) {
            adaptive_process(&ar, pattern[j]);
        }
    }
    
    printf("  Fast hits: %llu / %llu (%.1f%%)\n\n",
           (unsigned long long)ar.fast_hits,
           (unsigned long long)ar.total_ops,
           100.0 * ar.fast_hits / ar.total_ops);
    
    printf("Phase 2: After promotion (fast path)...\n");
    
    /* Next 100 iterations (should hit fast path) */
    uint64_t fast_before = ar.fast_hits;
    uint64_t ops_before = ar.total_ops;
    
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 8; j++) {
            adaptive_process(&ar, pattern[j]);
        }
    }
    
    uint64_t fast_gained = ar.fast_hits - fast_before;
    uint64_t ops_gained = ar.total_ops - ops_before;
    
    printf("  Fast hits: %llu / %llu (%.1f%%)\n\n",
           (unsigned long long)fast_gained,
           (unsigned long long)ops_gained,
           100.0 * fast_gained / ops_gained);
    
    adaptive_print_stats(&ar);
    
    printf("\nExpected: Fast hit rate increases over time\n");
}

/* ── Test 4: Self-Tuning ──────────────────────────────────────── */
void test_self_tuning(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 4: Self-Tuning Weights                   ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init(&ar);
    
    printf("Initial weights:\n");
    printf("  stable:  %.2f\n", ar.ctx.weight_stable);
    printf("  pattern: %.2f\n\n", ar.ctx.weight_pattern);
    
    /* Simulate feedback loop */
    printf("Applying positive feedback (10x)...\n");
    for (int i = 0; i < 10; i++) {
        adaptive_tune(&ar, 1.0f);
    }
    
    printf("  stable:  %.2f\n", ar.ctx.weight_stable);
    printf("  pattern: %.2f\n\n", ar.ctx.weight_pattern);
    
    printf("Applying negative feedback (10x)...\n");
    for (int i = 0; i < 10; i++) {
        adaptive_tune(&ar, -1.0f);
    }
    
    printf("  stable:  %.2f\n", ar.ctx.weight_stable);
    printf("  pattern: %.2f\n\n", ar.ctx.weight_pattern);
    
    printf("Expected: Weights adapt based on feedback\n");
}

/* ── Test 5: Route Distribution ───────────────────────────────── */
void test_route_distribution(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 5: Route Distribution (mixed patterns)  ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init(&ar);
    
    /* Mix of stable and chaotic */
    for (int i = 0; i < 1000; i++) {
        uint16_t x, y;
        
        if (i % 2 == 0) {
            /* Stable cluster */
            x = 500 + (i % 20);
            y = 500 + (i % 20);
        } else {
            /* Random (chaotic) */
            x = rand() & 0xFFFF;
            y = rand() & 0xFFFF;
        }
        
        uint64_t value = ((uint64_t)y << 16) | x;
        adaptive_process(&ar, value);
    }
    
    adaptive_print_stats(&ar);
    
    printf("\nExpected: Balanced distribution\n");
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  POGLS Adaptive Routing System Tests          ║\n");
    printf("║  Signal-Based • Weighted • Self-Tuning        ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    test_stable_pattern();
    test_chaotic_pattern();
    test_fast_promotion();
    test_self_tuning();
    test_route_distribution();
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  All Adaptive Routing Tests Complete          ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    return 0;
}
