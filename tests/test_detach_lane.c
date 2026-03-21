/*
 * test_detach_lane.c — Tests for pogls_detach_lane.h
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../pogls_detach_lane.h"

static int g_pass = 0, g_fail = 0;
#define section(s)  printf("\n  -- %s\n", s)
#define check(cond, ok, fail) do { \
    if(cond){printf("    v %s\n",ok);g_pass++;}  \
    else    {printf("    x FAIL: %s\n",fail);g_fail++;} \
} while(0)

/* T01: layout */
static void t01_layout(void)
{
    section("T01  Layout & magic");
    check(sizeof(DetachEntry) == 32u, "DetachEntry=32B", "size wrong");
    DetachLane dl;
    detach_lane_init(&dl, NULL);
    check(dl.magic == DETACH_MAGIC, "magic set", "magic wrong");
    check(dl.head == 0, "head=0", "head wrong");
    check(dl.tail == 0, "tail=0", "tail wrong");
}

/* T02: push + foreground drain */
static void t02_push_drain(void)
{
    section("T02  Push + foreground drain");
    DetachLane dl;
    detach_lane_init(&dl, NULL);  /* no delta — drain goes nowhere */

    int r = detach_lane_push(&dl, 0xDEAD, 0x42, DETACH_REASON_GEO_INVALID, 2, 0u, 0ULL);
    check(r == 1, "push ok (no overflow)", "push failed");
    check(dl.stats.pushed == 1, "pushed=1", "counter wrong");
    check(dl.stats.reason_geo == 1, "reason_geo=1", "reason counter wrong");
    check(dl.head == 1, "head=1", "head not advanced");

    /* check entry fields */
    DetachEntry *e = &dl.ring[0];
    check(e->value == 0xDEAD, "value preserved", "value wrong");
    check(e->angular_addr == 0x42, "addr preserved", "addr wrong");
    check(e->reason == DETACH_REASON_GEO_INVALID, "reason correct", "reason wrong");
    check(e->route_was == 2, "route_was=2", "route wrong");

    uint32_t n = detach_lane_drain(&dl);
    check(n == 1, "drained 1", "drain count wrong");
    check(dl.tail == 1, "tail=1 after drain", "tail wrong");
}

/* T03: all three reasons */
static void t03_reasons(void)
{
    section("T03  All three anomaly reasons");
    DetachLane dl;
    detach_lane_init(&dl, NULL);

    detach_lane_push(&dl, 1, 1, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);
    detach_lane_push(&dl, 2, 2, DETACH_REASON_GHOST_DRIFT, 1, 0u, 0ULL);
    detach_lane_push(&dl, 3, 3, DETACH_REASON_UNIT_CIRCLE, 2, 0u, 0ULL);
    detach_lane_push(&dl, 4, 4,
        DETACH_REASON_GEO_INVALID | DETACH_REASON_GHOST_DRIFT, 0, 0u, 0ULL);

    check(dl.stats.reason_geo   == 2, "geo=2",   "geo count wrong");
    check(dl.stats.reason_drift == 2, "drift=2", "drift count wrong");
    check(dl.stats.reason_circle== 1, "circle=1","circle count wrong");
}

/* T04: overflow drop-oldest */
static void t04_overflow(void)
{
    section("T04  Overflow drop-oldest (never block)");
    DetachLane dl;
    detach_lane_init(&dl, NULL);

    /* fill ring */
    for (uint32_t i = 0; i < DETACH_RING_SIZE; i++)
        detach_lane_push(&dl, i, i, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);

    check(dl.stats.overflows == 0, "no overflow while filling", "wrong");
    check(dl.head == DETACH_RING_SIZE, "head=RING_SIZE", "head wrong");

    /* one more — must drop oldest */
    int r = detach_lane_push(&dl, 0xFFFF, 0, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);
    check(r == 0, "overflow returns 0", "wrong");
    check(dl.stats.overflows == 1, "overflows=1", "counter wrong");

    /* reason flag set */
    uint32_t last_idx = (dl.head - 1) & DETACH_RING_MASK;
    check((dl.ring[last_idx].reason & DETACH_REASON_OVERFLOW) != 0,
          "OVERFLOW flag set on entry", "flag missing");

    /* still pushable after overflow */
    int r2 = detach_lane_push(&dl, 42, 0, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);
    check(r2 == 0, "still overflowing (ring still full)", "wrong");
    /* drain then push */
    detach_lane_drain(&dl);
    int r3 = detach_lane_push(&dl, 99, 0, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);
    check(r3 == 1, "push ok after drain", "failed after drain");
}

