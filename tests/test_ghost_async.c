/*
 * test_ghost_async.c — Tests for pogls_ghost_async.h
 * ══════════════════════════════════════════════════════════════════════
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>

#include "../pogls_ghost_async.h"

/* ── test harness ────────────────────────────────────────────────── */
static int  g_pass = 0, g_fail = 0;
static char g_section[64] = "init";

#define section(s)  do { snprintf(g_section,sizeof(g_section),"%s",s); \
                         printf("\n  ── %s\n",s); } while(0)

#define check(cond, ok_msg, fail_msg) do { \
    if (cond) { printf("    ✓ %s\n", ok_msg); g_pass++; } \
    else       { printf("    ✗ FAIL: %s\n", fail_msg); g_fail++; } \
} while(0)

/* ── drain callback accumulator ──────────────────────────────────── */
typedef struct {
    uint64_t  count;
    uint64_t  value_sum;   /* sum of all drained values */
    uint8_t   last_lane;
} DrainAcc;

static void drain_acc_cb(const GhostAsyncEntry *e, uint32_t n, void *ctx)
{
    DrainAcc *d = (DrainAcc *)ctx;
    for (uint32_t i = 0; i < n; i++) {
        d->count++;
        d->value_sum += e[i].value;
        d->last_lane  = e[i].lane;
    }
}

/* ── T01: struct size and magic ──────────────────────────────────── */
static void t01_struct_layout(void)
{
    section("T01  Struct layout & magic");
    check(sizeof(GhostAsyncEntry) == 32u,
          "GhostAsyncEntry = 32B", "GhostAsyncEntry wrong size");

    GhostAsyncRing r;
    ghost_async_init(&r, NULL, NULL);
    check(r.magic == GHOST_RING_MAGIC, "magic set", "magic mismatch");
    check(r.head  == 0, "head=0",   "head non-zero after init");
    check(r.tail  == 0, "tail=0",   "tail non-zero after init");
    check(r.pushed == 0, "pushed=0", "pushed non-zero after init");
}

/* ── T02: single push + foreground drain ─────────────────────────── */
static void t02_push_drain_single(void)
{
    section("T02  Single push → foreground drain");
    DrainAcc acc = {0,0,0};
    GhostAsyncRing r;
    ghost_async_init(&r, drain_acc_cb, &acc);

    int ok = ghost_async_push(&r, 0xDEADBEEFULL, 0x42ULL, 0xABCDULL, 7u);
    check(ok == 1, "push returned 1", "push failed");
    check(r.pushed == 1, "pushed=1", "pushed counter wrong");
    check(r.head   == 1, "head=1",   "head not advanced");

    uint32_t n = ghost_async_drain_foreground(&r);
    check(n == 1,              "drained 1 entry",     "drain count wrong");
    check(acc.count == 1,      "acc.count=1",         "acc count wrong");
    check(acc.value_sum == 0xDEADBEEFULL, "value correct", "value mismatch");
    check(acc.last_lane == 7u, "lane=7",               "lane mismatch");
    check(r.drained == 1,      "r.drained=1",          "drained counter wrong");
    check(r.overflows == 0,    "no overflows",         "unexpected overflow");
}

/* ── T03: push many, drain in batches ───────────────────────────── */
static void t03_bulk_push_drain(void)
{
    section("T03  Bulk push (2048) → batch drain");
    DrainAcc acc = {0,0,0};
    GhostAsyncRing r;
    ghost_async_init(&r, drain_acc_cb, &acc);

    uint64_t expected_sum = 0;
    for (uint32_t i = 0; i < 2048; i++) {
        uint64_t v = (uint64_t)i + 1u;
        ghost_async_push(&r, v, i, i ^ 0xFF, (uint8_t)(i % 54));
        expected_sum += v;
    }
    check(r.pushed == 2048, "pushed=2048", "push count wrong");

    ghost_async_drain_foreground(&r);
    check(acc.count == 2048, "drained 2048", "drain count mismatch");
    check(acc.value_sum == expected_sum, "sum correct", "value sum mismatch");
    check(r.overflows == 0, "no overflows", "unexpected overflows");
}

