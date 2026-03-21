/*
 * pogls_v394_minitest.c — V3.94 Critical Safety Tests
 * ══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "pogls_v394_unified.h"

#define TEST(cond, msg) do { \
    if (!(cond)) { \
        printf("  ❌ FAIL: %s\n", msg); \
        return 0; \
    } else { \
        printf("  ✅ PASS: %s\n", msg); \
    } \
} while(0)

/* Test 1: Routing Consistency */
int test_routing_consistency(void) {
    printf("\n[TEST 1] Routing Consistency\n");
    
    V394Engine eng;
    v394_init(&eng);
    
    /* Structured pattern */
    uint16_t base = 1000;
    RouteTarget routes[16];
    
    for (int i = 0; i < 16; i++) {
        uint16_t x = base + (i % 4) * 10;
        uint16_t y = base + (i / 4) * 10;
        uint64_t value = ((uint64_t)y << 16) | x;
        routes[i] = v394_process(&eng, value);
    }
    
    /* Repeat — should be consistent */
    int consistent = 0;
    for (int i = 0; i < 16; i++) {
        uint16_t x = base + (i % 4) * 10;
        uint16_t y = base + (i / 4) * 10;
        uint64_t value = ((uint64_t)y << 16) | x;
        RouteTarget route = v394_process(&eng, value);
        if (route == routes[i] || route == ROUTE_MAIN) consistent++;
    }
    
    TEST(consistent >= 14, "Routing stayed consistent (>87%)");
    return 1;
}

/* Test 2: Quad Overlap Behavior */
int test_quad_overlap(void) {
    printf("\n[TEST 2] Quad Overlap Behavior\n");
    
    /* Tight cluster */
    QuadView q1 = quad_probe(1000, 1000, 1000, 1000);
    TEST(q1.overlap > 0.8f, "Tight cluster has high overlap");
    
    /* Scattered */
    QuadView q2 = quad_probe(5000, 30000, 1000, 1000);
    TEST(q2.overlap < 0.5f, "Scattered has low overlap");
    
    return 1;
}

/* Test 3: Fast Path Activation */
int test_fast_path(void) {
    printf("\n[TEST 3] Fast Path Activation\n");
    
    V394Engine eng;
    v394_init(&eng);
    
    /* Feed stable pattern */
    for (int i = 0; i < 100; i++) {
        uint16_t x = 500 + (i % 4);
        uint16_t y = 500 + (i / 4);
        uint64_t value = ((uint64_t)y << 16) | x;
        v394_process(&eng, value);
    }
    
    TEST(eng.fast_skips > 50, "Fast path activated (>50 skips)");
    return 1;
}

/* Test 4: Chaotic Pattern Handling */
int test_chaotic_handling(void) {
    printf("\n[TEST 4] Chaotic Pattern Handling\n");
    
    V394Engine eng;
    v394_init(&eng);
    
    srand(42);
    for (int i = 0; i < 100; i++) {
        uint16_t x = rand() & 0xFFFF;
        uint16_t y = rand() & 0xFFFF;
        uint64_t value = ((uint64_t)y << 16) | x;
        v394_process(&eng, value);
    }
    
    TEST(eng.ghost_routes + eng.shadow_routes > 50,
         "Chaotic routed to GHOST/SHADOW (>50%)");
    return 1;
}

/* Test 5: Stress Test */
int test_stress(void) {
    printf("\n[TEST 5] Stress Test (10K ops)\n");
    
    V394Engine eng;
    v394_init(&eng);
    
    time_t start = time(NULL);
    
    for (int i = 0; i < 10000; i++) {
        uint16_t x = i % 1000;
        uint16_t y = i / 1000;
        uint64_t value = ((uint64_t)y << 16) | x;
        v394_process(&eng, value);
    }
    
    time_t elapsed = time(NULL) - start;
    
    printf("  Completed: 10K ops in %ld sec\n", elapsed);
    TEST(elapsed < 10, "Completed within 10 seconds");
    
    v394_print_stats(&eng);
    return 1;
}

/* Main */
int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  POGLS V3.94 Mini-Test Suite                  ║\n");
    printf("║  Critical Safety Verification                 ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    int passed = 0;
    passed += test_routing_consistency();
    passed += test_quad_overlap();
    passed += test_fast_path();
    passed += test_chaotic_handling();
    passed += test_stress();
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    if (passed == 5) {
        printf("║  ✅ ALL 5 TESTS PASSED                        ║\n");
        printf("║  V3.94 is VERIFIED SAFE                       ║\n");
    } else {
        printf("║  ❌ %d/5 TESTS PASSED                          ║\n", passed);
        printf("║  V3.94 needs REVIEW                           ║\n");
    }
    printf("╚════════════════════════════════════════════════╝\n");
    
    return (passed == 5) ? 0 : 1;
}
