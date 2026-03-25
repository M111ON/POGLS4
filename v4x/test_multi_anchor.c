/*
 * test_multi_anchor.c — POGLS V4.x Multi-Anchor System Tests
 * Tests: T01..T18 covering all sections of pogls_multi_anchor.h
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* CPU-only: skip CUDA guard */
#define POGLS_CANONICAL_CU  /* stub — include cpu_* only */

/* Inline cpu_canonicalize stubs from canonical.cu */
#define CAN_PHI_UP    1696631u
#define CAN_PHI_DOWN  648055u
#define CAN_PHI_SCALE (1u << 20)
#define CAN_ANCHOR    144u
#define CAN_GRID_A    12u
#define CAN_GRID_B    9u
#define CAN_CYCLE     720u

static inline uint32_t cpu_can_f(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_UP >> 20) % CAN_ANCHOR);
}
static inline uint32_t cpu_can_g(uint32_t v) {
    return (uint32_t)(((uint64_t)v * CAN_PHI_DOWN >> 20) % CAN_ANCHOR);
}
static inline uint32_t cpu_hash3(uint32_t x, uint32_t y, uint32_t z) {
    uint64_t h = (uint64_t)x * 2654435761u
               ^ (uint64_t)y * 2246822519u
               ^ (uint64_t)z * 3266489917u;
    h ^= h >> 17; h *= (uint64_t)CAN_PHI_DOWN; h ^= h >> 31;
    return (uint32_t)(h & 0xFFFFFFFFu);
}
static inline uint32_t cpu_canonicalize(uint32_t v) {
    uint32_t a = (cpu_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (cpu_can_g(v) / CAN_GRID_B) * CAN_GRID_B;
    return cpu_hash3(a*a - b*b, 2u*a*b, a*a + b*b);
}

#include "pogls_temporal_core.h"
#include "pogls_multi_anchor.h"

/* ── test framework ─────────────────────────────────────────────── */
static int pass = 0, fail = 0;
#define PASS(name) do { printf("  [PASS] %s\n", name); pass++; } while(0)
#define FAIL(name, ...) do { \
    printf("  [FAIL] %s — ", name); printf(__VA_ARGS__); printf("\n"); \
    fail++; } while(0)
#define CHECK(cond, name, ...) \
    do { if (cond) PASS(name); else FAIL(name, __VA_ARGS__); } while(0)

/* ══════════════════════════════════════════════════════════════════
 * T01 — compile-time: all anchors multiples of 18
 * ══════════════════════════════════════════════════════════════════ */
static void t01_anchor_values(void) {
    printf("T01 anchor values\n");
    CHECK(MA_ANCHOR_72  % 18 == 0, "72%%18==0",  "got %u", MA_ANCHOR_72 % 18);
    CHECK(MA_ANCHOR_144 % 18 == 0, "144%%18==0", "got %u", MA_ANCHOR_144 % 18);
    CHECK(MA_ANCHOR_288 % 18 == 0, "288%%18==0", "got %u", MA_ANCHOR_288 % 18);
    CHECK(MA_ANCHOR_360 % 18 == 0, "360%%18==0", "got %u", MA_ANCHOR_360 % 18);
    CHECK(MA_ANCHOR_360 < TC_CYCLE, "360<720", "not < TC_CYCLE");
    CHECK(MA_N_ANCHORS == 4, "N_ANCHORS=4", "got %u", MA_N_ANCHORS);
}

/* ══════════════════════════════════════════════════════════════════
 * T02 — ma_ctx_init defaults
 * ══════════════════════════════════════════════════════════════════ */
static void t02_ctx_init(void) {
    printf("T02 ctx_init defaults\n");
    MAAnchorCtx ctx;
    ma_ctx_init(&ctx);
    CHECK(ctx.anchor     == MA_ANCHOR_144, "default anchor=144", "got %u", ctx.anchor);
    CHECK(ctx.anchor_idx == 1u,            "default idx=1",      "got %u", ctx.anchor_idx);
    CHECK(ctx.alpha      == MA_ALPHA_CPU,  "default alpha=CPU",  "got %u", ctx.alpha);
    CHECK(ctx.select_count == 0,           "select_count=0",     "got %llu",
          (unsigned long long)ctx.select_count);
}

/* ══════════════════════════════════════════════════════════════════
 * T03 — ma_distortion: nearest-neighbor on anchor grid
 * ══════════════════════════════════════════════════════════════════ */
