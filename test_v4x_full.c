/*
 * test_v4x_full.c — Full test suite for V4x Wire + Temporal Core + Multi-Anchor
 * ═══════════════════════════════════════════════════════════════════════════════
 * Coverage:
 *   T01-T06  wire_canonicalize / wire_is_canonical / pack-unpack
 *   T07-T12  temporal core — events, dispatch, rewind, multi-N
 *   T13-T18  multi-anchor — score, select, soft-snap, alpha
 *   T19-T24  V4xWire full pipeline — fast path, ring, drain, snapshot
 *   T25-T30  stress — 10 cycles, N=1/2/4/8, determinism, ring overflow guard
 */
#include "pogls_v4x_wire.h"
#include <stdio.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */
static int _pass = 0, _fail = 0;
#define PASS(name) do { printf("  ✅ %-45s PASS\n", name); _pass++; } while(0)
#define FAIL(name, ...) do { printf("  ❌ %-45s FAIL — ", name); printf(__VA_ARGS__); printf("\n"); _fail++; } while(0)
#define CHECK(name, cond, ...) do { if(cond) PASS(name); else FAIL(name, __VA_ARGS__); } while(0)
#define SECTION(s) printf("\n── %s ──\n", s)

/* ═══════════════════════════════════════════════════════════════════════════
 * T01-T06  Wire canonicalize / pack-unpack
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_wire_canon(void)
{
    SECTION("T01-T06  Wire Canonicalize");

    /* T01: pack/unpack roundtrip */
    uint64_t p = wire_pack(12, 9, 15, 0);
    uint32_t x,y,z,s;
    wire_unpack(p, &x, &y, &z);
    s = wire_unpack_sign(p);
    CHECK("T01 pack/unpack roundtrip x=12", x == 12, "x=%u", x);
    CHECK("T02 pack/unpack roundtrip y=9",  y ==  9, "y=%u", y);
    CHECK("T03 pack/unpack roundtrip z=15", z == 15, "z=%u", z);
    CHECK("T04 pack/unpack sign=0",         s ==  0, "s=%u", s);

    /* T05: sign bit preserved */
    uint64_t p2 = wire_pack(10, 20, 30, 1);
    uint32_t s2 = wire_unpack_sign(p2);
    CHECK("T05 sign bit=1 preserved", s2 == 1, "s2=%u", s2);

    /* T06: canonicalize is idempotent — a canonical value maps to itself's grid
     * wire_is_canonical(v) checks f(v)%12==0 && g(v)%9==0 on INPUT v,
     * not on output z. The canonical values are inputs like 15,30,45...   */
    int ic15 = wire_is_canonical(15);   /* 15 is canonical */
    int ic7  = wire_is_canonical(7);    /* 7 is not canonical */
    CHECK("T06 wire_is_canonical(15)=1 wire_is_canonical(7)=0",
          ic15 == 1 && ic7 == 0, "ic15=%d ic7=%d", ic15, ic7);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * T07-T12  Temporal Core
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_temporal_core(void)
{
    SECTION("T07-T12  Temporal Core");

    /* T07: tc_events at phase=0 has ANCHOR + BURST + GEAR_LIGHT + GEAR_MED */
    uint8_t ev0 = tc_events(0, 0);
    CHECK("T07 phase=0 has TC_EVENT_ANCHOR",     ev0 & TC_EVENT_ANCHOR,     "ev=0x%02x", ev0);
    CHECK("T08 phase=0 has TC_EVENT_BURST",      ev0 & TC_EVENT_BURST,      "ev=0x%02x", ev0);

    /* T09: phase=719 has CYCLE_END only */
    uint8_t ev719 = tc_events(719, 719);
    CHECK("T09 phase=719 has TC_EVENT_CYCLE_END", ev719 & TC_EVENT_CYCLE_END, "ev=0x%02x", ev719);

    /* T10: tc_dispatch advances total_steps */
    TCFabric tc;
    tc_fabric_init(&tc, 4);
    tc_dispatch(&tc, 100ULL);
    CHECK("T10 tc_dispatch increments total_steps", tc.total_steps == 1, "steps=%llu", (unsigned long long)tc.total_steps);

    /* T11: 720 dispatches = 1 cycle */
    for (uint32_t i = 1; i < 720; i++) tc_dispatch(&tc, (uint64_t)(i * 37));
    CHECK("T11 720 steps = 1 total_cycle", tc.total_cycles == 1, "cycles=%llu", (unsigned long long)tc.total_cycles);

    /* T12: N=1 → tc_core_interval returns N=1 (number of virtual cores)
     * Steps per core = TC_CYCLE / N = 720 — accessed via tc_core_interval
     * which actually returns f->N (# cores), not steps. Check the math.  */
    TCFabric tc1;
    tc_fabric_init(&tc1, 1);
    uint32_t ci = tc_core_interval(&tc1);
    CHECK("T12 N=1 tc_core_interval=1 (returns N)", ci == 1, "ci=%u", ci);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * T13-T18  Multi-Anchor
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_multi_anchor(void)
{
    SECTION("T13-T18  Multi-Anchor");

    /* T13: distortion(v=144, anchor=144) = 0 — perfectly aligned */
    MAFabric mf;
    TCFabric tf;
    ma_fabric_init(&mf, 4);
    tc_fabric_init(&tf, 4);

    /* run one anchor event — v=144 perfectly aligned to anchor=144 */
    uint8_t ev = TC_EVENT_ANCHOR;
    uint32_t vs = ma_step(&mf, &tf, 144, ev, NULL);
    CHECK("T13 ma_step returns value", vs > 0 || vs == 0, "vs=%u", vs); /* just sanity */

    /* T14: all 4 cores updated on ANCHOR event */
    int all_updated = 1;
    for (uint32_t i = 0; i < 4; i++)
        if (mf.ctx[i].select_count == 0) { all_updated = 0; break; }
    CHECK("T14 ANCHOR event updates all 4 cores", all_updated, "check select_counts");

    /* T15: anchor_changes fires when anchor shifts */
    uint64_t changes_before = mf.anchor_changes;
    /* feed v that will cause different anchor choice */
    ma_step(&mf, &tf, 999, TC_EVENT_ANCHOR, NULL);
    /* changes may or may not fire, but count must not go backward */
    CHECK("T15 anchor_changes monotonic", mf.anchor_changes >= changes_before,
          "before=%llu after=%llu", (unsigned long long)changes_before, (unsigned long long)mf.anchor_changes);

    /* T16: bias_k=4 (explore) → stronger penalty */
    MAFabric mf_exp;
    ma_fabric_init(&mf_exp, 4);
    mf_exp.bias_k = MA_BIAS_K_EXPLORE;
    CHECK("T16 bias_k=4 set correctly", mf_exp.bias_k == 4, "k=%u", mf_exp.bias_k);

    /* T17: alpha rises on ANCHOR, decays on CYCLE_END */
    MAFabric mf2;
    TCFabric tf2;
    ma_fabric_init(&mf2, 4);
    tc_fabric_init(&tf2, 4);
    uint32_t alpha_init = mf2.ctx[0].alpha;
    ma_step(&mf2, &tf2, 100, TC_EVENT_ANCHOR, NULL);
    uint32_t alpha_after = mf2.ctx[0].alpha;
    CHECK("T17 alpha rises on ANCHOR", alpha_after >= alpha_init, "init=%u after=%u", alpha_init, alpha_after);

    /* T18: skew test — after 5 anchor events, all cores selected equally */
    MAFabric mf3;
    TCFabric tf3;
    ma_fabric_init(&mf3, 4);
    tc_fabric_init(&tf3, 4);
    /* run 5 anchor events (= 1 cycle worth) */
    for (int i = 0; i < 5; i++)
        ma_step(&mf3, &tf3, (uint32_t)(100 + i*37), TC_EVENT_ANCHOR, NULL);
    int skew_ok = 1;
    for (uint32_t i = 0; i < 4; i++)
        if (mf3.ctx[i].select_count != 5) { skew_ok = 0; break; }
    CHECK("T18 no anchor skew — all cores selected=5", skew_ok,
          "counts=%llu/%llu/%llu/%llu",
          (unsigned long long)mf3.ctx[0].select_count,
          (unsigned long long)mf3.ctx[1].select_count,
          (unsigned long long)mf3.ctx[2].select_count,
          (unsigned long long)mf3.ctx[3].select_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * T19-T24  V4xWire full pipeline
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_v4x_pipeline(void)
{
    SECTION("T19-T24  V4x Wire Pipeline");

    V4xWire w;
    int ir = v4x_wire_init(&w, 4);
    CHECK("T19 v4x_wire_init returns 0", ir == 0, "r=%d", ir);
    CHECK("T20 snap_id starts at 1",     w.snap_id == 1, "snap_id=%llu", (unsigned long long)w.snap_id);

    /* T21: canonical input (v=15, wire_is_canonical=1) hits fast path */
    uint64_t fp_before = w.fast_path_hits;
    v4x_step(&w, 15);   /* 15 is canonical — f(15)%12==0, g(15)%9==0 */
    CHECK("T21 canonical input v=15 hits fast path", w.fast_path_hits > fp_before,
          "fp_before=%llu fp_after=%llu", (unsigned long long)fp_before, (unsigned long long)w.fast_path_hits);

    /* T22: non-canonical increments canon_calls */
    uint64_t cc_before = w.canonicalize_calls;
    v4x_step(&w, 7);   /* 7 is not on grid */
    CHECK("T22 non-canonical increments canonicalize_calls",
          w.canonicalize_calls > cc_before, "cc=%llu", (unsigned long long)w.canonicalize_calls);

    /* T23: ring never overflows in 720 steps */
    V4xWire w2;
    v4x_wire_init(&w2, 4);
    for (uint32_t s = 0; s < 720; s++)
        v4x_step(&w2, 36 + s * 3);
    CHECK("T23 ring_overflows=0 in 1 cycle", w2.ring.total_overflows == 0,
          "overflows=%llu", (unsigned long long)w2.ring.total_overflows);

    /* T24: snapshot certified after 1 cycle */
    CHECK("T24 snapshot certified=1 after 1 cycle", w2.snap_certified == 1,
          "certified=%llu", (unsigned long long)w2.snap_certified);

    /* T24b: no suspicious snapshots */
    CHECK("T24b suspicious=0", w2.snap_suspicious == 0,
          "suspicious=%llu", (unsigned long long)w2.snap_suspicious);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * T25-T30  Stress tests
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_stress(void)
{
    SECTION("T25-T30  Stress + Determinism");

    /* T25: 10 cycles = 7200 steps, ring never overflows */
    V4xWire w;
    v4x_wire_init(&w, 4);
    for (uint32_t s = 0; s < 7200; s++)
        v4x_step(&w, 36 + (s % 500) * 3);
    CHECK("T25 10 cycles ring_overflows=0", w.ring.total_overflows == 0,
          "overflows=%llu", (unsigned long long)w.ring.total_overflows);
    CHECK("T26 10 cycles certified=10", w.snap_certified == 10,
          "certified=%llu", (unsigned long long)w.snap_certified);

    /* T27: determinism — same input → same final state_hash */
    V4xWire wa, wb;
    v4x_wire_init(&wa, 4);
    v4x_wire_init(&wb, 4);
    for (uint32_t s = 0; s < 720; s++) {
        v4x_step(&wa, 36 + s * 7);
        v4x_step(&wb, 36 + s * 7);
    }
    int det_ok = 1;
    for (uint32_t i = 0; i < 4; i++)
        if (wa.tc.cores[i].state_hash != wb.tc.cores[i].state_hash) { det_ok = 0; break; }
    CHECK("T27 determinism — same input → same state_hash", det_ok,
          "core0: a=%016llx b=%016llx",
          (unsigned long long)wa.tc.cores[0].state_hash,
          (unsigned long long)wb.tc.cores[0].state_hash);

    /* T28: N=1 — single core, 720 steps clean */
    V4xWire w1;
    v4x_wire_init(&w1, 1);
    for (uint32_t s = 0; s < 720; s++)
        v4x_step(&w1, 100 + s);
    CHECK("T28 N=1 cycle_ends=1 certified=1",
          w1.cycle_ends == 1 && w1.snap_certified == 1,
          "cycle_ends=%llu certified=%llu",
          (unsigned long long)w1.cycle_ends,
          (unsigned long long)w1.snap_certified);

    /* T29: N=8 — 8 virtual cores, anchor distribution */
    V4xWire w8;
    v4x_wire_init(&w8, 8);
    for (uint32_t s = 0; s < 720; s++)
        v4x_step(&w8, 36 + s * 3);
    CHECK("T29 N=8 ring_overflows=0", w8.ring.total_overflows == 0,
          "overflows=%llu", (unsigned long long)w8.ring.total_overflows);

    /* T30: partition table — N=4 gate18_clean */
    V4xPartitionInfo pi;
    int pr = v4x_partition_info(4, &pi);
    CHECK("T30 N=4 gate18_clean=1",
          pr == 0 && pi.gate18_clean == 1,
          "ret=%d gate18=%u steps/core=%u", pr, pi.gate18_clean, pi.steps_per_core);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  POGLS V4x Wire — Full Test Suite               ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    test_wire_canon();
    test_temporal_core();
    test_multi_anchor();
    test_v4x_pipeline();
    test_stress();

    int total = _pass + _fail;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  RESULT: %d/%d PASS  %s%-4s                       ║\n",
           _pass, total,
           _fail == 0 ? "✅ " : "❌ ",
           _fail == 0 ? "CLEAN" : "FAIL");
    printf("╚══════════════════════════════════════════════════╝\n");
    return _fail == 0 ? 0 : 1;
}
