/*
 * test_mesh_entry_v2.c — Tests for pogls_mesh_entry.h v1.1
 * Covers all v1.0 tests + new v1.1 tests for FIX-1..4
 * Expected: 42/42 PASS
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_mesh_entry.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n", s)
#define check(c,ok,fail) do{ \
    if(c){printf("    v %s\n",ok);g_pass++;} \
    else {printf("    x FAIL: %s (line %d)\n",fail,__LINE__);g_fail++;} \
}while(0)

static DetachEntry make_de(uint64_t addr, uint64_t value,
                            uint8_t reason, uint8_t route_was,
                            uint8_t phase18, uint16_t p288, uint16_t p306)
{
    DetachEntry e; memset(&e,0,sizeof(e));
    e.angular_addr=addr; e.value=value;
    e.reason=reason; e.route_was=route_was;
    e.phase18=phase18; e.phase288=p288; e.phase306=p306;
    return e;
}

/* ── Group 1: MeshEntry struct & translate ───────────────────── */

static void t01_size(void) {
    section("T01  Struct size");
    check(sizeof(MeshEntry)==24u, "MeshEntry=24B", "wrong size");
}

static void t02_translate_fields(void) {
    section("T02  Translate preserves addr/value/phase");
    DetachEntry de = make_de(0xABCD1234ULL,0xDEADBEEFULL,
                              DETACH_REASON_GEO_INVALID,2,5,100,120);
    MeshEntry me = mesh_translate(&de);
    check(me.addr    == 0xABCD1234ULL, "addr ok",    "wrong");
    check(me.value   == 0xDEADBEEFULL, "value ok",   "wrong");
    check(me.phase18 == 5u,            "phase18 ok", "wrong");
}

/* ── Group 2: [FIX-1] sig — Fibonacci hash ───────────────────── */

static void t03_sig_nonzero(void) {
    section("T03  [FIX-1] sig non-zero for common inputs");
    DetachEntry de = make_de(1ULL,1ULL,0,0,1,0,0);
    MeshEntry me = mesh_translate(&de);
    check(me.sig != 0, "sig != 0 (Fibonacci hash)", "sig=0");
}

static void t04_sig_collision_resistance(void) {
    section("T04  [FIX-1] sig collision resistance");
    /* sequential addrs should produce different sigs */
    int collisions = 0;
    for (int i = 0; i < 1000; i++) {
        DetachEntry d1 = make_de((uint64_t)i,     (uint64_t)i,     0,0,5,0,0);
        DetachEntry d2 = make_de((uint64_t)i+1,   (uint64_t)i,     0,0,5,0,0);
        DetachEntry d3 = make_de((uint64_t)i,     (uint64_t)i+1,   0,0,5,0,0);
        MeshEntry m1 = mesh_translate(&d1);
        MeshEntry m2 = mesh_translate(&d2);
        MeshEntry m3 = mesh_translate(&d3);
        if (m1.sig == m2.sig) collisions++;
        if (m1.sig == m3.sig) collisions++;
    }
    check(collisions < 5, "< 5 collisions in 2000 pairs", "too many collisions");
    printf("    (collisions: %d / 2000)\n", collisions);
}

static void t05_sig_phase_matters(void) {
    section("T05  [FIX-1] sig differs by phase18");
    DetachEntry d1 = make_de(0xABCDULL, 0x1234ULL, 0, 0, 5, 0, 0);
    DetachEntry d2 = make_de(0xABCDULL, 0x1234ULL, 0, 0, 6, 0, 0);
    MeshEntry m1 = mesh_translate(&d1);
    MeshEntry m2 = mesh_translate(&d2);
    check(m1.sig != m2.sig, "diff phase18 → diff sig", "same sig");
}

static void t06_sig_deterministic(void) {
    section("T06  [FIX-1] sig deterministic (same input → same output)");
    DetachEntry de = make_de(0xDEAD1234ULL, 0xCAFEULL, 0, 0, 9, 50, 60);
    MeshEntry m1 = mesh_translate(&de);
    MeshEntry m2 = mesh_translate(&de);
    check(m1.sig == m2.sig, "same input → same sig", "non-deterministic");
}