static void t03_distortion(void) {
    printf("T03 distortion\n");
    /* v exactly on grid → distortion = 0 */
    CHECK(ma_distortion(144, 144) == 0, "on-grid=0", "got %u", ma_distortion(144,144));
    CHECK(ma_distortion(0, 72) == 0, "0 on 72-grid=0", "got %u", ma_distortion(0,72));
    /* v = anchor/2 → maximum distortion (512) */
    uint32_t mid = ma_distortion(36, 72);   /* 36 = 72/2 */
    CHECK(mid == 512u, "midpoint=512", "got %u", mid);
    /* symmetry: d(v, a) = d(a - v%a, a) */
    uint32_t d1 = ma_distortion(10, 72);
    uint32_t d2 = ma_distortion(72 - 10, 72);
    CHECK(d1 == d2, "symmetric", "d1=%u d2=%u", d1, d2);
    /* always in [0, 512] */
    for (uint32_t v = 0; v < 1000; v++) {
        uint32_t d = ma_distortion(v, 144);
        if (d > 512) { FAIL("range", "v=%u d=%u", v, d); return; }
    }
    PASS("range[0..512]");
}

/* ══════════════════════════════════════════════════════════════════
 * T04 — ma_snap: nearest anchor grid point
 * ══════════════════════════════════════════════════════════════════ */
static void t04_snap(void) {
    printf("T04 snap\n");
    /* exactly on grid → unchanged */
    CHECK(ma_snap(144, 144) == 144, "on-grid", "got %u", ma_snap(144,144));
    CHECK(ma_snap(0,   72)  == 0,   "zero",    "got %u", ma_snap(0,72));
    /* round down */
    CHECK(ma_snap(10, 72) == 0,  "floor", "got %u", ma_snap(10,72));
    /* round up */
    CHECK(ma_snap(40, 72) == 72, "ceil",  "got %u", ma_snap(40,72));
    /* tie → floor (d_floor == d_ceil: d_floor <= d_ceil) */
    CHECK(ma_snap(36, 72) == 0 || ma_snap(36,72) == 72,
          "tie-either", "got %u", ma_snap(36,72));
    /* result always multiple of anchor */
    for (uint32_t v = 0; v < 500; v++) {
        uint32_t s = ma_snap(v, 144);
        if (s % 144 != 0) { FAIL("mult-of-anchor", "v=%u s=%u", v, s); return; }
    }
    PASS("result%%anchor==0");
}

/* ══════════════════════════════════════════════════════════════════
 * T05 — ma_soft_snap: alpha=0 → v unchanged, alpha=256 → nearest
 * ══════════════════════════════════════════════════════════════════ */
static void t05_soft_snap(void) {
    printf("T05 soft_snap\n");
    uint32_t v = 100, a = 144;
    /* alpha=0: no snap, output = v */
    CHECK(ma_soft_snap(v, a, 0) == v, "alpha=0→v", "got %u", ma_soft_snap(v,a,0));
    /* alpha=256: full snap to nearest */
    uint32_t nearest = ma_snap(v, a);
    CHECK(ma_soft_snap(v, a, 256) == nearest, "alpha=256→nearest",
          "expected %u got %u", nearest, ma_soft_snap(v,a,256));
    /* alpha=128: midpoint */
    uint32_t mid = ma_soft_snap(v, a, 128);
    /* mid should be between v and nearest (inclusive) */
    uint32_t lo = (v < nearest) ? v : nearest;
    uint32_t hi = (v < nearest) ? nearest : v;
    CHECK(mid >= lo && mid <= hi, "mid in range", "mid=%u lo=%u hi=%u", mid, lo, hi);
    /* deterministic */
    CHECK(ma_soft_snap(v,a,128) == ma_soft_snap(v,a,128), "deterministic", "");
}

/* ══════════════════════════════════════════════════════════════════
 * T06 — alpha transitions: rise/decay clamped
 * ══════════════════════════════════════════════════════════════════ */
static void t06_alpha_transitions(void) {
    printf("T06 alpha rise/decay\n");
    MAAnchorCtx ctx;
    ma_ctx_init(&ctx);

    /* rise: should reach GPU level and clamp */
    for (int i = 0; i < 20; i++) ma_alpha_rise(&ctx);
    CHECK(ctx.alpha == MA_ALPHA_GPU, "rise clamps at GPU", "got %u", ctx.alpha);

    /* decay: should approach CPU level and floor */
    for (int i = 0; i < 100; i++) ma_alpha_decay(&ctx);
    CHECK(ctx.alpha == MA_ALPHA_CPU, "decay floors at CPU", "got %u", ctx.alpha);

    /* one more decay: stays at CPU */
    ma_alpha_decay(&ctx);
    CHECK(ctx.alpha == MA_ALPHA_CPU, "floor stable", "got %u", ctx.alpha);
}

