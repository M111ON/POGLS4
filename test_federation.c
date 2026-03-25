/*
 * test_federation.c — POGLS Federation Layer Test Suite
 * ══════════════════════════════════════════════════════
 *
 * FED01-FED05  fed_gate: PASS / GHOST / DROP conditions
 * FED06-FED08  BackpressureCtx: HWM throttle, resume, pop
 * FED09-FED11  ShadowSnapshot: init, commit, double-buffer swap
 * FED12-FED15  EarlyMerkle: update, reduce, tile hash consistency
 * FED16-FED20  fed_write full path: gate + BP + merkle + write
 * FED21-FED24  fed_commit: dual merkle A+B, atomic swap
 * FED25-FED27  fed_recover: clean path
 * FED28-FED30  Stress: 1000 writes, no overflow, determinism
 */
#include "pogls_delta_compat.h"
#include "storage/pogls_delta_world_b.h"
#include "pogls_federation.h"
#include <stdio.h>
#include <string.h>

static int _pass = 0, _fail = 0;
#define PASS(n)         do { printf("  \xe2\x9c\x85 %-48s PASS\n", n); _pass++; } while(0)
#define FAIL(n, ...)    do { printf("  \xe2\x9d\x8c %-48s FAIL -- ", n); printf(__VA_ARGS__); printf("\n"); _fail++; } while(0)
#define CHECK(n,c,...)  do { if(c) PASS(n); else FAIL(n,__VA_ARGS__); } while(0)
#define SECTION(s)      printf("\n-- %s --\n", s)

/* helper: build a packed GPU cell */
static uint32_t make_packed(uint8_t lane, uint8_t iso)
{
    /* PACK: bit[25:20]=lane, bit0=iso */
    return ((uint32_t)(lane & 0x3F) << 20) | (iso & 1u);
}

/* ── FED01-FED05  Gate ────────────────────────────────────────────── */
static void test_gate(void)
{
    SECTION("FED01-FED05  Pre-commit Gate");
    GateStats gs = {0};

    /* FED01: iso=0, valid lane, mature → PASS */
    uint32_t p1 = make_packed(5, 0);
    GateResult r1 = fed_gate(p1, 100, &gs);
    CHECK("FED01 iso=0 mature → GATE_PASS", r1 == GATE_PASS,
          "got=%d", r1);

    /* FED02: iso=1 → DROP */
    uint32_t p2 = make_packed(5, 1);
    GateResult r2 = fed_gate(p2, 100, &gs);
    CHECK("FED02 iso=1 → GATE_DROP", r2 == GATE_DROP,
          "got=%d", r2);

    /* FED03: op_count < GHOST_STREAK_MAX → GHOST (warm-up) */
    uint32_t p3 = make_packed(3, 0);
    GateResult r3 = fed_gate(p3, 2, &gs);
    CHECK("FED03 op_count<8 → GATE_GHOST", r3 == GATE_GHOST,
          "got=%d", r3);

    /* FED04: lane=63 (max valid) → PASS when mature */
    uint32_t p4 = make_packed(53, 0);
    GateResult r4 = fed_gate(p4, 999, &gs);
    CHECK("FED04 lane=53 mature → GATE_PASS", r4 == GATE_PASS,
          "got=%d", r4);

    /* FED05: stats counters updated */
    CHECK("FED05 gate stats passed >= 2", gs.passed >= 2,
          "passed=%llu", (unsigned long long)gs.passed);
    CHECK("FED05 gate stats dropped >= 1", gs.dropped >= 1,
          "dropped=%llu", (unsigned long long)gs.dropped);
}

/* ── FED06-FED08  Backpressure ───────────────────────────────────── */
static void test_backpressure(void)
{
    SECTION("FED06-FED08  Backpressure");
    BackpressureCtx bp;
    bp_init(&bp);

    /* FED06: fresh BP → bp_check returns 0 (no throttle) */
    CHECK("FED06 fresh BP no throttle", bp_check(&bp) == 0,
          "depth=%u", bp.queue_depth);

    /* FED07: fill to HWM → bp_check returns 1 */
    for (uint32_t i = 0; i < FED_QUEUE_HWM + 1; i++)
        bp_push(&bp);
    CHECK("FED07 depth > HWM → bp_check=1", bp_check(&bp) == 1,
          "depth=%u hwm=%u", bp.queue_depth, FED_QUEUE_HWM);

    /* FED08: drain below LWM → bp_check returns 0 */
    while (bp.queue_depth > FED_QUEUE_LWM / 2)
        bp_pop(&bp);
    CHECK("FED08 drain to LWM/2 → bp_check=0", bp_check(&bp) == 0,
          "depth=%u lwm=%u", bp.queue_depth, FED_QUEUE_LWM);
}

