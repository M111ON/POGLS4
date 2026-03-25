/*
 * test_federation.c — POGLS Federation Layer Test Suite (V38-aligned)
 * FED01-FED30 covers gate, BP, shadow, merkle, write, commit, recovery, stress
 */
#include "core_c/pogls_delta.h"
#include "core_c/pogls_delta_world_b.h"
#include "pogls_federation.h"
#include <stdio.h>
#include <string.h>

static int _pass = 0, _fail = 0;
#define PASS(n)        do { printf("  \xe2\x9c\x85 %-50s PASS\n", n); _pass++; } while(0)
#define FAIL(n, ...)   do { printf("  \xe2\x9d\x8c %-50s FAIL -- ", n); printf(__VA_ARGS__); printf("\n"); _fail++; } while(0)
#define CHECK(n,c,...) do { if(c) PASS(n); else FAIL(n,__VA_ARGS__); } while(0)
#define SECTION(s)     printf("\n-- %s --\n", s)

/* packed: hil[19:0] | lane[25:20] | iso[26]
 * gate CHECK2: hil%54 == lane → use hil=lane (lane<54) */
static uint32_t make_packed(uint8_t lane, uint8_t iso)
{
    uint32_t hil = (uint32_t)lane;
    return (hil & 0xFFFFFu)
         | ((uint32_t)(lane & 0x3Fu) << 20)
         | ((uint32_t)(iso  & 1u)    << 26);
}

static void test_gate(void)
{
    SECTION("FED01-FED05  Pre-commit Gate");
    GateStats gs = {0};

    GateResult r1 = fed_gate(make_packed(5, 0), 100, &gs);
    CHECK("FED01 iso=0 mature → GATE_PASS",   r1 == GATE_PASS,  "got=%d", r1);

    GateResult r2 = fed_gate(make_packed(5, 1), 100, &gs);
    CHECK("FED02 iso=1 → GATE_DROP",          r2 == GATE_DROP,  "got=%d", r2);

    GateResult r3 = fed_gate(make_packed(3, 0), 2, &gs);
    CHECK("FED03 op_count<8 → GATE_GHOST",    r3 == GATE_GHOST, "got=%d", r3);

    GateResult r4 = fed_gate(make_packed(53, 0), 999, &gs);
    CHECK("FED04 lane=53 mature → GATE_PASS", r4 == GATE_PASS,  "got=%d", r4);

    CHECK("FED05a gate.passed >= 2",  gs.passed  >= 2, "passed=%llu",  (unsigned long long)gs.passed);
    CHECK("FED05b gate.dropped >= 1", gs.dropped >= 1, "dropped=%llu", (unsigned long long)gs.dropped);
}

static void test_backpressure(void)
{
    SECTION("FED06-FED08  Backpressure");
    BackpressureCtx bp; bp_init(&bp);

    CHECK("FED06 fresh → no throttle", bp_check(&bp) == 0, "depth=%u", bp.queue_depth);

    for (uint32_t i = 0; i < FED_QUEUE_HWM + 1; i++) bp_push(&bp);
    CHECK("FED07 HWM → throttle=1", bp_check(&bp) == 1, "depth=%u", bp.queue_depth);

    while (bp.queue_depth > FED_QUEUE_LWM / 2) bp_pop(&bp);
    CHECK("FED08 drain → throttle=0", bp_check(&bp) == 0, "depth=%u", bp.queue_depth);
}

static void test_shadow_snapshot(void)
{
    SECTION("FED09-FED11  Shadow Snapshot");
    ShadowSnapshot ss;
    int r = ss_init(&ss, "/tmp/fed_test_vault");
    CHECK("FED09 ss_init returns 0",   r == 0,         "r=%d",      r);
    CHECK("FED09b active starts at 0", ss.active == 0, "active=%u", ss.active);

    int r1 = ss_commit(&ss);
    CHECK("FED10 ss_commit returns 0",     r1 == 0,        "rc=%d",     r1);
    CHECK("FED10b active swapped to 1",    ss.active == 1, "active=%u", ss.active);

    ss_commit(&ss);
    CHECK("FED11 second commit wraps to 0", ss.active == 0, "active=%u", ss.active);
    ss_close(&ss);
}

static void test_early_merkle(void)
{
    SECTION("FED12-FED15  Early Merkle");
    EarlyMerkle em; em_init(&em);

    int all_zero = 1;
    for (uint32_t i = 0; i < FED_TILE_COUNT && all_zero; i++)
        for (int b = 0; b < (int)FED_TILE_HASH_SZ; b++)
            if (em.tile_hash[i][b]) { all_zero = 0; break; }
    CHECK("FED12 fresh tiles all zero", all_zero, "tile[0][0]=0x%02x", em.tile_hash[0][0]);

    uint8_t buf[8] = {1,2,3,4,5,6,7,8};
    em_update(&em, 5, buf, 8);
    CHECK("FED13 em_update changes tile[5]", em.tile_hash[5][0] != 0,
          "tile[5][0]=0x%02x", em.tile_hash[5][0]);

    EarlyMerkle em2; em_init(&em2);
    em_update(&em2, 5, buf, 8);
    CHECK("FED14 em_update deterministic",
          memcmp(em.tile_hash[5], em2.tile_hash[5], FED_TILE_HASH_SZ) == 0,
          "tile[5][0]: a=0x%02x b=0x%02x", em.tile_hash[5][0], em2.tile_hash[5][0]);

    em_reduce(&em);
    CHECK("FED15 em_reduce sets root_valid=1", em.root_valid == 1, "rv=%u", em.root_valid);
    CHECK("FED15b root[0] non-zero after update+reduce", em.root[0] != 0, "root[0]=0x%02x", em.root[0]);
}