/* T05: occupancy */
static void t05_occupancy(void)
{
    section("T05  Occupancy & pressure");
    DetachLane dl;
    detach_lane_init(&dl, NULL);

    check(detach_lane_occupancy_pct(&dl) == 0, "empty=0%", "wrong");
    check(!detach_lane_under_pressure(&dl), "no pressure when empty", "wrong");

    for (uint32_t i = 0; i < DETACH_RING_SIZE * 3 / 4; i++)
        detach_lane_push(&dl, i, i, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);

    uint32_t pct = detach_lane_occupancy_pct(&dl);
    check(pct >= 74u && pct <= 76u, "75% occupancy", "pct wrong");
    check(detach_lane_under_pressure(&dl), "under pressure at 75%", "wrong");
}

/* T06: threaded flush */
static void t06_threaded(void)
{
    section("T06  Threaded async flush (no delta)");
    DetachLane dl;
    detach_lane_init(&dl, NULL);

    int rc = detach_lane_start(&dl);
    check(rc == 0, "start ok", "start failed");
    check(dl.thread_started == 1, "thread_started=1", "flag wrong");

    for (uint32_t i = 0; i < 2048; i++)
        detach_lane_push(&dl, i, i,
            DETACH_REASON_GEO_INVALID | DETACH_REASON_GHOST_DRIFT, 0, 0u, 0ULL);

    struct timespec ts = {0, 5000000L};
    nanosleep(&ts, NULL);

    rc = detach_lane_stop(&dl);
    check(rc == 0, "stop ok", "stop failed");
    check(dl.thread_started == 0, "thread_started=0", "flag wrong");
    detach_lane_drain(&dl);
    check(dl.stats.pushed == 2048, "2048 pushed", "count wrong");
}

/* T07: DiamondBlock encoding in flush pass */
static void t07_block_encoding(void)
{
    section("T07  DiamondBlock encoding correctness");
    DetachLane dl;

    /* use a fake delta to verify block content */
    /* we test by checking ring entries directly before flush */
    detach_lane_init(&dl, NULL);

    detach_lane_push(&dl, 0xABCDEF01ULL, 0x12345678ULL,
                     DETACH_REASON_GHOST_DRIFT, 1, 0u, 0ULL);

    DetachEntry *e = &dl.ring[0];
    check(e->value == 0xABCDEF01ULL, "value in ring", "wrong");
    check(e->angular_addr == 0x12345678ULL, "addr in ring", "wrong");
    check(e->reason == DETACH_REASON_GHOST_DRIFT, "reason in ring", "wrong");
    check(e->route_was == 1, "route_was in ring", "wrong");
    check(e->phase18 == 0, "phase18=0 (op_count=0)", "phase wrong");
}

/* T08: NULL safety */
static void t08_null(void)
{
    section("T08  NULL safety");
    check(detach_lane_init(NULL, NULL) == -1, "init(NULL)=-1", "crash");
    check(detach_lane_push(NULL,0,0,0,0, 0u, 0ULL) == 0, "push(NULL)=0", "crash");
    check(detach_lane_drain(NULL) == 0, "drain(NULL)=0", "crash");
    check(detach_lane_occupancy_pct(NULL) == 0, "occ(NULL)=0", "crash");
    check(!detach_lane_under_pressure(NULL), "pressure(NULL)=false", "crash");
    detach_lane_stats(NULL);
    check(1, "stats(NULL) no crash", "crash");
}

/* T09: wrap-around */
static void t09_wrap(void)
{
    section("T09  Ring wrap-around (3 full cycles)");
    DetachLane dl;
    detach_lane_init(&dl, NULL);

    for (uint32_t cycle = 0; cycle < 3; cycle++) {
        for (uint32_t i = 0; i < DETACH_RING_SIZE; i++)
            detach_lane_push(&dl, i, i, DETACH_REASON_GEO_INVALID, 0, 0u, 0ULL);
        detach_lane_drain(&dl);
    }
    check(dl.stats.pushed == 3ULL * DETACH_RING_SIZE,
          "3 full cycles pushed", "count wrong");
    check(dl.stats.overflows == 0, "no overflow on clean cycles", "overflow");
}

/* T10: double start guard */
static void t10_double_start(void)
{
    section("T10  Double start guard");
    DetachLane dl;
    detach_lane_init(&dl, NULL);
    detach_lane_start(&dl);
    int rc2 = detach_lane_start(&dl);
    check(rc2 != 0, "double start rejected", "not guarded");
    detach_lane_stop(&dl);
}

int main(void)
{
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Detach Safety Layer — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");

    t01_layout();
    t02_push_drain();
    t03_reasons();
    t04_overflow();
    t05_occupancy();
    t06_threaded();
    t07_block_encoding();
    t08_null();
    t09_wrap();
    t10_double_start();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — Detach live (Protect) [S]\n",
               g_pass, g_pass+g_fail);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