/* ── FED09-FED11  ShadowSnapshot ────────────────────────────────── */
static void test_shadow_snapshot(void)
{
    SECTION("FED09-FED11  Shadow Snapshot");
    ShadowSnapshot ss;

    /* FED09: ss_init with temp vault */
    int r = ss_init(&ss, "/tmp/fed_test_vault");
    CHECK("FED09 ss_init returns 0", r == 0, "r=%d", r);
    CHECK("FED09 active starts at 0", ss.active == 0, "active=%u", ss.active);

    /* FED10: ss_commit swaps active buffer */
    int rc = ss_commit(&ss);
    CHECK("FED10 ss_commit returns 0", rc == 0, "rc=%d", rc);
    CHECK("FED10 active swapped to 1", ss.active == 1, "active=%u", ss.active);

    /* FED11: second commit swaps back to 0 */
    ss_commit(&ss);
    CHECK("FED11 second commit active=0", ss.active == 0, "active=%u", ss.active);

    ss_close(&ss);
}

/* ── FED12-FED15  EarlyMerkle ────────────────────────────────────── */
static void test_early_merkle(void)
{
    SECTION("FED12-FED15  Early Merkle");
    EarlyMerkle em;
    em_init(&em);

    /* FED12: fresh merkle → all tile hashes zero */
    int all_zero = 1;
    for (uint32_t i = 0; i < FED_TILE_COUNT; i++)
        if (em.tile_hash[i] != 0) { all_zero = 0; break; }
    CHECK("FED12 fresh tiles all zero", all_zero, "tile[0]=%016llx",
          (unsigned long long)em.tile_hash[0]);

    /* FED13: em_update changes tile hash */
    uint8_t buf[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
    em_update(&em, 5, buf, 8);
    CHECK("FED13 em_update changes tile[5]", em.tile_hash[5] != 0,
          "tile[5]=%016llx", (unsigned long long)em.tile_hash[5]);

    /* FED14: em_update is deterministic */
    EarlyMerkle em2;
    em_init(&em2);
    em_update(&em2, 5, buf, 8);
    CHECK("FED14 em_update deterministic", em.tile_hash[5] == em2.tile_hash[5],
          "a=%016llx b=%016llx",
          (unsigned long long)em.tile_hash[5],
          (unsigned long long)em2.tile_hash[5]);

    /* FED15: em_reduce sets combined_root != 0 */
    em_reduce(&em);
    CHECK("FED15 em_reduce sets combined_root", em.combined_root != 0,
          "root=%016llx", (unsigned long long)em.combined_root);
}

/* ── FED16-FED20  fed_write ──────────────────────────────────────── */
static void test_fed_write(void)
{
    SECTION("FED16-FED20  fed_write full path");
    FederationCtx f;
    int r = fed_init(&f, "/tmp/fed_test_write");
    CHECK("FED16 fed_init returns 0", r == 0, "r=%d", r);

    /* FED17: PASS write increments op_count */
    uint32_t p = make_packed(5, 0);
    uint64_t op_before = f.op_count;
    GateResult gr = fed_write(&f, p, 0x100ULL, 0xDEADBEEFULL);
    /* first write: op_count < GHOST_STREAK_MAX → GHOST */
    int wrote = (gr == GATE_PASS || gr == GATE_GHOST);
    CHECK("FED17 fed_write returns PASS or GHOST", wrote, "gr=%d", gr);

    /* FED18: iso=1 → DROP, op_count unchanged */
    uint32_t p_iso = make_packed(5, 1);
    uint64_t op_mid = f.op_count;
    fed_write(&f, p_iso, 0x200ULL, 0xCAFEULL);
    CHECK("FED18 iso=1 DROP does not increment op_count",
          f.op_count == op_mid, "op=%llu expected=%llu",
          (unsigned long long)f.op_count, (unsigned long long)op_mid);

    /* FED19: 100 mature writes succeed */
    int ok = 1;
    for (uint32_t i = 0; i < 100; i++) {
        GateResult g = fed_write(&f, make_packed(i%54, 0),
                                 (uint64_t)i * 1000ULL,
                                 (uint64_t)i * 0x1234ULL);
        if (g == GATE_DROP) { ok = 0; break; }
    }
    CHECK("FED19 100 mature writes no DROP", ok, "failed at some write");

    /* FED20: gate stats consistent */
    CHECK("FED20 gate.passed + gate.ghosted + gate.dropped > 0",
          f.gate.passed + f.gate.ghosted + f.gate.dropped > 0,
          "all zero?");

    fed_close(&f);
}

/* ── FED21-FED24  fed_commit ─────────────────────────────────────── */
static void test_fed_commit(void)
{
    SECTION("FED21-FED24  fed_commit dual Merkle");
    FederationCtx f;
    fed_init(&f, "/tmp/fed_test_commit");

    /* prime with 50 mature writes */
    for (uint32_t i = 0; i < 50; i++)
        fed_write(&f, make_packed(i % 54, 0),
                  (uint64_t)i * 500ULL, (uint64_t)i);

    /* FED21: fed_commit returns 0 */
    int rc = fed_commit(&f);
    CHECK("FED21 fed_commit returns 0", rc == 0, "rc=%d", rc);

    /* FED22: shadow snapshot swapped after commit */
    uint32_t active_after = f.ss.active;
    CHECK("FED22 active buffer swapped", active_after == 1,
          "active=%u", active_after);

    /* FED23: EarlyMerkle reset after commit */
    int tiles_reset = 1;
    for (uint32_t i = 0; i < FED_TILE_COUNT; i++)
        if (f.em.tile_hash[i] != 0) { tiles_reset = 0; break; }
    CHECK("FED23 em tiles reset after commit", tiles_reset,
          "tile[0]=%016llx", (unsigned long long)f.em.tile_hash[0]);

    /* FED24: second commit also succeeds */
    for (uint32_t i = 0; i < 20; i++)
        fed_write(&f, make_packed(i % 54, 0), (uint64_t)i, (uint64_t)i);
    int rc2 = fed_commit(&f);
    CHECK("FED24 second fed_commit returns 0", rc2 == 0, "rc=%d", rc2);

    fed_close(&f);
}

/* ── FED25-FED27  Recovery ───────────────────────────────────────── */
static void test_recovery(void)
{
    SECTION("FED25-FED27  Recovery");

    /* FED25: fed_recover returns CLEAN for fresh path */
    Delta_DualRecovery dr = fed_recover("/tmp/fed_test_recovery");
    CHECK("FED25 fed_recover world_a clean or new",
          dr.world_a == DELTA_RECOVERY_CLEAN || dr.world_a == DELTA_RECOVERY_NEW,
          "world_a=%d", dr.world_a);
    CHECK("FED26 fed_recover world_b clean or new",
          dr.world_b == DELTA_RECOVERY_CLEAN || dr.world_b == DELTA_RECOVERY_NEW,
          "world_b=%d", dr.world_b);

    /* FED27: NULL vault → handled gracefully */
    FederationCtx f;
    int r = fed_init(&f, NULL);
    CHECK("FED27 fed_init NULL vault returns -1", r == -1, "r=%d", r);
}

/* ── FED28-FED30  Stress ────────────────────────────────────────── */
static void test_stress(void)
{
    SECTION("FED28-FED30  Stress + Determinism");

    /* FED28: 1000 writes, backpressure never permanently stuck */
    FederationCtx f;
    fed_init(&f, "/tmp/fed_test_stress");
    uint64_t drops = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        GateResult g = fed_write(&f, make_packed(i % 54, 0),
                                 (uint64_t)i * 777ULL, (uint64_t)i);
        if (g == GATE_DROP) drops++;
        /* drain every 128 to simulate V4 consuming */
        if (i % 128 == 127) fed_drain(&f, 256);
    }
    CHECK("FED28 1000 writes drops only from iso (0 here)", drops == 0,
          "drops=%llu", (unsigned long long)drops);

    /* FED29: commit after bulk write succeeds */
    int rc = fed_commit(&f);
    CHECK("FED29 bulk commit returns 0", rc == 0, "rc=%d", rc);

    /* FED30: determinism — same sequence → same combined_root */
    FederationCtx fa, fb;
    fed_init(&fa, "/tmp/fed_test_det_a");
    fed_init(&fb, "/tmp/fed_test_det_b");
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t p = make_packed(i % 54, 0);
        fed_write(&fa, p, (uint64_t)i, (uint64_t)i * 0xABCDULL);
        fed_write(&fb, p, (uint64_t)i, (uint64_t)i * 0xABCDULL);
    }
    /* force merkle reduce on both */
    em_reduce(&fa.em);
    em_reduce(&fb.em);
    CHECK("FED30 deterministic combined_root",
          fa.em.combined_root == fb.em.combined_root,
          "a=%016llx b=%016llx",
          (unsigned long long)fa.em.combined_root,
          (unsigned long long)fb.em.combined_root);
    fed_close(&fa);
    fed_close(&fb);
    fed_close(&f);
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(void)
{
    printf("\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97\n");
    printf("\xe2\x95\x91  POGLS Federation Layer Test Suite                  \xe2\x95\x91\n");
    printf("\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\n");

    test_gate();
    test_backpressure();
    test_shadow_snapshot();
    test_early_merkle();
    test_fed_write();
    test_fed_commit();
    test_recovery();
    test_stress();

    int total = _pass + _fail;
    printf("\n\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97\n");
    printf("\xe2\x95\x91  RESULT: %d/%d PASS  %s%-5s                            \xe2\x95\x91\n",
           _pass, total,
           _fail == 0 ? "\xe2\x9c\x85 " : "\xe2\x9d\x8c ",
           _fail == 0 ? "CLEAN" : "FAIL");
    printf("\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\n");
    return _fail == 0 ? 0 : 1;
}
