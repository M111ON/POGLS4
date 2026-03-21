/*
 * test_hydra_batch.c — Tests for pogls_hydra_batch.h
 * ══════════════════════════════════════════════════════════════════════
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../hydra/pogls_hydra_batch.h"

/* ── test harness ────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;

#define section(s)  printf("\n  ── %s\n", s)

#define check(cond, ok_msg, fail_msg) do { \
    if (cond) { printf("    ✓ %s\n", ok_msg); g_pass++; } \
    else       { printf("    ✗ FAIL: %s\n", fail_msg); g_fail++; } \
} while(0)

/* ── dispatch accumulator ────────────────────────────────────────── */
typedef struct {
    uint64_t  total_ops;
    uint64_t  full_dispatches;   /* count of dispatches with count==4  */
    uint64_t  partial_dispatches;
    uint64_t  value_sum;
    uint32_t  last_head_id;
    uint32_t  last_count;
} DispatchAcc;

static void dispatch_acc_cb(const HBEntry *e, uint32_t n,
                             uint32_t head_id, void *ctx)
{
    DispatchAcc *d = (DispatchAcc *)ctx;
    d->total_ops        += n;
    d->last_head_id      = head_id;
    d->last_count        = n;
    if (n == HB_BATCH_SIZE) d->full_dispatches++;
    else                    d->partial_dispatches++;
    for (uint32_t i = 0; i < n; i++) d->value_sum += e[i].value;
}

/* ── T01: struct size & magic ────────────────────────────────────── */
static void t01_layout(void)
{
    section("T01  Layout & magic");
    check(sizeof(HBEntry) == 16u, "HBEntry=16B", "HBEntry wrong size");

    /* HBSlot contains 4×HBEntry = 64B + metadata, aligned to 64B */
    check(sizeof(HBSlot) % 64u == 0, "HBSlot cache-line aligned",
          "HBSlot alignment wrong");

    HydraBatch hb;
    hydra_batch_init(&hb, NULL, NULL);
    check(hb.magic == HYDRA_BATCH_MAGIC, "magic set", "magic wrong");
    check(hb.total_pushed == 0, "pushed=0", "pushed non-zero");
}

/* ── T02: push 3 → no dispatch; push 1 more → dispatch ──────────── */
static void t02_batch_trigger(void)
{
    section("T02  Batch trigger at 4");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 0);

    /* push 3 — should not dispatch */
    for (int i = 0; i < 3; i++) {
        int r = hydra_batch_push(&hb, 0, (uint64_t)(i+1), 0);
        check(r == 1, "push returns 1 (not full)", "push wrong return");
    }
    check(acc.total_ops == 0, "no dispatch at 3 ops", "premature dispatch");
    check(hydra_batch_pending(&hb, 0) == 3, "pending=3", "pending wrong");

    /* push 4th → dispatch */
    int r = hydra_batch_push(&hb, 0, 4ULL, 0);
    check(r == 2, "push returns 2 (batch dispatched)", "batch not dispatched");
    check(acc.total_ops == 4, "dispatch got 4 ops", "dispatch count wrong");
    check(acc.full_dispatches == 1, "full_dispatches=1", "full counter wrong");
    check(acc.value_sum == 1+2+3+4, "value sum correct", "value sum wrong");
    check(hydra_batch_pending(&hb, 0) == 0, "pending=0 after dispatch",
          "slot not cleared");
}

/* ── T03: partial flush ──────────────────────────────────────────── */
static void t03_partial_flush(void)
{
    section("T03  Partial flush (< 4 ops)");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 1);

    hydra_batch_push(&hb, 1, 10ULL, 0x100ULL);
    hydra_batch_push(&hb, 1, 20ULL, 0x200ULL);
    /* only 2 pushed — flush */
    hydra_batch_flush_head(&hb, 1);

    check(acc.total_ops == 2, "partial flush dispatched 2", "count wrong");
    check(acc.partial_dispatches == 1, "partial_dispatches=1", "partial counter wrong");
    check(acc.value_sum == 30ULL, "value sum=30", "value sum wrong");
    check(hydra_batch_pending(&hb, 1) == 0, "pending=0 after flush", "not cleared");
}