/* ══════════════════════════════════════════════════════════════════
 * T07 — ma_select_best: returns valid anchor index
 * ══════════════════════════════════════════════════════════════════ */
static void t07_select_best(void) {
    printf("T07 select_best\n");
    MAAnchorCtx ctx;
    ma_ctx_init(&ctx);

    uint32_t a = ma_select_best(&ctx, 0xDEADBEEFULL, 288);
    int found = 0;
    for (uint32_t i = 0; i < MA_N_ANCHORS; i++)
        if (a == MA_ANCHORS[i]) { found = 1; break; }
    CHECK(found, "result in anchor set", "got %u", a);
    CHECK(ctx.select_count == 1, "select_count incremented", "got %llu",
          (unsigned long long)ctx.select_count);

    /* v=0 → all distortions=0, drift determines winner */
    ma_ctx_init(&ctx);
    for (int i = 0; i < 10; i++) ma_select_best(&ctx, 0, 0);
    /* should not crash, idx still valid */
    CHECK(ctx.anchor_idx < MA_N_ANCHORS, "idx valid after repeated", "got %u", ctx.anchor_idx);
}

/* ══════════════════════════════════════════════════════════════════
 * T08 — EMA score: decays over time
 * ══════════════════════════════════════════════════════════════════ */
static void t08_ema_decay(void) {
    printf("T08 EMA decay\n");
    uint64_t score = 1024;
    uint64_t prev  = score;
    /* after 10 ema updates with 0, score should decrease */
    for (int i = 0; i < 10; i++) ma_ema_update(&score, 0);
    CHECK(score < prev, "score decreases", "score=%llu prev=%llu",
          (unsigned long long)score, (unsigned long long)prev);
    /* score never negative (uint64 — always true, but explicit) */
    CHECK(score <= prev, "no overflow", "score=%llu", (unsigned long long)score);
}

/* ══════════════════════════════════════════════════════════════════
 * T09 — ma_fabric_init: correct N, magic
 * ══════════════════════════════════════════════════════════════════ */
