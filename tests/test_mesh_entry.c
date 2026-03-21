/*
 * test_mesh_entry.c — Tests for pogls_mesh_entry.h
 * MeshEntry struct + translate + MeshEntryBuf
 * Expected: 9/9 PASS
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

static DetachEntry make_detach(uint64_t addr, uint64_t value,
                                uint8_t reason, uint8_t route_was,
                                uint8_t phase18, uint16_t p288, uint16_t p306)
{
    DetachEntry e; memset(&e, 0, sizeof(e));
    e.angular_addr = addr;
    e.value        = value;
    e.reason       = reason;
    e.route_was    = route_was;
    e.phase18      = phase18;
    e.phase288     = p288;
    e.phase306     = p306;
    return e;
}

/* T01: struct sizes */
static void t01_size(void) {
    section("T01  Struct sizes");
    check(sizeof(MeshEntry) == 24u, "MeshEntry=24B", "wrong size");
    check(sizeof(MeshEntryBuf) > 0, "MeshEntryBuf exists", "missing");
}

/* T02: translate preserves addr/value/phase */
static void t02_translate_basic(void) {
    section("T02  mesh_translate preserves fields");
    DetachEntry de = make_detach(0xABCD1234ULL, 0xDEADBEEFULL,
                                  DETACH_REASON_GEO_INVALID, 2, 5, 100, 120);
    MeshEntry me = mesh_translate(&de);
    check(me.addr    == 0xABCD1234ULL, "addr preserved",    "addr wrong");
    check(me.value   == 0xDEADBEEFULL, "value preserved",   "value wrong");
    check(me.phase18 == 5u,            "phase18 preserved", "phase18 wrong");
    check(me.sig     != 0,             "sig non-zero",      "sig=0");
}

/* T03: type classification — priority SEQ > BURST > GHOST > ANOMALY */
static void t03_type_classify(void) {
    section("T03  Type classification (priority order)");

    /* SEQ: ghost drift — highest priority */
    DetachEntry seq = make_detach(0, 0, DETACH_REASON_GHOST_DRIFT, 1, 10, 50, 50);
    check(mesh_classify_type(&seq) == MESH_TYPE_SEQ,
          "SEQ: ghost_drift = SEQ", "wrong type");

    /* BURST: geo_invalid + early phase */
    DetachEntry burst = make_detach(0, 0, DETACH_REASON_GEO_INVALID, 2, 1, 0, 0);
    check(mesh_classify_type(&burst) == MESH_TYPE_BURST,
          "BURST: geo_invalid + phase18<3", "wrong type");

    /* GHOST: route_was=1, no geo_invalid */
    DetachEntry ghost = make_detach(0, 0, 0, 1, 10, 50, 50);
    check(mesh_classify_type(&ghost) == MESH_TYPE_GHOST,
          "GHOST: route_was=1, no geo", "wrong type");

    /* ANOMALY: geo_invalid + late phase */
    DetachEntry anom = make_detach(0, 0, DETACH_REASON_GEO_INVALID, 2, 10, 50, 50);
    check(mesh_classify_type(&anom) == MESH_TYPE_ANOMALY,
          "ANOMALY: geo_invalid + late phase", "wrong type");

    /* NULL safety */
    check(mesh_classify_type(NULL) == MESH_TYPE_ANOMALY,
          "classify(NULL) = ANOMALY", "crash");
}

/* T04: delta (world crossing signal) */
static void t04_delta(void) {
    section("T04  Delta (world crossing signal)");

    /* balanced */
    DetachEntry bal = make_detach(0, 0, 0, 0, 0, 100, 100);
    check(mesh_translate(&bal).delta == 0, "balanced delta=0", "wrong");

    /* World A lean */
    DetachEntry a_lean = make_detach(0, 0, 0, 0, 0, 150, 100);
    check(mesh_translate(&a_lean).delta > 0, "World A lean > 0", "wrong");

    /* World B lean */
    DetachEntry b_lean = make_detach(0, 0, 0, 0, 0, 100, 150);
    check(mesh_translate(&b_lean).delta < 0, "World B lean < 0", "wrong");
}