static void test_fed_write(void)
{
    SECTION("FED16-FED20  fed_write full path");
    FederationCtx f;
    CHECK("FED16 fed_init returns 0", fed_init(&f, "/tmp/fed_test_write") == 0, "r=-1");

    GateResult gr = fed_write(&f, make_packed(5, 0), 0x100ULL, 0xDEADBEEFULL);
    CHECK("FED17 first write PASS or GHOST", gr == GATE_PASS || gr == GATE_GHOST, "gr=%d", gr);

    uint64_t op_mid = f.op_count;
    fed_write(&f, make_packed(5, 1), 0x200ULL, 0xCAFEULL);
    CHECK("FED18 iso=1 DROP no op_count increment", f.op_count == op_mid,
          "op=%llu expected=%llu", (unsigned long long)f.op_count, (unsigned long long)op_mid);

    int ok = 1;
    for (uint32_t i = 0; i < 100; i++)
        if (fed_write(&f, make_packed(i%54, 0), (uint64_t)i*1000, (uint64_t)i) == GATE_DROP)
            { ok = 0; break; }
    CHECK("FED19 100 mature writes no DROP", ok, "failed");

    CHECK("FED20 gate counters non-zero",
          f.gate.passed + f.gate.ghosted + f.gate.dropped > 0, "all zero");
    fed_close(&f);
}

static void test_fed_commit(void)
{
    SECTION("FED21-FED24  fed_commit");
    FederationCtx f;
    fed_init(&f, "/tmp/fed_test_commit");

    for (uint32_t i = 0; i < 50; i++)
        fed_write(&f, make_packed(i%54, 0), (uint64_t)i*500, (uint64_t)i);

    CHECK("FED21 fed_commit returns 0", fed_commit(&f) == 0, "rc=-1");
    CHECK("FED22 active swapped",       f.ss.active == 1, "active=%u", f.ss.active);

    int tiles_reset = 1;
    for (uint32_t i = 0; i < FED_TILE_COUNT && tiles_reset; i++)
        for (int b = 0; b < (int)FED_TILE_HASH_SZ; b++)
            if (f.em.tile_hash[i][b]) { tiles_reset = 0; break; }
    CHECK("FED23 em tiles reset after commit", tiles_reset, "tile[0][0]=0x%02x", f.em.tile_hash[0][0]);

    for (uint32_t i = 0; i < 20; i++)
        fed_write(&f, make_packed(i%54, 0), (uint64_t)i, (uint64_t)i);
    CHECK("FED24 second fed_commit returns 0", fed_commit(&f) == 0, "rc=-1");
    fed_close(&f);
}

static void test_recovery(void)
{
    SECTION("FED25-FED27  Recovery");
    Delta_DualRecovery dr = fed_recover("/tmp/fed_test_recovery");
    CHECK("FED25 world_a clean or new",
          dr.world_a == DELTA_RECOVERY_CLEAN || dr.world_a == DELTA_RECOVERY_NEW,
          "world_a=%d", dr.world_a);
    CHECK("FED26 world_b clean or new",
          dr.world_b == DELTA_RECOVERY_CLEAN || dr.world_b == DELTA_RECOVERY_NEW,
          "world_b=%d", dr.world_b);
    CHECK("FED27 NULL vault → -1", fed_init(&(FederationCtx){0}, NULL) == -1, "r=0");
}

static void test_stress(void)
{
    SECTION("FED28-FED30  Stress + Determinism");
    FederationCtx f;
    fed_init(&f, "/tmp/fed_test_stress");
    uint64_t drops = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        if (fed_write(&f, make_packed(i%54, 0), (uint64_t)i*777, (uint64_t)i) == GATE_DROP)
            drops++;
        if (i % 128 == 127) fed_drain(&f, 256);
    }
    CHECK("FED28 1000 writes 0 drops", drops == 0, "drops=%llu", (unsigned long long)drops);
    CHECK("FED29 bulk commit ok",      fed_commit(&f) == 0, "rc=-1");

    /* FED30: deterministic merkle root */
    EarlyMerkle ma, mb;
    em_init(&ma); em_init(&mb);
    uint8_t buf[8] = {0xAB, 0xCD, 0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < 50; i++) {
        buf[0] = (uint8_t)(i & 0xFF);
        em_update(&ma, i%54, buf, 8);
        em_update(&mb, i%54, buf, 8);
    }
    em_reduce(&ma); em_reduce(&mb);
    CHECK("FED30 deterministic root",
          memcmp(ma.root, mb.root, FED_TILE_HASH_SZ) == 0,
          "root[0]: a=0x%02x b=0x%02x", ma.root[0], mb.root[0]);
    fed_close(&f);
}

int main(void)
{
    printf("\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97\n");
    printf("\xe2\x95\x91  POGLS Federation Layer Test Suite (V38-aligned)   \xe2\x95\x91\n");
    printf("\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\n");

    test_gate();
    test_backpressure();
    test_shadow_snapshot();
    test_early_merkle();
    test_fed_write();
    test_fed_commit();
    test_recovery();
    test_stress();

    int total = _pass + _fail;
    printf("\n\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97\n");
    printf("\xe2\x95\x91  RESULT: %d/%d PASS  %s%-5s                            \xe2\x95\x91\n",
           _pass, total, _fail==0?"\xe2\x9c\x85 ":"\xe2\x9d\x8c ", _fail==0?"CLEAN":"FAIL");
    printf("\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\n");
    return _fail == 0 ? 0 : 1;
}