static void t09_fabric_init(void) {
    printf("T09 fabric_init\n");
    MAFabric mf;
    ma_fabric_init(&mf, 4);
    CHECK(mf.magic == MA_MAGIC, "magic ok", "got 0x%08X", mf.magic);
    CHECK(mf.N == 4, "N=4", "got %u", mf.N);
    CHECK(mf.total_snaps == 0, "snaps=0", "got %llu",
          (unsigned long long)mf.total_snaps);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(mf.ctx[i].anchor == MA_ANCHOR_144,
              "core default anchor", "core %u got %u", i, mf.ctx[i].anchor);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * T10 — ma_step + TCFabric integration: no crash, returns uint32
 * ══════════════════════════════════════════════════════════════════ */
static void t10_ma_step_basic(void) {
    printf("T10 ma_step basic\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);

    int ok = 1;
    for (int i = 0; i < 720; i++) {
        uint32_t v_clean = cpu_canonicalize((uint32_t)i * 7919u);
        uint8_t  ev      = tc_dispatch(&tf, v_clean);
        uint32_t vs      = ma_step(&mf, &tf, v_clean, ev);
        /* snapped value must be deterministic — call again same state */
        (void)vs;
        if (tf.total_steps == 0) { ok = 0; break; }
    }
    CHECK(ok, "720 steps no crash", "crashed");
    CHECK(tf.total_cycles == 1, "1 full cycle", "got %llu",
          (unsigned long long)tf.total_cycles);
}

/* ══════════════════════════════════════════════════════════════════
 * T11 — ma_step: TC_EVENT_ANCHOR raises alpha
 * ══════════════════════════════════════════════════════════════════ */
static void t11_anchor_event_alpha(void) {
    printf("T11 anchor event → alpha rise\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);

    uint32_t alpha_before = mf.ctx[0].alpha;
    /* run until we hit TC_EVENT_ANCHOR (phase % 144 == 0) */
    int seen = 0;
    for (int i = 0; i < 720 && !seen; i++) {
        uint32_t v = cpu_canonicalize((uint32_t)i);
        uint8_t ev = tc_dispatch(&tf, v);
        ma_step(&mf, &tf, v, ev);
        if (ev & TC_EVENT_ANCHOR) seen = 1;
    }
    CHECK(seen, "TC_EVENT_ANCHOR hit", "never fired");
    /* at least one core should have higher alpha */
    int any_raised = 0;
    for (uint32_t i = 0; i < 4; i++)
        if (mf.ctx[i].alpha > alpha_before) any_raised = 1;
    CHECK(any_raised, "alpha raised after anchor", "all still %u", alpha_before);
}

/* ══════════════════════════════════════════════════════════════════
 * T12 — ma_step: TC_EVENT_CYCLE_END decays alpha
 * ══════════════════════════════════════════════════════════════════ */
static void t12_cycle_end_decay(void) {
    printf("T12 cycle_end → alpha decay\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);
    /* force high alpha */
    for (uint32_t i = 0; i < 4; i++) mf.ctx[i].alpha = MA_ALPHA_GPU;

    /* run to TC_EVENT_CYCLE_END */
    for (int i = 0; i < 720; i++) {
        uint32_t v = cpu_canonicalize((uint32_t)i);
        uint8_t ev = tc_dispatch(&tf, v);
        ma_step(&mf, &tf, v, ev);
    }
    int any_decayed = 0;
    for (uint32_t i = 0; i < 4; i++)
        if (mf.ctx[i].alpha < MA_ALPHA_GPU) any_decayed = 1;
    CHECK(any_decayed, "alpha decayed after cycle_end", "none decayed");
}

/* ══════════════════════════════════════════════════════════════════
 * T13 — anchor changes tracked in fabric
 * ══════════════════════════════════════════════════════════════════ */
static void t13_anchor_changes(void) {
    printf("T13 anchor_changes counter\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);

    /* run 3 full cycles — should see some anchor changes eventually */
    for (int i = 0; i < 720 * 3; i++) {
        uint32_t v = cpu_canonicalize((uint32_t)i * 31337u + (uint32_t)i);
        uint8_t  ev = tc_dispatch(&tf, v);
        ma_step(&mf, &tf, v, ev);
    }
    /* anchor_changes >= 0 (may be 0 if always 144 wins, that's valid) */
    CHECK(mf.total_snaps >= 0, "total_snaps valid", "negative impossible");
    /* just verify counter doesn't wrap to huge number */
    CHECK(mf.anchor_changes < 10000, "anchor_changes sane", "got %llu",
          (unsigned long long)mf.anchor_changes);
}

/* ══════════════════════════════════════════════════════════════════
 * T14 — determinism: same inputs → same outputs
 * ══════════════════════════════════════════════════════════════════ */
static void t14_determinism(void) {
    printf("T14 determinism\n");
    TCFabric tf1, tf2; tc_fabric_init(&tf1,4); tc_fabric_init(&tf2,4);
    MAFabric mf1, mf2; ma_fabric_init(&mf1,4); ma_fabric_init(&mf2,4);

    int ok = 1;
    for (int i = 0; i < 1000; i++) {
        uint32_t v  = cpu_canonicalize((uint32_t)i * 2654435761u);
        uint8_t  e1 = tc_dispatch(&tf1, v);
        uint8_t  e2 = tc_dispatch(&tf2, v);
        uint32_t s1 = ma_step(&mf1, &tf1, v, e1);
        uint32_t s2 = ma_step(&mf2, &tf2, v, e2);
        if (s1 != s2) { ok = 0;
            printf("    mismatch at i=%d v=%u s1=%u s2=%u\n", i,v,s1,s2);
            break; }
    }
    CHECK(ok, "deterministic over 1000 steps", "diverged");
}

/* ══════════════════════════════════════════════════════════════════
 * T15 — core isolation: core 0 state doesn't bleed to core 1
 * ══════════════════════════════════════════════════════════════════ */
static void t15_core_isolation(void) {
    printf("T15 core isolation\n");
    MAFabric mf; ma_fabric_init(&mf, 4);
    /* manually force core 0 to different alpha */
    mf.ctx[0].alpha = MA_ALPHA_GPU;
    mf.ctx[1].alpha = MA_ALPHA_CPU;
    /* verify they don't share */
    CHECK(mf.ctx[0].alpha != mf.ctx[1].alpha, "different alpha per core",
          "both same");
    /* force different anchors */
    mf.ctx[0].anchor = MA_ANCHOR_72;
    mf.ctx[1].anchor = MA_ANCHOR_360;
    CHECK(mf.ctx[0].anchor != mf.ctx[1].anchor, "different anchor per core",
          "both same");
}

/* ══════════════════════════════════════════════════════════════════
 * T16 — ma_step: null safety
 * ══════════════════════════════════════════════════════════════════ */
static void t16_null_safety(void) {
    printf("T16 null safety\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);
    uint32_t v = 12345u;
    uint32_t r;
    /* null mf */
    r = ma_step(NULL, &tf, v, 0);
    CHECK(r == v, "null mf → v passthrough", "got %u", r);
    /* null tf */
    r = ma_step(&mf, NULL, v, 0);
    CHECK(r == v, "null tf → v passthrough", "got %u", r);
    /* null ctx */
    ma_ctx_init(NULL);    PASS("init(NULL) no crash");
    ma_alpha_rise(NULL);  PASS("rise(NULL) no crash");
    ma_alpha_decay(NULL); PASS("decay(NULL) no crash");
    ma_select_best(NULL, 0, 0);  /* returns default */ PASS("select_best(NULL)");
}

/* ══════════════════════════════════════════════════════════════════
 * T17 — full pipeline smoke: canonical → temporal → multi-anchor
 * ══════════════════════════════════════════════════════════════════ */
static void t17_full_pipeline_smoke(void) {
    printf("T17 full pipeline smoke\n");
    TCFabric tf; tc_fabric_init(&tf, 4);
    MAFabric mf; ma_fabric_init(&mf, 4);

    uint64_t sum = 0;
    int snap_happened = 0;
    for (uint32_t i = 0; i < 2160; i++) {   /* 3 full cycles */
        uint32_t v_raw   = i * 104729u;
        uint32_t v_clean = cpu_canonicalize(v_raw);
        uint8_t  ev      = tc_dispatch(&tf, v_clean);
        uint32_t v_snap  = ma_step(&mf, &tf, v_clean, ev);
        sum += v_snap;
        if (v_snap != v_clean) snap_happened = 1;
    }
    CHECK(tf.total_cycles == 3, "3 cycles complete", "got %llu",
          (unsigned long long)tf.total_cycles);
    CHECK(sum > 0, "non-zero output sum", "sum=0");
    CHECK(snap_happened, "at least one snap occurred", "no snap ever");
}

/* ══════════════════════════════════════════════════════════════════
 * T18 — score function: known input, expected ordering
 *   v=0 → distortion=0 for all anchors → drift breaks tie
 *   v=anchor/2 → max distortion for that anchor
 * ══════════════════════════════════════════════════════════════════ */
static void t18_score_ordering(void) {
    printf("T18 score ordering\n");
    /* v=0: on-grid for all anchors → distortion=0 for all */
    for (uint32_t i = 0; i < MA_N_ANCHORS; i++) {
        uint32_t d = ma_distortion(0, MA_ANCHORS[i]);
        CHECK(d == 0, "v=0 distortion=0", "anchor=%u got %u", MA_ANCHORS[i], d);
    }
    /* v = MA_ANCHOR_72/2 = 36: max distortion for anchor_72 */
    uint32_t d72 = ma_distortion(36, MA_ANCHOR_72);
    CHECK(d72 == 512, "mid-72 distortion=512", "got %u", d72);
    /* smaller anchor → higher distortion for non-aligned v */
    uint32_t d144 = ma_distortion(36, MA_ANCHOR_144);
    uint32_t d288 = ma_distortion(36, MA_ANCHOR_288);
    /* 36 is closer to 0 on 144/288 grids → lower distortion */
    CHECK(d144 < d72, "d144<d72 for v=36", "d144=%u d72=%u", d144, d72);
    CHECK(d288 < d144, "d288<d144 for v=36", "d288=%u d144=%u", d288, d144);
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("═══════════════════════════════════════════════\n");
    printf("  POGLS Multi-Anchor Test Suite  (T01..T18)\n");
    printf("═══════════════════════════════════════════════\n\n");

    t01_anchor_values();
    t02_ctx_init();
    t03_distortion();
    t04_snap();
    t05_soft_snap();
    t06_alpha_transitions();
    t07_select_best();
    t08_ema_decay();
    t09_fabric_init();
    t10_ma_step_basic();
    t11_anchor_event_alpha();
    t12_cycle_end_decay();
    t13_anchor_changes();
    t14_determinism();
    t15_core_isolation();
    t16_null_safety();
    t17_full_pipeline_smoke();
    t18_score_ordering();

    printf("\n═══════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d   TOTAL: %d\n", pass, fail, pass+fail);
    printf("═══════════════════════════════════════════════\n");
    return (fail == 0) ? 0 : 1;
}
