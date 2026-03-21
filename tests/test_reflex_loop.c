/*
 * test_reflex_loop.c — End-to-end reflex loop test (standalone)
 *
 * Simulates the complete closed loop WITHOUT needing full POGLS codebase:
 *   anomaly zone → DetachEntry → MeshEntry → ReflexBias → route demotion
 *
 * Expected: 8/8 PASS
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

/* Minimal pipeline simulator */
typedef struct {
    ReflexBias reflex;
    uint64_t route_main;
    uint64_t route_ghost;
    uint64_t route_shadow;
    uint64_t reflex_demotions;
} MiniPipeline;

static void mini_init(MiniPipeline *p) {
    memset(p, 0, sizeof(*p));
    reflex_init(&p->reflex);
}

typedef enum { ROUTE_MAIN=0, ROUTE_GHOST=1, ROUTE_SHADOW=2 } RouteTarget;

/* Simulate L3 routing (simplified): structured → MAIN, else GHOST */
static RouteTarget mini_l3(uint64_t addr) {
    /* even addr = structured = MAIN, odd = GHOST */
    return (addr % 2 == 0) ? ROUTE_MAIN : ROUTE_GHOST;
}

/* Mini pipeline process — simulates pipeline_wire_process() */
static RouteTarget mini_process(MiniPipeline *p, uint64_t value, uint64_t addr) {
    RouteTarget route = mini_l3(addr);

    /* route_final: apply reflex bias */
    if (route == ROUTE_MAIN && reflex_should_demote(&p->reflex, addr)) {
        route = ROUTE_GHOST;
        p->reflex_demotions++;
    }

    switch (route) {
    case ROUTE_MAIN:   p->route_main++;   break;
    case ROUTE_GHOST:  p->route_ghost++;  break;
    case ROUTE_SHADOW: p->route_shadow++; break;
    }
    (void)value;
    return route;
}