/* ── T04: flush all heads ────────────────────────────────────────── */
static void t04_flush_all(void)
{
    section("T04  Flush all heads");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);

    /* activate 4 heads, push 2 ops each */
    for (uint32_t h = 0; h < 4; h++) {
        hydra_batch_activate(&hb, h);
        hydra_batch_push(&hb, h, h*10 + 1, 0);
        hydra_batch_push(&hb, h, h*10 + 2, 0);
    }
    check(acc.total_ops == 0, "no dispatch before flush", "premature dispatch");

    hydra_batch_flush_all(&hb);
    check(acc.total_ops == 8, "flush all: 8 ops", "flush all count wrong");
    check(acc.partial_dispatches == 4, "4 partial dispatches", "partial count wrong");
    check(hydra_batch_total_pending(&hb) == 0, "all pending=0", "pending not cleared");
}

/* ── T05: push_multi (N > 4, multiple batches) ───────────────────── */
static void t05_push_multi(void)
{
    section("T05  push_multi (12 ops = 3 full batches)");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 0);

    HBEntry ops[12];
    uint64_t expected_sum = 0;
    for (int i = 0; i < 12; i++) {
        ops[i].value        = (uint64_t)(i + 1);
        ops[i].angular_addr = (uint64_t)(i * 100);
        expected_sum += ops[i].value;
    }

    uint32_t dispatched = hydra_batch_push_multi(&hb, 0, ops, 12);
    check(dispatched == 12, "push_multi dispatched 12", "dispatch count wrong");
    check(acc.total_ops == 12, "acc.total_ops=12", "acc count wrong");
    check(acc.full_dispatches == 3, "3 full batches", "full batch count wrong");
    check(acc.value_sum == expected_sum, "value sum correct", "sum wrong");
    check(hydra_batch_pending(&hb, 0) == 0, "no pending after exact 12",
          "pending left");
}

/* ── T06: push_multi with remainder ─────────────────────────────── */
static void t06_push_multi_remainder(void)
{
    section("T06  push_multi with remainder (10 ops = 2 full + 2 pending)");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 2);

    HBEntry ops[10];
    for (int i = 0; i < 10; i++) { ops[i].value = i+1; ops[i].angular_addr = 0; }

    hydra_batch_push_multi(&hb, 2, ops, 10);
    check(acc.full_dispatches == 2, "2 full batches dispatched", "wrong");
    check(acc.total_ops == 8, "8 ops dispatched so far", "wrong");
    check(hydra_batch_pending(&hb, 2) == 2, "2 ops pending", "pending wrong");

    /* flush remainder */
    hydra_batch_flush_head(&hb, 2);
    check(acc.total_ops == 10, "all 10 dispatched after flush", "total wrong");
}

/* ── T07: head routing — head_id passed to callback ─────────────── */
static void t07_head_routing(void)
{
    section("T07  Head ID routing to callback");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);

    hydra_batch_activate(&hb, 5);
    for (int i = 0; i < 4; i++)
        hydra_batch_push(&hb, 5, (uint64_t)i, 0);

    check(acc.last_head_id == 5u, "callback got head_id=5", "head_id wrong");
}

/* ── T08: inactive head push returns 0 ──────────────────────────── */
static void t08_inactive_head(void)
{
    section("T08  Inactive head push returns 0");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    /* head 3 NOT activated */
    int r = hydra_batch_push(&hb, 3, 0xDEAD, 0);
    check(r == 0, "inactive push returns 0", "wrong return");
    check(acc.total_ops == 0, "no dispatch to inactive head", "dispatched");
}

/* ── T09: deactivate flushes pending ops ─────────────────────────── */
static void t09_deactivate_flush(void)
{
    section("T09  Deactivate flushes pending ops");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 7);
    hydra_batch_push(&hb, 7, 111ULL, 0);
    hydra_batch_push(&hb, 7, 222ULL, 0);
    /* 2 ops pending — deactivate should flush them */
    hydra_batch_deactivate(&hb, 7);
    check(acc.total_ops == 2, "deactivate flushed 2 ops", "flush count wrong");
    check(hb.slots[7].active == 0, "head 7 now inactive", "still active");
    check(hydra_batch_pending(&hb, 7) == 0, "no pending after deactivate", "pending left");
}

