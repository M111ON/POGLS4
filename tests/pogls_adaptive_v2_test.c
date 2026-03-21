/*
 * pogls_adaptive_v2_test.c — Context-Based Adaptive Routing Test
 * ══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "pogls_adaptive_v2.h"

/* ── Test 1: Structured Pattern (grid, not linear!) ───────────── */
void test_structured_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 1: Structured Pattern (4×4 grid)        ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init_v2(&ar);
    
    uint16_t base_x = 32768;
    uint16_t base_y = 32768;
    
    /* 4x4 grid pattern (structured, not linear) */
    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < 16; i++) {
            uint16_t x = base_x + (i % 4) * 10;
            uint16_t y = base_y + (i / 4) * 10;
            uint64_t value = ((uint64_t)y << 16) | x;
            
            adaptive_process_v2(&ar, value);
        }
    }
    
    adaptive_print_stats_v2(&ar);
    
    printf("\nExpected: High MAIN (grid has structure)\n");
}

/* ── Test 2: Circular Pattern (organic structure) ─────────────── */
void test_circular_pattern(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 2: Circular Pattern (sin/cos)           ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init_v2(&ar);
    
    uint16_t base_x = 32768;
    uint16_t base_y = 32768;
    uint16_t radius = 100;
    
    /* Circular pattern */
    for (int iter = 0; iter < 100; iter++) {
        for (int angle = 0; angle < 360; angle += 30) {
            float rad = angle * M_PI / 180.0f;
            uint16_t x = base_x + (uint16_t)(cos(rad) * radius);
            uint16_t y = base_y + (uint16_t)(sin(rad) * radius);
            uint64_t value = ((uint64_t)y << 16) | x;
            
            adaptive_process_v2(&ar, value);
        }
    }
    
    adaptive_print_stats_v2(&ar);
    
    printf("\nExpected: High MAIN (circle has pattern)\n");
}

/* ── Test 3: Random Chaos ─────────────────────────────────────── */
void test_random_chaos(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 3: Random Chaos (no structure)          ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init_v2(&ar);
    
    srand(42);
    
    /* Completely random */
    for (int i = 0; i < 1000; i++) {
        uint16_t x = rand() & 0xFFFF;
        uint16_t y = rand() & 0xFFFF;
        uint64_t value = ((uint64_t)y << 16) | x;
        
        adaptive_process_v2(&ar, value);
    }
    
    adaptive_print_stats_v2(&ar);
    
    printf("\nExpected: High GHOST (chaotic)\n");
}

/* ── Test 4: Fast Path Promotion ──────────────────────────────── */
void test_fast_promotion_v2(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 4: Fast Path Promotion (structured)     ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init_v2(&ar);
    
    uint16_t base = 32768;
    
    printf("Phase 1: Learning (first 50 iterations)...\n");
    
    /* Grid pattern */
    for (int iter = 0; iter < 50; iter++) {
        for (int i = 0; i < 16; i++) {
            uint16_t x = base + (i % 4) * 10;
            uint16_t y = base + (i / 4) * 10;
            uint64_t value = ((uint64_t)y << 16) | x;
            adaptive_process_v2(&ar, value);
        }
    }
    
    printf("  Fast: %llu / %llu (%.1f%%)\n",
           (unsigned long long)ar.fast_hits,
           (unsigned long long)ar.total_ops,
           100.0 * ar.fast_hits / ar.total_ops);
    
    printf("\nPhase 2: After promotion (next 50 iterations)...\n");
    
    uint64_t fast_before = ar.fast_hits;
    uint64_t ops_before = ar.total_ops;
    
    for (int iter = 0; iter < 50; iter++) {
        for (int i = 0; i < 16; i++) {
            uint16_t x = base + (i % 4) * 10;
            uint16_t y = base + (i / 4) * 10;
            uint64_t value = ((uint64_t)y << 16) | x;
            adaptive_process_v2(&ar, value);
        }
    }
    
    uint64_t fast_gained = ar.fast_hits - fast_before;
    uint64_t ops_gained = ar.total_ops - ops_before;
    
    printf("  Fast: %llu / %llu (%.1f%%)\n\n",
           (unsigned long long)fast_gained,
           (unsigned long long)ops_gained,
           100.0 * fast_gained / ops_gained);
    
    adaptive_print_stats_v2(&ar);
    
    printf("\nExpected: Fast path increases dramatically\n");
}

/* ── Test 5: Anchor Locking ───────────────────────────────────── */
void test_anchor_locking(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 5: Anchor System (reference locking)    ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    AdaptiveRouter ar;
    adaptive_init_v2(&ar);
    
    printf("Initial anchor: (%d, %d)\n", ar.ctx.anchor_x, ar.ctx.anchor_y);
    
    /* Feed structured pattern */
    uint16_t center_x = 40000;
    uint16_t center_y = 40000;
    
    for (int i = 0; i < 100; i++) {
        uint16_t x = center_x + (i % 4) * 10;
        uint16_t y = center_y + (i / 4) * 10;
        uint64_t value = ((uint64_t)y << 16) | x;
        adaptive_process_v2(&ar, value);
    }
    
    printf("After 100 ops: (%d, %d) [updates: %llu]\n",
           ar.ctx.anchor_x, ar.ctx.anchor_y,
           (unsigned long long)ar.anchor_updates);
    
    adaptive_print_stats_v2(&ar);
    
    printf("\nExpected: Anchor moves to pattern center\n");
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  POGLS Adaptive Routing V2 Tests              ║\n");
    printf("║  Context-Based • Local Window • Anchor        ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    test_structured_pattern();
    test_circular_pattern();
    test_random_chaos();
    test_fast_promotion_v2();
    test_anchor_locking();
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  All V2 Tests Complete                         ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    return 0;
}