/* T05: is_mesh_anomaly guard */
static void t05_anomaly_guard(void) {
    section("T05  is_mesh_anomaly guard");
    DetachEntry geo  = make_detach(0,0,DETACH_REASON_GEO_INVALID,0,0,0,0);
    DetachEntry drft = make_detach(0,0,DETACH_REASON_GHOST_DRIFT,0,0,0,0);
    DetachEntry circ = make_detach(0,0,DETACH_REASON_UNIT_CIRCLE,0,0,0,0);
    DetachEntry none = make_detach(0,0,0,0,0,0,0);
    check(is_mesh_anomaly(&geo),  "GEO_INVALID = anomaly",  "wrong");
    check(is_mesh_anomaly(&drft), "GHOST_DRIFT = anomaly",  "wrong");
    check(is_mesh_anomaly(&circ), "UNIT_CIRCLE = anomaly",  "wrong");
    check(!is_mesh_anomaly(&none),"no reason = not anomaly","wrong");
    check(!is_mesh_anomaly(NULL), "NULL = not anomaly",     "crash");
}

/* T06: ring buffer push/pop */
static void t06_buf_push_pop(void) {
    section("T06  MeshEntryBuf push/pop");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);
    check(buf.magic == MESH_ENTRY_MAGIC, "magic ok", "wrong");

    MeshEntry e = {0xABCD, 0x1234, 0x5678, MESH_TYPE_GHOST, 5, 10};
    check(mesh_entry_push(&buf, &e) == 1, "push ok", "failed");
    check(mesh_entry_pending(&buf) == 1, "pending=1", "wrong");

    MeshEntry out; memset(&out, 0, sizeof(out));
    check(mesh_entry_pop(&buf, &out) == 1, "pop ok", "failed");
    check(out.addr  == 0xABCD,          "addr ok",  "wrong");
    check(out.type  == MESH_TYPE_GHOST, "type ok",  "wrong");
    check(mesh_entry_pending(&buf) == 0, "empty after pop", "wrong");
}

/* T07: overflow drop-oldest */
static void t07_overflow(void) {
    section("T07  Overflow drop-oldest policy");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);
    MeshEntry e = {0};
    for (uint32_t i = 0; i < MESH_ENTRY_BUF_SIZE; i++) {
        e.value = i;
        mesh_entry_push(&buf, &e);
    }
    check(buf.overflows == 0, "no overflow filling exactly", "wrong");
    e.value = 0xFFFF;
    mesh_entry_push(&buf, &e);
    check(buf.overflows == 1, "overflow=1 on overfill", "wrong");
}

/* T08: batch drain */
static void t08_drain(void) {
    section("T08  Batch drain");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);
    MeshEntry e = {0};
    for (int i = 0; i < 100; i++) { e.value = i; mesh_entry_push(&buf, &e); }
    MeshEntry out[128];
    uint32_t n = mesh_entry_drain(&buf, out, 128);
    check(n == 100,              "drained 100",          "count wrong");
    check(out[0].value == 0,    "first entry value=0",  "wrong");
    check(out[99].value == 99,  "last entry value=99",  "wrong");
    check(mesh_entry_pending(&buf) == 0, "empty after drain", "wrong");
}

/* T09: sig uniqueness */
static void t09_sig_unique(void) {
    section("T09  Sig fingerprint uniqueness");
    DetachEntry d1 = make_detach(100, 200, 0, 0, 5, 0, 0);
    DetachEntry d2 = make_detach(100, 201, 0, 0, 5, 0, 0);
    DetachEntry d3 = make_detach(101, 200, 0, 0, 5, 0, 0);
    MeshEntry m1 = mesh_translate(&d1);
    MeshEntry m2 = mesh_translate(&d2);
    MeshEntry m3 = mesh_translate(&d3);
    check(m1.sig != m2.sig, "diff value → diff sig", "collision");
    check(m1.sig != m3.sig, "diff addr → diff sig",  "collision");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS MeshEntry Translation Layer — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");
    t01_size(); t02_translate_basic(); t03_type_classify();
    t04_delta(); t05_anomaly_guard();  t06_buf_push_pop();
    t07_overflow(); t08_drain(); t09_sig_unique();
    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — MeshEntry live [S]\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