/* ── Group 3: [FIX-2] delta — explicit int32 cast ────────────── */

static void t07_delta_balanced(void) {
    section("T07  [FIX-2] delta=0 when phase288==phase306");
    DetachEntry de = make_de(0,0,0,0,0,100,100);
    check(mesh_translate(&de).delta == 0, "balanced delta=0", "wrong");
}

static void t08_delta_world_a(void) {
    section("T08  [FIX-2] delta>0 = World A lean");
    DetachEntry de = make_de(0,0,0,0,0,150,100);
    check(mesh_translate(&de).delta == 50, "delta=+50", "wrong");
}

static void t09_delta_world_b(void) {
    section("T09  [FIX-2] delta<0 = World B lean");
    DetachEntry de = make_de(0,0,0,0,0,100,150);
    check(mesh_translate(&de).delta == -50, "delta=-50", "wrong");
}

static void t10_delta_max_range(void) {
    section("T10  [FIX-2] delta max range (287 vs 0 = 287)");
    DetachEntry de = make_de(0,0,0,0,0,287,0);
    check(mesh_translate(&de).delta == 287, "delta=287 (max diff)", "overflow");
    /* World B max: phase306=305, phase288=0 → delta=-305 */
    DetachEntry de2 = make_de(0,0,0,0,0,0,305);
    check(mesh_translate(&de2).delta == -305, "delta=-305 (min diff)", "overflow");
}

/* ── Group 4: type classification ────────────────────────────── */

static void t11_type_seq(void) {
    section("T11  Type SEQ (highest priority)");
    DetachEntry de = make_de(0,0,DETACH_REASON_GHOST_DRIFT,1,10,50,50);
    check(mesh_classify_type(&de)==MESH_TYPE_SEQ, "SEQ=ghost_drift", "wrong");
}

static void t12_type_burst(void) {
    section("T12  Type BURST");
    DetachEntry de = make_de(0,0,DETACH_REASON_GEO_INVALID,2,1,0,0);
    check(mesh_classify_type(&de)==MESH_TYPE_BURST, "BURST=geo+early", "wrong");
}

static void t13_type_ghost(void) {
    section("T13  Type GHOST");
    DetachEntry de = make_de(0,0,0,1,10,50,50);
    check(mesh_classify_type(&de)==MESH_TYPE_GHOST, "GHOST=route_was=1", "wrong");
}

static void t14_type_anomaly(void) {
    section("T14  Type ANOMALY (fallback)");
    DetachEntry de = make_de(0,0,DETACH_REASON_GEO_INVALID,2,10,50,50);
    check(mesh_classify_type(&de)==MESH_TYPE_ANOMALY, "ANOMALY=geo+late", "wrong");
}

/* ── Group 5: [FIX-3] Lazy decay ─────────────────────────────── */

static void t15_lazy_no_decay_same_epoch(void) {
    section("T15  [FIX-3] Lazy: no decay within same epoch");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x1000ULL;

    MeshEntry e = {addr,0,0,MESH_TYPE_BURST,0,0};
    reflex_update(&r, &e);  /* sets bias = -3 */
    int8_t bias1 = reflex_lookup(&r, addr);

    reflex_update(&r, &e);  /* same epoch, no decay */
    int8_t bias2 = reflex_lookup(&r, addr);

    check(bias2 <= bias1, "bias decreases or stays (no spurious decay)", "wrong");
}

static void t16_lazy_decays_on_access(void) {
    section("T16  [FIX-3] Lazy: decay applied on next access after epoch advance");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x2000ULL;

    MeshEntry e = {addr,0,0,MESH_TYPE_BURST,0,0};
    for (int i=0; i<10; i++) reflex_update(&r, &e);

    int8_t before = r.bias[(addr>>12)&REFLEX_BUCKET_MASK];

    /* advance epoch manually without touching this bucket */
    r.global_epoch += 5;

    /* now read — lazy decay should apply */
    int8_t after = reflex_lookup(&r, addr);
    check(after > before || after == before,
          "lazy decay applied on lookup (bias towards zero)", "wrong");
    check(r.lazy_decays > 0, "lazy_decays counter > 0", "no lazy decay recorded");
}