/* ── T04: ring overflow detection ────────────────────────────────── */
static void t04_overflow(void)
{
    section("T04  Ring overflow (no drain)");
    GhostAsyncRing r;
    ghost_async_init(&r, NULL, NULL);   /* no drain callback */

    /* fill ring completely */
    uint32_t ok_count = 0;
    for (uint32_t i = 0; i < GHOST_RING_SIZE; i++) {
        ok_count += ghost_async_push(&r, i, i, i, 0u);
    }
    check(ok_count == GHOST_RING_SIZE, "filled ring ok", "fill failed");

    /* next push must overflow */
    int overflow = ghost_async_push(&r, 0xFFFFFFFF, 0, 0, 0);
    check(overflow == 0, "overflow returns 0", "overflow not detected");
    check(r.overflows == 1, "overflows=1", "overflow counter wrong");
    check(ghost_async_had_overflow(&r), "had_overflow true", "had_overflow false");

    /* drain and push again — should succeed */
    ghost_async_drain_foreground(&r);
    int recover = ghost_async_push(&r, 42ULL, 0, 0, 0);
    check(recover == 1, "push after drain ok", "push after drain failed");
}

/* ── T05: occupancy percentage ───────────────────────────────────── */
static void t05_occupancy(void)
{
    section("T05  Occupancy percentage");
    GhostAsyncRing r;
    ghost_async_init(&r, NULL, NULL);

    check(ghost_async_occupancy_pct(&r) == 0, "empty=0%", "wrong pct on empty");

    /* push half */
    for (uint32_t i = 0; i < GHOST_RING_SIZE / 2; i++)
        ghost_async_push(&r, i, i, i, 0u);

    uint32_t pct = ghost_async_occupancy_pct(&r);
    check(pct >= 49u && pct <= 51u, "half-full ~50%", "occupancy wrong");

    /* push to highwater */
    for (uint32_t i = GHOST_RING_SIZE/2; i < GHOST_RING_HIGHWATER; i++)
        ghost_async_push(&r, i, i, i, 0u);

    pct = ghost_async_occupancy_pct(&r);
    check(pct >= 74u && pct <= 76u, "highwater ~75%", "highwater pct wrong");
}

/* ── T06: threaded drain ─────────────────────────────────────────── */
static void t06_threaded_drain(void)
{
    section("T06  Threaded drain");
    DrainAcc acc = {0,0,0};
    GhostAsyncRing r;
    ghost_async_init(&r, drain_acc_cb, &acc);

    int rc = ghost_async_start(&r);
    check(rc == 0, "start ok", "thread start failed");
    check(r.thread_started == 1, "thread_started=1", "thread flag wrong");

    /* push from main thread while drain thread is running */
    for (uint32_t i = 0; i < 4096; i++)
        ghost_async_push(&r, (uint64_t)i, i, i, (uint8_t)(i%54));

    /* give drain thread time to process */
    struct timespec ts = {0, 5000000L};  /* 5 ms */
    nanosleep(&ts, NULL);

    rc = ghost_async_stop(&r);
    check(rc == 0, "stop ok", "thread stop failed");
    check(r.thread_started == 0, "thread_started=0", "flag not cleared");

    /* final foreground drain for any remaining */
    ghost_async_drain_foreground(&r);

    check(acc.count == 4096, "all 4096 drained",
          "not all entries drained");
    check(r.overflows == 0, "no overflows", "overflow in threaded test");
}

/* ── T07: double start guard ─────────────────────────────────────── */
static void t07_double_start(void)
{
    section("T07  Double start guard");
    GhostAsyncRing r;
    ghost_async_init(&r, NULL, NULL);
    ghost_async_start(&r);

    int rc2 = ghost_async_start(&r);
    check(rc2 != 0, "double start rejected", "double start not guarded");

    ghost_async_stop(&r);
}

/* ── T08: NULL safety ────────────────────────────────────────────── */
static void t08_null_safety(void)
{
    section("T08  NULL safety");
    check(ghost_async_init(NULL, NULL, NULL) == -1,
          "init(NULL)=-1", "init null crash");
    check(ghost_async_push(NULL, 0, 0, 0, 0) == 0,
          "push(NULL)=0",  "push null crash");
    check(ghost_async_drain_foreground(NULL) == 0,
          "drain(NULL)=0", "drain null crash");
    check(ghost_async_had_overflow(NULL) == 0,
          "had_overflow(NULL)=0", "had_overflow null crash");
    check(ghost_async_occupancy_pct(NULL) == 0,
          "occupancy(NULL)=0", "occupancy null crash");
}

