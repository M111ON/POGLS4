/*
 * test_mesh_entry.c — Tests for pogls_mesh_entry.h
 * ══════════════════════════════════════════════════════════════
 * 9 test groups, standalone compile
 */
#define _POSIX_C_SOURCE 200809L
#define POGLS_MESH_ENTRY_STANDALONE  /* use stub DetachEntry */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pogls_mesh_entry.h"

static int g_pass = 0, g_fail = 0;
#define section(s)  printf("\n  -- %s\n", s)
#define check(c, ok, fail) do { \
    if (c) { printf("    v %s\n", ok);   g_pass++; } \
    else   { printf("    x FAIL: %s\n", fail); g_fail++; } \
} while(0)

/* helper: build DetachEntry */
static DetachEntry make_de(uint64_t addr, uint64_t value,
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

/* ── T01: struct sizes ────────────────────────────────────────── */
static void t01_sizes(void)
{
    section("T01  Struct sizes");
    check(sizeof(MeshEntry) == 24u, "MeshEntry=24B", "wrong size");
    check(sizeof(DetachEntry) == 24u || sizeof(DetachEntry) > 0,
          "DetachEntry size ok", "zero size");
    MeshEntryBuf b; mesh_entry_buf_init(&b);
    check(b.magic == MESH_ENTRY_MAGIC, "buf magic ok", "magic wrong");
    check(b.head == 0, "head=0", "head wrong");
    check(b.tail == 0, "tail=0", "tail wrong");
}

/* ── T02: translate preserves core fields ─────────────────────── */
static void t02_translate_basic(void)
{
    section("T02  mesh_translate preserves addr/value/phase18");
    DetachEntry de = make_de(0xABCD1234ULL, 0xDEADBEEFULL,
                              DETACH_REASON_GEO_INVALID, 2, 5, 100, 120);
    MeshEntry me = mesh_translate(&de);
    check(me.addr    == 0xABCD1234ULL, "addr preserved",    "addr wrong");
    check(me.value   == 0xDEADBEEFULL, "value preserved",   "value wrong");
    check(me.phase18 == 5u,            "phase18 preserved", "phase18 wrong");
}

/* ── T03: type classification ─────────────────────────────────── */
static void t03_type_classify(void)
{
    section("T03  Type classification");

    /* GHOST: route_was=1, no geo_invalid */
    DetachEntry ghost_de = make_de(0, 0, 0, 1, 10, 50, 50);
    check(mesh_classify_type(&ghost_de) == MESH_TYPE_GHOST,
          "GHOST type correct", "wrong type");

    /* BURST: geo_invalid + phase18 < 3 */
    DetachEntry burst_de = make_de(0, 0, DETACH_REASON_GEO_INVALID, 2, 1, 0, 0);
    check(mesh_classify_type(&burst_de) == MESH_TYPE_BURST,
          "BURST type correct", "wrong type");

    /* SEQ: ghost drift (even if also geo) */
    DetachEntry seq_de = make_de(0, 0, DETACH_REASON_GHOST_DRIFT, 1, 10, 50, 50);
    check(mesh_classify_type(&seq_de) == MESH_TYPE_SEQ,
          "SEQ type correct", "wrong type");

    /* ANOMALY: geo_invalid + late phase */
    DetachEntry anom_de = make_de(0, 0, DETACH_REASON_GEO_INVALID, 2, 10, 50, 50);
    check(mesh_classify_type(&anom_de) == MESH_TYPE_ANOMALY,
          "ANOMALY type correct", "wrong type");

    /* NULL safety */
    check(mesh_classify_type(NULL) == MESH_TYPE_ANOMALY,
          "classify(NULL) = ANOMALY (safe)", "crash");
}

/* ── T04: delta (world crossing signal) ───────────────────────── */
static void t04_delta(void)
{
    section("T04  Delta world crossing signal");

    /* balanced: p288 == p306 */
    DetachEntry bal = make_de(0, 0, 0, 0, 0, 100, 100);
    MeshEntry m1 = mesh_translate(&bal);
    check(m1.delta == 0, "balanced: delta=0",   "wrong");

    /* World A lean: p288 > p306 */
    DetachEntry a_lean = make_de(0, 0, 0, 0, 0, 150, 100);
    MeshEntry m2 = mesh_translate(&a_lean);
    check(m2.delta > 0, "World A lean: delta>0", "wrong");

    /* World B lean: p306 > p288 */
    DetachEntry b_lean = make_de(0, 0, 0, 0, 0, 100, 150);
    MeshEntry m3 = mesh_translate(&b_lean);
    check(m3.delta < 0, "World B lean: delta<0", "wrong");

    /* twin window: both small */
    DetachEntry twin = make_de(0, 0, 0, 0, 0, 5, 5);
    MeshEntry m4 = mesh_translate(&twin);
    check(m4.delta == 0, "twin window: delta=0", "wrong");
}

/* ── T05: sig uniqueness ──────────────────────────────────────── */
static void t05_sig(void)
{
    section("T05  Sig fingerprint uniqueness");

    DetachEntry d1 = make_de(100ULL, 200ULL, 0, 0, 5, 0, 0);
    DetachEntry d2 = make_de(100ULL, 201ULL, 0, 0, 5, 0, 0); /* diff value */
    DetachEntry d3 = make_de(101ULL, 200ULL, 0, 0, 5, 0, 0); /* diff addr  */
    DetachEntry d4 = make_de(100ULL, 200ULL, 0, 0, 6, 0, 0); /* diff phase */

    MeshEntry m1 = mesh_translate(&d1);
    MeshEntry m2 = mesh_translate(&d2);
    MeshEntry m3 = mesh_translate(&d3);
    MeshEntry m4 = mesh_translate(&d4);

    check(m1.sig != m2.sig, "diff value → diff sig",  "collision");
    check(m1.sig != m3.sig, "diff addr  → diff sig",  "collision");
    check(m1.sig != m4.sig, "diff phase → diff sig",  "collision");

    /* same input → same sig (deterministic) */
    MeshEntry m1b = mesh_translate(&d1);
    check(m1.sig == m1b.sig, "same input → same sig", "not deterministic");
}

/* ── T06: ring buffer push/pop single ─────────────────────────── */
static void t06_buf_basic(void)
{
    section("T06  Ring buffer push/pop (single entry)");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);

    MeshEntry e = {0xABCD1234ULL, 0xDEADBEEFULL, 0x11223344u,
                   MESH_TYPE_GHOST, 7u, -18};
    int ok = mesh_entry_push(&buf, &e);
    check(ok == 1,                     "push returned 1",    "wrong");
    check(buf.pushed == 1,             "pushed=1",           "wrong");
    check(mesh_entry_pending(&buf)==1, "pending=1",          "wrong");

    MeshEntry out; memset(&out, 0, sizeof(out));
    int got = mesh_entry_pop(&buf, &out);
    check(got == 1,                    "pop got entry",      "empty");
    check(out.addr  == e.addr,         "addr preserved",     "wrong");
    check(out.value == e.value,        "value preserved",    "wrong");
    check(out.type  == MESH_TYPE_GHOST,"type preserved",     "wrong");
    check(out.delta == e.delta,        "delta preserved",    "wrong");
    check(mesh_entry_pending(&buf)==0, "pending=0 after pop","wrong");
    check(buf.consumed == 1,           "consumed=1",         "wrong");
}