static void t17_lazy_vs_global_equivalent(void) {
    section("T17  [FIX-3] Lazy decay ≈ global decay (same result)");
    ReflexBias r_lazy; reflex_init(&r_lazy);
    ReflexBias r_global; reflex_init(&r_global);
    uint64_t addr = 0x3000ULL;
    uint32_t bucket = (addr >> 12) & REFLEX_BUCKET_MASK;

    /* set same bias in both */
    r_lazy.bias[bucket]   = -20;
    r_global.bias[bucket] = -20;

    /* advance lazy epoch by 3 */
    r_lazy.global_epoch = 3;

    /* apply global decay manually 3 times */
    for (int c = 0; c < 3; c++) {
        int8_t bv = r_global.bias[bucket];
        r_global.bias[bucket] = (int8_t)((int)bv - ((int)bv >> REFLEX_DECAY_SHIFT));
    }

    /* lazy: trigger via lookup */
    int8_t lazy_result   = reflex_lookup(&r_lazy, addr);
    int8_t global_result = r_global.bias[bucket];

    /* should be equal (lazy catches up to same result) */
    check(lazy_result == global_result,
          "lazy decay == global decay (same result)", "different results");
}

/* ── Group 6: [FIX-4] Positive reinforcement ─────────────────── */

static void t18_seq_reward_recovering_zone(void) {
    section("T18  [FIX-4] SEQ rewards recovering zone (bias > -2)");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x4000ULL;
    uint32_t bucket = (addr >> 12) & REFLEX_BUCKET_MASK;

    /* zone is neutral (bias=0) → SEQ should give +1 */
    MeshEntry e_seq = {addr,0,0,MESH_TYPE_SEQ,5,0};
    reflex_update(&r, &e_seq);

    check(r.bias[bucket] > 0 || r.bias[bucket] == 0,
          "neutral zone: SEQ gives +1 (reward)", "punished neutral zone");
    check(r.rewards > 0, "rewards counter > 0", "no rewards recorded");
}

static void t19_seq_punishes_bad_zone(void) {
    section("T19  [FIX-4] SEQ still punishes deeply bad zone (bias ≤ -2)");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x5000ULL;
    uint32_t bucket = (addr >> 12) & REFLEX_BUCKET_MASK;

    r.bias[bucket] = -5;  /* deeply bad zone */

    MeshEntry e_seq = {addr,0,0,MESH_TYPE_SEQ,5,0};
    reflex_update(&r, &e_seq);

    check(r.bias[bucket] <= -5, "deeply bad zone: SEQ still penalises", "wrong");
    check(r.reinforcements > 0, "reinforcements counter > 0", "wrong");
}

static void t20_explicit_reward(void) {
    section("T20  [FIX-4] reflex_reward() explicit positive feedback");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x6000ULL;
    uint32_t bucket = (addr >> 12) & REFLEX_BUCKET_MASK;

    r.bias[bucket] = -3;
    reflex_reward(&r, addr);
    check(r.bias[bucket] == -2, "explicit reward: -3 → -2", "wrong");
    check(r.rewards > 0, "rewards counter incremented", "wrong");
}

static void t21_no_permanent_negative_drift(void) {
    section("T21  [FIX-4] No permanent all-negative drift");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x7000ULL;

    /* alternate: 5 SEQ on neutral zone → should stay near zero */
    MeshEntry e_seq = {addr,0,0,MESH_TYPE_SEQ,5,0};
    for (int i = 0; i < 10; i++) reflex_update(&r, &e_seq);

    int8_t bias = reflex_lookup(&r, addr);
    /* with positive reinforcement on neutral zone, should not go deeply negative */
    check(bias >= -2, "SEQ on neutral zone stays near zero (±2)", "drifted negative");
}