/* ── T09: head/tail wrap ─────────────────────────────────────────── */
static void t09_wrap_around(void)
{
    section("T09  head/tail wrap-around");
    DrainAcc acc = {0,0,0};
    GhostAsyncRing r;
    ghost_async_init(&r, drain_acc_cb, &acc);

    /* force 3 full cycles through the ring */
    for (uint32_t cycle = 0; cycle < 3; cycle++) {
        for (uint32_t i = 0; i < GHOST_RING_SIZE; i++)
            ghost_async_push(&r, (uint64_t)(cycle*GHOST_RING_SIZE + i),
                             i, i, 0u);
        ghost_async_drain_foreground(&r);
    }

    uint64_t expected = 3ULL * GHOST_RING_SIZE;
    check(acc.count == expected, "3 full cycles drained",
          "wrap-around count wrong");
    check(r.overflows == 0, "no overflow on wrap", "wrap caused overflow");
}

/* ── T10: entry field integrity after wrap ───────────────────────── */
static void t10_entry_integrity(void)
{
    section("T10  Entry field integrity");

    /* verify value/addr/sig/lane preserved through ring */
    typedef struct { uint64_t val; uint64_t addr; uint64_t sig; uint8_t lane; } Ref;
    static Ref refs[8] = {
        {0x1111111111111111ULL, 0xAAAA, 0xBBBB, 11u},
        {0x2222222222222222ULL, 0xCCCC, 0xDDDD, 22u},
        {0x3333333333333333ULL, 0xEEEE, 0xFFFF, 33u},
        {0x4444444444444444ULL, 0x1234, 0x5678, 44u},
        {0x5555555555555555ULL, 0xABCD, 0xEF01, 5u},
        {0x6666666666666666ULL, 0x9876, 0x5432, 6u},
        {0x7777777777777777ULL, 0x1357, 0x2468, 7u},
        {0x8888888888888888ULL, 0xFEDC, 0xBA98, 8u},
    };

    /* custom drain that checks fields */
    typedef struct { int ok; } IntChk;
    static IntChk chk = {1};

    static Ref   *s_refs;
    static int    s_idx;
    s_refs = refs; s_idx  = 0;

    struct { GhostAsyncRing r; } ctx;
    memset(&ctx, 0, sizeof(ctx));

    void (*cb)(const GhostAsyncEntry*, uint32_t, void*);
    cb = NULL;  /* we'll drain manually and check below */

    GhostAsyncRing r;
    ghost_async_init(&r, NULL, NULL);

    for (int i = 0; i < 8; i++)
        ghost_async_push(&r, refs[i].val, refs[i].addr,
                         refs[i].sig, refs[i].lane);

    /* drain manually */
    GhostAsyncEntry batch[GHOST_DRAIN_BATCH];
    uint32_t t = r.tail;
    uint32_t h = __atomic_load_n(&r.head, __ATOMIC_ACQUIRE);
    (void)chk; (void)s_refs; (void)s_idx; (void)cb; (void)ctx;

    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        GhostAsyncEntry *e = &r.ring[(t + i) & GHOST_RING_MASK];
        if (e->value != refs[i].val ||
            e->angular_addr != refs[i].addr ||
            e->sig  != refs[i].sig  ||
            e->lane != refs[i].lane) {
            all_ok = 0;
            printf("    ✗ entry[%d] mismatch\n", i);
        }
    }
    (void)batch; (void)h;
    check(all_ok, "all 8 entries preserved", "entry field mismatch");
}

/* ── main ────────────────────────────────────────────────────────── */
int main(void)
{
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Ghost Async Ring — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");

    t01_struct_layout();
    t02_push_drain_single();
    t03_bulk_push_drain();
    t04_overflow();
    t05_occupancy();
    t06_threaded_drain();
    t07_double_start();
    t08_null_safety();
    t09_wrap_around();
    t10_entry_integrity();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0) {
        printf("  %d / %d PASS  ✓ ALL PASS — Ghost Async live 👻\n",
               g_pass, g_pass + g_fail);
    } else {
        printf("  %d / %d PASS  ✗ %d FAILED\n",
               g_pass, g_pass + g_fail, g_fail);
    }
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