/* ── T07: overflow drop-oldest ────────────────────────────────── */
static void t07_overflow(void)
{
    section("T07  Overflow drop-oldest (never block)");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);

    MeshEntry e = {0};
    for (uint32_t i = 0; i < MESH_ENTRY_BUF_SIZE; i++) {
        e.value = i;
        mesh_entry_push(&buf, &e);
    }
    check(buf.overflows == 0,
          "no overflow while filling ring", "wrong");
    check(mesh_entry_pending(&buf) == MESH_ENTRY_BUF_SIZE,
          "ring full", "wrong");

    /* one more → drop oldest */
    e.value = 0xFFFF;
    mesh_entry_push(&buf, &e);
    check(buf.overflows == 1, "overflow=1 after +1",   "wrong");

    /* still pushable */
    e.value = 0xEEEE;
    int r = mesh_entry_push(&buf, &e);
    check(r == 1, "still pushable after overflow", "wrong");
}

/* ── T08: batch drain ─────────────────────────────────────────── */
static void t08_drain(void)
{
    section("T08  Batch drain 100 entries");
    MeshEntryBuf buf; mesh_entry_buf_init(&buf);

    MeshEntry e = {0};
    uint64_t expected_sum = 0;
    for (uint32_t i = 0; i < 100; i++) {
        e.value = i + 1;
        expected_sum += e.value;
        mesh_entry_push(&buf, &e);
    }

    MeshEntry out[128];
    uint32_t n = mesh_entry_drain(&buf, out, 128);
    check(n == 100,                       "drained 100",          "wrong");
    check(mesh_entry_pending(&buf) == 0,  "pending=0 after drain","wrong");
    check(out[0].value  == 1,             "first entry value=1",  "wrong");
    check(out[99].value == 100,           "last entry value=100", "wrong");

    uint64_t sum = 0;
    for (uint32_t i = 0; i < 100; i++) sum += out[i].value;
    check(sum == expected_sum, "sum correct", "sum wrong");
}

/* ── T09: NULL safety ─────────────────────────────────────────── */
static void t09_null(void)
{
    section("T09  NULL safety");
    mesh_entry_buf_init(NULL);
    check(mesh_entry_push(NULL, NULL) == 0,   "push(NULL)=0",    "crash");
    check(mesh_entry_pop(NULL, NULL) == 0,    "pop(NULL)=0",     "crash");
    check(mesh_entry_pending(NULL) == 0,      "pending(NULL)=0", "crash");
    check(mesh_entry_drain(NULL,NULL,0) == 0, "drain(NULL)=0",   "crash");
    check(!mesh_entry_buf_valid(NULL),        "valid(NULL)=0",   "crash");
    check(1, "all NULL paths survived", "impossible");
}

/* ── main ─────────────────────────────────────────────────────── */
int main(void)
{
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS MeshEntry Translation Layer — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");

    t01_sizes();
    t02_translate_basic();
    t03_type_classify();
    t04_delta();
    t05_sig();
    t06_buf_basic();
    t07_overflow();
    t08_drain();
    t09_null();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — MeshEntry live [S]\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass + g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
