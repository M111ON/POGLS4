/*
 * test_mesh_wire.c — Tests for ReflexBias (in pogls_mesh_entry.h)
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

static void force_decay(ReflexBias *r, int cycles) {
    for (int i = 0; i < cycles; i++) {
        for (uint32_t b = 0; b < REFLEX_BUCKETS; b++) {
            int8_t bv = r->bias[b];
            r->bias[b] = (int8_t)((int)bv - ((int)bv >> REFLEX_DECAY_SHIFT));
        }
        r->decays++;
    }
}

/* T01: init */
static void t01_init(void) {
    section("T01  ReflexBias init");
    ReflexBias r; reflex_init(&r);
    check(r.bias[0] == 0,          "bias[0]=0 on init",   "wrong");
    check(r.push_count == 0,       "push_count=0",         "wrong");
    check(reflex_lookup(&r, 0) == 0, "lookup(0)=0",        "wrong");
    check(reflex_lookup(NULL, 0) == 0, "lookup(NULL)=0",   "crash");
    check(!reflex_should_demote(&r, 0), "no demote on init","wrong");
}

/* T02: BURST gives largest penalty */
static void t02_penalty_order(void) {
    section("T02  Penalty order: BURST > ANOMALY > GHOST/SEQ");
    uint64_t addr = 0x1000ULL;

    ReflexBias r1; reflex_init(&r1);
    MeshEntry ghost = {addr,0,0,MESH_TYPE_GHOST,5,0};
    reflex_update(&r1, &ghost);
    int8_t ghost_bias = reflex_lookup(&r1, addr);

    ReflexBias r2; reflex_init(&r2);
    MeshEntry burst = {addr,0,0,MESH_TYPE_BURST,5,0};
    reflex_update(&r2, &burst);
    int8_t burst_bias = reflex_lookup(&r2, addr);

    check(ghost_bias < 0,        "ghost gives negative bias",        "wrong");
    check(burst_bias < 0,        "burst gives negative bias",        "wrong");
    check(burst_bias < ghost_bias, "burst penalty > ghost penalty",  "wrong");
}

/* T03: threshold guards weak signal */
static void t03_threshold(void) {
    section("T03  Threshold: single event does NOT demote");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x2000ULL;

    MeshEntry e = {addr,0,0,MESH_TYPE_GHOST,5,0};
    reflex_update(&r, &e);  /* single ghost = -1 bias */

    check(!reflex_should_demote(&r, addr),
          "single ghost (-1) does not trigger demotion (threshold=-4)",
          "over-triggered");
}

/* T04: repeated events build demotion signal */
static void t04_demotion_builds(void) {
    section("T04  Repeated BURST builds demotion signal");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x3000ULL;

    MeshEntry e = {addr,0,0,MESH_TYPE_BURST,3,-5};
    for (int i = 0; i < 10; i++) reflex_update(&r, &e);

    check(reflex_should_demote(&r, addr),
          "10x BURST → demotion triggered (bias <= -4)",
          "demotion not triggered");
    check(r.reinforcements == 10, "reinforcements=10", "wrong");
}

/* T05: decay recovers zone */
static void t05_decay_recovery(void) {
    section("T05  Decay recovers zone over time");
    ReflexBias r; reflex_init(&r);
    uint64_t addr = 0x4000ULL;

    MeshEntry e = {addr,0,0,MESH_TYPE_BURST,0,0};
    for (int i = 0; i < 20; i++) reflex_update(&r, &e);
    int8_t saturated = reflex_lookup(&r, addr);
    check(saturated < REFLEX_DEMOTE_THRESHOLD, "saturated below threshold", "wrong");

    force_decay(&r, 60);  /* many decay cycles */
    int8_t recovered = reflex_lookup(&r, addr);
    check(recovered > saturated, "bias recovered after decay", "stuck");
    check(r.decays == 60, "decay cycles counted", "wrong");
}

/* T06: bucket isolation */
static void t06_isolation(void) {
    section("T06  Bucket isolation");
    ReflexBias r; reflex_init(&r);
    uint64_t addr_a = 0x1000ULL;  /* bucket 1 */
    uint64_t addr_b = 0x8000ULL;  /* bucket 8 */

    MeshEntry ea = {addr_a,0,0,MESH_TYPE_BURST,0,0};
    for (int i = 0; i < 15; i++) reflex_update(&r, &ea);

    check(reflex_should_demote(&r, addr_a), "zone A demoted", "not demoted");
    check(!reflex_should_demote(&r, addr_b), "zone B clean",  "leaked");
    check(reflex_lookup(&r, addr_b) == 0, "zone B bias=0",   "non-zero");
}

/* T07: V4 route_score simulation */
static void t07_route_score(void) {
    section("T07  V4 route_score simulation");
    ReflexBias r; reflex_init(&r);
    uint64_t bad  = 0x5000ULL;
    uint64_t good = 0x9000ULL;

    MeshEntry e = {bad,0,0,MESH_TYPE_BURST,0,0};
    for (int i = 0; i < 10; i++) reflex_update(&r, &e);

    /* V4 logic: if reflex_should_demote → GHOST */
    int route_bad  = reflex_should_demote(&r, bad)  ? 1 : 0;  /* 1=GHOST */
    int route_good = reflex_should_demote(&r, good) ? 1 : 0;

    check(route_bad  == 1, "bad zone → demoted to GHOST", "not demoted");
    check(route_good == 0, "good zone → stays MAIN",      "false demotion");
}

/* T08: NULL safety */
static void t08_null(void) {
    section("T08  NULL safety");
    reflex_init(NULL);
    reflex_update(NULL, NULL);
    check(reflex_lookup(NULL, 0) == 0,    "lookup(NULL)=0",        "crash");
    check(!reflex_should_demote(NULL, 0), "demote(NULL)=false",    "crash");
    check(1, "all NULL paths survived", "crash");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS ReflexBias — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");
    t01_init(); t02_penalty_order(); t03_threshold();
    t04_demotion_builds(); t05_decay_recovery();
    t06_isolation(); t07_route_score(); t08_null();
    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — ReflexBias live [S]\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
