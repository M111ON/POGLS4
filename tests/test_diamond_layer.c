/*
 * test_diamond_layer.c — Tests for pogls_diamond_layer.h
 * Expected: ALL PASS
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_diamond_layer.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n", s)
#define check(c,ok,fail) do{ \
    if(c){printf("    v %s\n",ok);g_pass++;} \
    else {printf("    x FAIL: %s (line %d)\n",fail,__LINE__);g_fail++;} \
}while(0)

/* helper: make MeshEntry */
static MeshEntry make_me(uint64_t addr, uint8_t type, int16_t delta)
{
    MeshEntry m; memset(&m,0,sizeof(m));
    m.addr  = addr;
    m.type  = type;
    m.delta = delta;
    return m;
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 1: diamond_id                                            */
/* ═══════════════════════════════════════════════════════════════ */

static void t01_id_range(void) {
    section("T01  diamond_id always in [0..63]");
    int out_of_range = 0;
    /* scan many (a,b,delta) combinations */
    for (int32_t a = 0; a < (1<<20); a += 4096) {
        for (int32_t b = 0; b < (1<<20); b += 4096) {
            for (int16_t d = -305; d <= 287; d += 50) {
                uint32_t id = diamond_id(a, b, d);
                if (id >= DIAMOND_COUNT) out_of_range++;
            }
        }
    }
    check(out_of_range == 0, "all diamond_id in [0..63]", "out of range");
}

static void t02_id_deterministic(void) {
    section("T02  diamond_id deterministic");
    int32_t a=500000, b=300000; int16_t d=50;
    uint32_t id1 = diamond_id(a, b, d);
    uint32_t id2 = diamond_id(a, b, d);
    check(id1 == id2, "same input → same id", "non-deterministic");
}

static void t03_id_delta_matters(void) {
    section("T03  delta injection changes id");
    int32_t a=500000, b=300000;
    uint32_t id_d0   = diamond_id(a, b,   0);
    uint32_t id_d100 = diamond_id(a, b, 100);
    uint32_t id_dn50 = diamond_id(a, b, -50);
    /* at least some should differ */
    int any_diff = (id_d0 != id_d100) || (id_d0 != id_dn50);
    check(any_diff, "different delta → different id (at least some)", "all same");
}

static void t04_id_distribution(void) {
    section("T04  diamond_id distribution across 64 buckets");
    uint32_t hist[64] = {0};
    /* scan PHI-scattered addresses */
    for (uint32_t addr = 0; addr < (1u<<20); addr += 128) {
        uint32_t mask = (1u<<20)-1u;
        int32_t a = (int32_t)(((uint64_t)addr * POGLS_PHI_UP)   >> 20) & (int32_t)mask;
        int32_t b = (int32_t)(((uint64_t)addr * POGLS_PHI_DOWN)  >> 20) & (int32_t)mask;
        uint32_t id = diamond_id(a, b, 0);
        hist[id]++;
    }
    uint32_t filled = 0, mn=0xFFFFFFFF, mx=0;
    for (int i=0;i<64;i++) {
        if (hist[i]>0) filled++;
        if (hist[i]<mn) mn=hist[i];
        if (hist[i]>mx) mx=hist[i];
    }
    double skew = (mn > 0) ? (double)mx/mn : 999.0;
    printf("    (filled %u/64, min=%u max=%u skew=%.2fx)\n",
           filled, mn, mx, skew);
    check(filled == 64, "all 64 diamonds reachable", "some empty");
    check(skew < 5.0, "distribution skew < 5x", "too skewed");
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 2: DiamondCell struct                                    */
/* ═══════════════════════════════════════════════════════════════ */

static void t05_cell_size(void) {
    section("T05  DiamondCell=4B, 64 cells=256B");
    check(sizeof(DiamondCell) == 4u, "DiamondCell=4B", "wrong size");
    check(sizeof(DiamondLayer) >= 256u, "DiamondLayer >= 256B", "wrong");
}

static void t06_init_clean(void) {
    section("T06  diamond_init: all cells zeroed");
    DiamondLayer dl; diamond_init(&dl);
    check(dl.magic == DIAMOND_MAGIC, "magic ok", "wrong");
    check(dl.total_updates == 0, "total_updates=0", "wrong");
    int all_zero = 1;
    for (int i=0;i<64;i++) {
        if (dl.cells[i].bias!=0 || dl.cells[i].heat!=0 || dl.cells[i].count!=0)
            all_zero = 0;
    }
    check(all_zero, "all cells zeroed", "non-zero on init");
    check(dl.cold_cells == 64u, "cold_cells=64 on init", "wrong");
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 3: diamond_update (the heart)                            */
/* ═══════════════════════════════════════════════════════════════ */

static void t07_seq_boosts_bias(void) {
    section("T07  SEQ type boosts bias positive");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry m = make_me(0x100000ULL, MESH_TYPE_SEQ, 0);

    diamond_update(&dl, 0, &m);
    check(dl.cells[0].bias > 0, "SEQ: bias > 0 after update", "not positive");
    check(dl.cells[0].count == 1, "count=1 after 1 update", "wrong");
    check(dl.seq_boosts == 1, "seq_boosts=1", "wrong");
}

static void t08_ghost_demotes_bias(void) {
    section("T08  GHOST type demotes bias negative");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry m = make_me(0x200000ULL, MESH_TYPE_GHOST, 0);

    diamond_update(&dl, 1, &m);
    check(dl.cells[1].bias < 0, "GHOST: bias < 0 after update", "not negative");
    check(dl.ghost_demotes == 1, "ghost_demotes=1", "wrong");
}

static void t09_burst_mild_positive(void) {
    section("T09  BURST type: mild positive bias");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry m = make_me(0, MESH_TYPE_BURST, 0);
    diamond_update(&dl, 2, &m);
    /* +1 then decay: result ≥ 0 */
    check(dl.cells[2].bias >= 0, "BURST: bias >= 0", "negative");
    check(dl.burst_updates == 1, "burst_updates=1", "wrong");
}

static void t10_heat_increments(void) {
    section("T10  Heat increments on update, decays");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry m = make_me(0, MESH_TYPE_SEQ, 0);

    for (int i=0; i<10; i++) diamond_update(&dl, 3, &m);
    check(dl.cells[3].heat > 0, "heat > 0 after 10 updates", "wrong");
    check(dl.cells[3].heat <= 10u, "heat stays bounded by decay", "wrong");
}

static void t11_decay_prevents_saturation(void) {
    section("T11  Decay prevents bias saturation");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry seq = make_me(0, MESH_TYPE_SEQ, 0);
    MeshEntry ghost = make_me(0, MESH_TYPE_GHOST, 0);

    /* hammer SEQ 1000 times */
    for (int i=0; i<1000; i++) diamond_update(&dl, 4, &seq);
    int8_t after_seq = dl.cells[4].bias;
    check(after_seq <= DIAMOND_BIAS_MAX, "SEQ hammer: bias <= +60", "exceeded max");
    check(after_seq > 0, "SEQ hammer: bias still positive", "went negative");

    /* hammer GHOST 1000 times */
    for (int i=0; i<1000; i++) diamond_update(&dl, 5, &ghost);
    int8_t after_ghost = dl.cells[5].bias;
    check(after_ghost >= DIAMOND_BIAS_MIN, "GHOST hammer: bias >= -60", "exceeded min");
    check(after_ghost < 0, "GHOST hammer: bias still negative", "went positive");
    printf("    (SEQ converge=%d, GHOST converge=%d)\n",
           (int)after_seq, (int)after_ghost);
}

static void t12_convergence_values(void) {
    section("T12  Convergence values (SEQ and GHOST stabilize)");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry seq   = make_me(0, MESH_TYPE_SEQ,   0);
    MeshEntry ghost = make_me(0, MESH_TYPE_GHOST, 0);

    /* run until convergence */
    int8_t prev_seq=-99, prev_ghost=99;
    for (int i=0; i<200; i++) {
        diamond_update(&dl, 10, &seq);
        diamond_update(&dl, 11, &ghost);
    }
    int8_t conv_seq   = dl.cells[10].bias;
    int8_t conv_ghost = dl.cells[11].bias;

    /* convergence: last 10 iterations should be stable */
    int8_t last_seq=conv_seq, last_ghost=conv_ghost;
    for (int i=0; i<10; i++) {
        diamond_update(&dl, 10, &seq);
        diamond_update(&dl, 11, &ghost);
    }
    check(dl.cells[10].bias == last_seq || dl.cells[10].bias == last_seq+1
          || dl.cells[10].bias == last_seq-1,
          "SEQ converges (stable ±1)", "not converged");
    check(dl.cells[11].bias == last_ghost || dl.cells[11].bias == last_ghost+1
          || dl.cells[11].bias == last_ghost-1,
          "GHOST converges (stable ±1)", "not converged");

    printf("    (SEQ converge=%d, GHOST converge=%d)\n",
           (int)dl.cells[10].bias, (int)dl.cells[11].bias);
    (void)prev_seq; (void)prev_ghost;
    (void)conv_seq; (void)conv_ghost;
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 4: feedback to V4                                        */
/* ═══════════════════════════════════════════════════════════════ */

static void t13_diamond_bias_feedback(void) {
    section("T13  diamond_bias() feedback to route_score");
    DiamondLayer dl; diamond_init(&dl);

    /* SEQ zone */
    MeshEntry seq = make_me(0, MESH_TYPE_SEQ, 0);
    for (int i=0; i<20; i++) diamond_update(&dl, 20, &seq);
    int8_t seq_bias = diamond_bias(&dl, 20);

    /* GHOST zone */
    MeshEntry ghost = make_me(0, MESH_TYPE_GHOST, 0);
    for (int i=0; i<20; i++) diamond_update(&dl, 21, &ghost);
    int8_t ghost_bias = diamond_bias(&dl, 21);

    /* simulate V4 route_score application */
    int route_seq   = 50 + (int)seq_bias;
    int route_ghost = 50 + (int)ghost_bias;

    check(seq_bias > 0,   "SEQ zone: positive bias",  "wrong");
    check(ghost_bias < 0, "GHOST zone: negative bias", "wrong");
    check(route_seq > 50, "SEQ zone: route_score boosted", "not boosted");
    check(route_ghost < 50, "GHOST zone: route_score lowered", "not lowered");
}

static void t14_should_demote_boost(void) {
    section("T14  should_demote / should_boost thresholds");
    DiamondLayer dl; diamond_init(&dl);

    /* demote: bias strictly < DEMOTE_THRESHOLD (-8) */
    dl.cells[30].bias = (int8_t)(DIAMOND_DEMOTE_THRESHOLD);      /* = -8, no demote */
    dl.cells[31].bias = (int8_t)(DIAMOND_DEMOTE_THRESHOLD - 1);  /* = -9, demote    */
    /* boost: bias strictly > BOOST_THRESHOLD (+8) */
    dl.cells[32].bias = (int8_t)(DIAMOND_BOOST_THRESHOLD);       /* = +8, no boost  */
    dl.cells[33].bias = (int8_t)(DIAMOND_BOOST_THRESHOLD + 1);   /* = +9, boost     */

    check( diamond_should_demote(&dl, 31), "bias=-9 → demote",    "wrong");
    check(!diamond_should_demote(&dl, 30), "bias=-8 → no demote", "wrong");
    check( diamond_should_boost(&dl, 33),  "bias=+9 → boost",     "wrong");
    check(!diamond_should_boost(&dl, 32),  "bias=+8 → no boost",  "wrong");
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 5: diamond_process (full pipeline)                       */
/* ═══════════════════════════════════════════════════════════════ */

static void t15_process_returns_valid_id(void) {
    section("T15  diamond_process returns valid id");
    DiamondLayer dl; diamond_init(&dl);

    for (uint32_t addr = 0; addr < (1u<<20); addr += 1024) {
        MeshEntry m = make_me((uint64_t)addr, MESH_TYPE_SEQ, 10);
        uint32_t id = diamond_process(&dl, &m);
        if (id >= DIAMOND_COUNT) {
            check(0, "all process ids in range", "out of range");
            return;
        }
    }
    check(1, "all process ids in [0..63]", "out of range");
    check(dl.total_updates > 0, "total_updates incremented", "wrong");
}

static void t16_process_coverage(void) {
    section("T16  diamond_process covers many cells");
    DiamondLayer dl; diamond_init(&dl);

    for (uint32_t addr = 0; addr < (1u<<20); addr += 256) {
        MeshEntry m = make_me((uint64_t)addr, MESH_TYPE_GHOST, (int16_t)(addr % 100));
        diamond_process(&dl, &m);
    }
    uint32_t cov = diamond_coverage(&dl);
    printf("    (coverage: %u / 64)\n", cov);
    check(cov >= 32u, "process covers >= 32 cells", "low coverage");
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 6: ReflexBias + DiamondLayer combined                    */
/* ═══════════════════════════════════════════════════════════════ */

static void t17_combined_signal(void) {
    section("T17  ReflexBias + DiamondLayer combined signal");
    ReflexBias  rb; reflex_init(&rb);
    DiamondLayer dl; diamond_init(&dl);

    uint64_t bad_addr = 0x5000ULL;

    /* flood bad zone with GHOST */
    for (int i=0; i<20; i++) {
        MeshEntry ghost = make_me(bad_addr, MESH_TYPE_GHOST, -50);
        reflex_update(&rb, &ghost);
        uint32_t id = diamond_process(&dl, &ghost);

        /* simulate V4 combined route decision */
        int8_t page_bias    = reflex_lookup(&rb, bad_addr);
        int8_t cluster_bias = diamond_bias(&dl, id);
        int combined = (int)page_bias + (int)cluster_bias;
        (void)combined;
    }

    int8_t final_page    = reflex_lookup(&rb, bad_addr);
    MeshEntry probe = make_me(bad_addr, MESH_TYPE_GHOST, -50);
    uint32_t id = diamond_process(&dl, &probe);
    int8_t final_cluster = diamond_bias(&dl, id);

    check(final_page < 0,    "page-level: negative bias",    "wrong");
    check(final_cluster < 0, "cluster-level: negative bias", "wrong");

    int combined_score = 50 + (int)final_page + (int)final_cluster;
    check(combined_score < 50, "combined signal lowers route_score", "wrong");
    printf("    (page=%d cluster=%d combined_score=%d)\n",
           (int)final_page, (int)final_cluster, combined_score);
}

static void t18_seq_zone_gets_double_boost(void) {
    section("T18  SEQ zone: page + cluster double boost");
    ReflexBias  rb; reflex_init(&rb);
    DiamondLayer dl; diamond_init(&dl);
    uint64_t good_addr = 0x9000ULL;

    for (int i=0; i<10; i++) {
        MeshEntry seq = make_me(good_addr, MESH_TYPE_SEQ, 20);
        reflex_update(&rb, &seq);
        diamond_process(&dl, &seq);
    }

    int8_t page_bias = reflex_lookup(&rb, good_addr);
    MeshEntry probe  = make_me(good_addr, MESH_TYPE_SEQ, 20);
    uint32_t  id     = diamond_process(&dl, &probe);
    int8_t    cl_bias = diamond_bias(&dl, id);

    printf("    (page=%d cluster=%d)\n", (int)page_bias, (int)cl_bias);
    check(cl_bias > 0, "cluster bias > 0 for SEQ zone", "wrong");
    /* page may be +1 (SEQ reward) or near 0 — both valid */
    check(page_bias >= 0, "page bias >= 0 for SEQ zone", "wrong");
}

/* ═══════════════════════════════════════════════════════════════ */
/* Group 7: edge cases & NULL safety                              */
/* ═══════════════════════════════════════════════════════════════ */

static void t19_null_safety(void) {
    section("T19  NULL safety");
    diamond_init(NULL);
    diamond_update(NULL, 0, NULL);
    check(diamond_bias(NULL, 0) == 0,      "bias(NULL)=0",    "crash");
    check(diamond_heat(NULL, 0) == 0,      "heat(NULL)=0",    "crash");
    check(diamond_count(NULL, 0) == 0,     "count(NULL)=0",   "crash");
    check(diamond_coverage(NULL) == 0,     "coverage(NULL)=0","crash");
    check(diamond_process(NULL, NULL) == 0,"process(NULL)=0", "crash");
    check(!diamond_should_demote(NULL, 0), "demote(NULL)=0",  "crash");
    check(!diamond_should_boost(NULL, 0),  "boost(NULL)=0",   "crash");
    check(1, "all NULL paths survived", "crash");
}

static void t20_out_of_range_id(void) {
    section("T20  Out-of-range id safety");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry m = make_me(0, MESH_TYPE_GHOST, 0);
    diamond_update(&dl, 64, &m);   /* id=64 = out of range */
    diamond_update(&dl, 255, &m);
    check(dl.total_updates == 0, "out-of-range id: no update", "updated");
    check(diamond_bias(&dl, 64) == 0,  "bias(64)=0",  "wrong");
    check(diamond_bias(&dl, 255) == 0, "bias(255)=0", "wrong");
}

static void t21_count_caps_at_uint16(void) {
    section("T21  count caps at uint16 max (65535)");
    DiamondLayer dl; diamond_init(&dl);
    dl.cells[0].count = 0xFFFEu;
    MeshEntry m = make_me(0, MESH_TYPE_SEQ, 0);
    diamond_update(&dl, 0, &m);
    check(dl.cells[0].count == 0xFFFFu, "count=65535 after increment", "wrong");
    diamond_update(&dl, 0, &m);
    check(dl.cells[0].count == 0xFFFFu, "count stays at 65535 (capped)", "overflow");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Diamond Layer — Test Suite\n");
    printf("  diamond_id + DiamondCell + update + feedback\n");
    printf("══════════════════════════════════════════════════\n");

    printf("\n=== Group 1: diamond_id ===\n");
    t01_id_range();
    t02_id_deterministic();
    t03_id_delta_matters();
    t04_id_distribution();

    printf("\n=== Group 2: DiamondCell struct ===\n");
    t05_cell_size();
    t06_init_clean();

    printf("\n=== Group 3: diamond_update (heart) ===\n");
    t07_seq_boosts_bias();
    t08_ghost_demotes_bias();
    t09_burst_mild_positive();
    t10_heat_increments();
    t11_decay_prevents_saturation();
    t12_convergence_values();

    printf("\n=== Group 4: feedback to V4 ===\n");
    t13_diamond_bias_feedback();
    t14_should_demote_boost();

    printf("\n=== Group 5: diamond_process ===\n");
    t15_process_returns_valid_id();
    t16_process_coverage();

    printf("\n=== Group 6: Reflex + Diamond combined ===\n");
    t17_combined_signal();
    t18_seq_zone_gets_double_boost();

    printf("\n=== Group 7: edge cases ===\n");
    t19_null_safety();
    t20_out_of_range_id();
    t21_count_caps_at_uint16();

    DiamondLayer dl_final; diamond_init(&dl_final);
    for (uint32_t a=0; a<(1u<<20); a+=256) {
        MeshEntry m = make_me((uint64_t)a,
            (uint8_t)((a/256)%4), (int16_t)(a%200-100));
        diamond_process(&dl_final, &m);
    }
    diamond_stats(&dl_final);

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — Diamond live ♦\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