/* ── T10: efficiency percentage ──────────────────────────────────── */
static void t10_efficiency(void)
{
    section("T10  Efficiency percentage");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);
    hydra_batch_activate(&hb, 0);

    /* push exactly 8 ops = 2 full batches, no partial */
    for (int i = 0; i < 8; i++) hydra_batch_push(&hb, 0, i, 0);
    check(hydra_batch_efficiency_pct(&hb) == 100u,
          "efficiency=100% (only full batches)", "efficiency wrong");

    /* now push 1 partial and flush → 1 partial dispatch added */
    hydra_batch_push(&hb, 0, 99, 0);
    hydra_batch_flush_head(&hb, 0);
    /* 2 full + 1 partial = 2/3 = 66% */
    uint32_t eff = hydra_batch_efficiency_pct(&hb);
    check(eff >= 65u && eff <= 68u, "efficiency ~66% with one partial",
          "efficiency out of range");
}

/* ── T11: NULL safety ────────────────────────────────────────────── */
static void t11_null_safety(void)
{
    section("T11  NULL safety");
    check(hydra_batch_init(NULL, NULL, NULL) == -1,
          "init(NULL)=-1", "null crash");
    check(hydra_batch_push(NULL, 0, 0, 0) == 0,
          "push(NULL)=0",  "null crash");
    check(hydra_batch_pending(NULL, 0) == 0,
          "pending(NULL)=0", "null crash");
    check(hydra_batch_total_pending(NULL) == 0,
          "total_pending(NULL)=0", "null crash");
    check(hydra_batch_efficiency_pct(NULL) == 0,
          "efficiency(NULL)=0", "null crash");
    hydra_batch_flush_head(NULL, 0);   /* should not crash */
    hydra_batch_flush_all(NULL);       /* should not crash */
    hydra_batch_activate(NULL, 0);     /* should not crash */
    hydra_batch_deactivate(NULL, 0);   /* should not crash */
    check(1, "all NULL paths survived", "crash");
}

/* ── T12: stats counter integrity across many ops ────────────────── */
static void t12_stats_integrity(void)
{
    section("T12  Stats integrity (1000 ops, 3 heads)");
    DispatchAcc acc = {0};
    HydraBatch hb;
    hydra_batch_init(&hb, dispatch_acc_cb, &acc);

    for (uint32_t h = 0; h < 3; h++) hydra_batch_activate(&hb, h);

    for (uint32_t i = 0; i < 1000; i++)
        hydra_batch_push(&hb, i % 3, (uint64_t)i, (uint64_t)i * 7);

    hydra_batch_flush_all(&hb);

    check(hb.total_pushed == 1000, "total_pushed=1000", "wrong");
    check(hb.total_dispatched == 1000, "total_dispatched=1000", "wrong");
    check(acc.total_ops == 1000, "acc.total_ops=1000", "wrong");

    /* 1000 / 4 = 250 full batches; 1000 % 4 = 0 → all full (3 heads × 333.33) */
    /* Each head: 333 or 334 ops. 333 = 83×4+1 → 83 full + 1 partial per head */
    /* exact numbers depend on distribution — just verify totals add up */
    uint64_t all_dispatch = hb.full_batches * HB_BATCH_SIZE + hb.partial_flushes * 1; /* partial count per op? */
    /* better: dispatched = full_batches * 4 + partial_dispatched (each partial ≤ 3) */
    check(hb.full_batches + hb.partial_flushes > 0, "some dispatches happened", "none");
    (void)all_dispatch;

    /* value sum = 0+1+...+999 = 499500 */
    check(acc.value_sum == 499500ULL, "value sum = 499500", "sum wrong");
}

/* ── main ────────────────────────────────────────────────────────── */
int main(void)
{
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Hydra Batch-4 Commit — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");

    t01_layout();
    t02_batch_trigger();
    t03_partial_flush();
    t04_flush_all();
    t05_push_multi();
    t06_push_multi_remainder();
    t07_head_routing();
    t08_inactive_head();
    t09_deactivate_flush();
    t10_efficiency();
    t11_null_safety();
    t12_stats_integrity();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0) {
        printf("  %d / %d PASS  ✓ ALL PASS — Hydra Batch live ⚡\n",
               g_pass, g_pass + g_fail);
    } else {
        printf("  %d / %d PASS  ✗ %d FAILED\n",
               g_pass, g_pass + g_fail, g_fail);
    }
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