/* Simulated DetachEntry flush → MeshEntry → reflex update */
static void mini_flush_anomaly(MiniPipeline *p, uint64_t addr,
                                uint8_t reason, uint8_t phase18) {
    DetachEntry de;
    memset(&de, 0, sizeof(de));
    de.angular_addr = addr;
    de.value        = addr ^ 0xDEAD;
    de.reason       = reason;
    de.route_was    = (reason & DETACH_REASON_GHOST_DRIFT) ? 1u : 2u;
    de.phase18      = phase18;
    de.phase288     = 50;
    de.phase306     = 60;

    if (!is_mesh_anomaly(&de)) return;

    MeshEntry me = mesh_translate(&de);
    reflex_update(&p->reflex, &me);
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* T01: clean start — no demotion */
static void t01_clean_start(void) {
    section("T01  Clean start — no reflex demotion");
    MiniPipeline p; mini_init(&p);

    for (int i = 0; i < 100; i++)
        mini_process(&p, i, (uint64_t)i * 2);  /* all even = MAIN candidates */

    check(p.reflex_demotions == 0, "no demotions on fresh pipeline", "wrong");
    check(p.route_main == 100, "all structured → MAIN", "wrong");
}

/* T02: anomaly zone gets demoted */
static void t02_anomaly_demotes(void) {
    section("T02  Anomaly zone → reflex builds → demotion");
    MiniPipeline p; mini_init(&p);
    uint64_t bad_addr = 0x4000ULL;

    /* hammer anomalies from bad zone */
    for (int i = 0; i < 10; i++)
        mini_flush_anomaly(&p, bad_addr, DETACH_REASON_GEO_INVALID, 1);

    check(reflex_should_demote(&p.reflex, bad_addr),
          "bad zone flagged for demotion", "not flagged");

    /* now process ops at bad addr — structured (even) but should be demoted */
    RouteTarget r = mini_process(&p, 0, bad_addr);
    check(r == ROUTE_GHOST, "bad zone demoted MAIN → GHOST", "not demoted");
    check(p.reflex_demotions >= 1, "demotion counter incremented", "wrong");
}

/* T03: good zone unaffected */
static void t03_good_zone_safe(void) {
    section("T03  Good zone unaffected by bad zone");
    MiniPipeline p; mini_init(&p);
    uint64_t bad   = 0x3000ULL;
    uint64_t good  = 0x7000ULL;

    for (int i = 0; i < 10; i++)
        mini_flush_anomaly(&p, bad, DETACH_REASON_GEO_INVALID, 1);

    check( reflex_should_demote(&p.reflex, bad),  "bad zone demoted",   "wrong");
    check(!reflex_should_demote(&p.reflex, good), "good zone safe",     "leaked");

    RouteTarget r = mini_process(&p, 0, good);  /* even = MAIN candidate */
    check(r == ROUTE_MAIN, "good zone stays MAIN", "false demotion");
}

/* T04: SEQ drift gets mild penalty (does not demote on single event) */
static void t04_seq_mild(void) {
    section("T04  SEQ drift = mild penalty only");
    MiniPipeline p; mini_init(&p);
    uint64_t addr = 0x5000ULL;

    mini_flush_anomaly(&p, addr, DETACH_REASON_GHOST_DRIFT, 10);
    int8_t bias = reflex_lookup(&p.reflex, addr);
    check(bias == -1, "single SEQ = -1 bias", "wrong");
    check(!reflex_should_demote(&p.reflex, addr),
          "single SEQ does not trigger demotion", "over-triggered");
}

/* T05: full loop — anomaly → feedback → routing change */
static void t05_full_loop(void) {
    section("T05  Full loop: observe → think (reflex) → influence");
    MiniPipeline p; mini_init(&p);
    uint64_t addr = 0x6000ULL;  /* even = MAIN candidate by L3 */

    /* Phase 1: before anomaly history */
    RouteTarget r1 = mini_process(&p, 0, addr);
    check(r1 == ROUTE_MAIN, "phase 1: addr → MAIN (no history)", "wrong");

    /* Phase 2: anomalies observed */
    for (int i = 0; i < 10; i++)
        mini_flush_anomaly(&p, addr, DETACH_REASON_GEO_INVALID, 1);

    /* Phase 3: same addr now gets demoted */
    RouteTarget r3 = mini_process(&p, 0, addr);
    check(r3 == ROUTE_GHOST, "phase 3: addr → GHOST (history learned)", "not demoted");

    /* Phase 4: decay recovery */
    for (int cycle = 0; cycle < 80; cycle++) {
        for (uint32_t b = 0; b < REFLEX_BUCKETS; b++) {
            int8_t bv = p.reflex.bias[b];
            p.reflex.bias[b] = (int8_t)((int)bv - ((int)bv >> REFLEX_DECAY_SHIFT));
        }
        p.reflex.decays++;
    }
    RouteTarget r4 = mini_process(&p, 0, addr);
    check(r4 == ROUTE_MAIN, "phase 4: addr → MAIN after decay (zone recovered)", "stuck");
}

/* T06: multiple zones independent */
static void t06_multi_zone(void) {
    section("T06  Multi-zone independence");
    MiniPipeline p; mini_init(&p);

    /* poison 3 different zones */
    uint64_t zones[3] = {0x1000ULL, 0x2000ULL, 0x3000ULL};
    for (int z = 0; z < 3; z++)
        for (int i = 0; i < 10; i++)
            mini_flush_anomaly(&p, zones[z], DETACH_REASON_GEO_INVALID, 1);

    /* clean zones unaffected */
    uint64_t clean[3] = {0x9000ULL, 0xA000ULL, 0xB000ULL};
    int all_clean = 1;
    for (int z = 0; z < 3; z++)
        if (reflex_should_demote(&p.reflex, clean[z])) all_clean = 0;

    check(all_clean, "3 clean zones unaffected", "leaked");

    /* all bad zones demoted */
    int all_bad = 1;
    for (int z = 0; z < 3; z++)
        if (!reflex_should_demote(&p.reflex, zones[z])) all_bad = 0;
    check(all_bad, "3 bad zones all demoted", "not all demoted");
}

/* T07: mesh_entry_push → reflex_update integration */
static void t07_buf_reflex_integration(void) {
    section("T07  MeshEntryBuf + ReflexBias integration");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);
    ReflexBias r;    reflex_init(&r);
    uint64_t addr = 0xD000ULL;

    /* push entries to buf, update reflex for each */
    for (int i = 0; i < 8; i++) {
        MeshEntry e = {addr, (uint64_t)i, 0, MESH_TYPE_BURST, (uint8_t)i, -5};
        mesh_entry_push(&buf, &e);
        reflex_update(&r, &e);
    }

    check(buf.pushed == 8, "buf: 8 pushed", "wrong");
    check(reflex_should_demote(&r, addr), "reflex demotes after 8 BURSTs", "wrong");
    check(r.reinforcements == 8, "8 reinforcements", "wrong");
}

/* T08: translate + classify roundtrip */
static void t08_translate_roundtrip(void) {
    section("T08  Translate + classify roundtrip");
    /* geo_invalid + early phase → BURST */
    DetachEntry de; memset(&de, 0, sizeof(de));
    de.angular_addr = 0xE000ULL;
    de.value        = 0xCAFE;
    de.reason       = DETACH_REASON_GEO_INVALID;
    de.route_was    = 2;
    de.phase18      = 2;   /* < 3 → BURST */
    de.phase288     = 100;
    de.phase306     = 120;

    MeshEntry me = mesh_translate(&de);
    check(me.addr  == 0xE000ULL,       "addr round-trip",   "wrong");
    check(me.value == 0xCAFE,          "value round-trip",  "wrong");
    check(me.type  == MESH_TYPE_BURST, "type=BURST",        "wrong");
    check(me.delta == -20,             "delta=100-120=-20", "wrong");
    check(me.phase18 == 2,             "phase18=2",         "wrong");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Reflex Loop — End-to-End Test Suite\n");
    printf("  (standalone: no full POGLS codebase needed)\n");
    printf("══════════════════════════════════════════════════\n");
    t01_clean_start();
    t02_anomaly_demotes();
    t03_good_zone_safe();
    t04_seq_mild();
    t05_full_loop();
    t06_multi_zone();
    t07_buf_reflex_integration();
    t08_translate_roundtrip();
    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — Loop closed 🔁\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