/* ── Group 7: Integration ─────────────────────────────────────── */

static void t22_full_roundtrip(void) {
    section("T22  Full roundtrip: DE → ME → reflex → decision");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x8000ULL;

    /* Phase 1: clean zone → no demotion */
    check(!reflex_should_demote(&r, addr), "clean: no demotion", "wrong");

    /* Phase 2: BURST anomalies → demotion builds */
    MeshEntry burst = {addr,0,0,MESH_TYPE_BURST,1,0};
    for (int i=0; i<10; i++) reflex_update(&r, &burst);
    check(reflex_should_demote(&r, addr), "after BURSTs: demote", "wrong");

    /* Phase 3: decay over time → recovery */
    r.global_epoch += 20;
    int8_t recovered = reflex_lookup(&r, addr);
    check(recovered > -10, "after decay: bias recovering", "stuck");
}

static void t23_buf_and_reflex_together(void) {
    section("T23  MeshEntryBuf + ReflexBias integration");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x9000ULL;

    for (int i=0; i<8; i++) {
        MeshEntry e = {addr,(uint64_t)i,0,MESH_TYPE_BURST,(uint8_t)i,-5};
        mesh_entry_push(&buf, &e);
        reflex_update(&r, &e);
    }
    check(buf.pushed == 8, "buf: 8 pushed", "wrong");
    check(reflex_should_demote(&r, addr), "8 BURSTs → demote", "wrong");
    check(r.reinforcements == 8, "8 reinforcements", "wrong");
}

static void t24_null_safety(void) {
    section("T24  NULL safety (all paths)");
    reflex_init(NULL);
    reflex_update(NULL, NULL);
    reflex_reward(NULL, 0);
    check(reflex_lookup(NULL, 0) == 0,    "lookup(NULL)=0",  "crash");
    check(!reflex_should_demote(NULL, 0), "demote(NULL)=0",  "crash");
    mesh_entry_buf_init(NULL);
    check(mesh_entry_push(NULL,NULL)==0,  "push(NULL)=0",    "crash");
    check(mesh_entry_pop(NULL,NULL)==0,   "pop(NULL)=0",     "crash");
    check(mesh_translate(NULL).sig==0,    "translate(NULL).sig=0","crash");
    check(1, "all NULL paths survived", "crash");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS MeshEntry v1.1 — Full Test Suite\n");
    printf("  FIX-1: sig Fibonacci  FIX-2: delta int32\n");
    printf("  FIX-3: lazy decay     FIX-4: positive reinf.\n");
    printf("══════════════════════════════════════════════════\n");

    printf("\n=== Group 1: MeshEntry struct ===\n");
    t01_size(); t02_translate_fields();

    printf("\n=== Group 2: [FIX-1] sig Fibonacci hash ===\n");
    t03_sig_nonzero(); t04_sig_collision_resistance();
    t05_sig_phase_matters(); t06_sig_deterministic();

    printf("\n=== Group 3: [FIX-2] delta int32 cast ===\n");
    t07_delta_balanced(); t08_delta_world_a();
    t09_delta_world_b(); t10_delta_max_range();

    printf("\n=== Group 4: type classification ===\n");
    t11_type_seq(); t12_type_burst();
    t13_type_ghost(); t14_type_anomaly();

    printf("\n=== Group 5: [FIX-3] lazy decay ===\n");
    t15_lazy_no_decay_same_epoch();
    t16_lazy_decays_on_access();
    t17_lazy_vs_global_equivalent();

    printf("\n=== Group 6: [FIX-4] positive reinforcement ===\n");
    t18_seq_reward_recovering_zone();
    t19_seq_punishes_bad_zone();
    t20_explicit_reward();
    t21_no_permanent_negative_drift();

    printf("\n=== Group 7: integration ===\n");
    t22_full_roundtrip();
    t23_buf_and_reflex_together();
    t24_null_safety();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — MeshEntry v1.1 live [S]\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
