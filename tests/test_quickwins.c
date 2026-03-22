/*
 * test_quickwins.c — Tests for DHC callbacks + SliceTag
 * Expected: ALL PASS
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_detach_callbacks.h"
#include "../pogls_slice_tag.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n", s)
#define check(c,ok,fail) do{ \
    if(c){printf("    v %s\n",ok);g_pass++;} \
    else {printf("    x FAIL: %s (line %d)\n",fail,__LINE__);g_fail++;} \
}while(0)

/* ── shared test state ───────────────────────────────────────────── */
static uint64_t g_mesh_calls = 0;
static uint64_t g_dhc_calls  = 0;
static uint8_t  g_last_mesh_type = 0xFF;
static uint64_t g_last_dhc_addr  = 0;
static uint64_t g_last_dhc_reason = 0;

static void test_mesh_cb(const MeshEntry *e, void *ctx) {
    (void)ctx;
    g_mesh_calls++;
    if (e) g_last_mesh_type = e->type;
}

static void test_dhc_cb(const DetachEntry *e, void *ctx) {
    (void)ctx;
    g_dhc_calls++;
    if (e) { g_last_dhc_addr = e->angular_addr; g_last_dhc_reason = e->reason; }
}

static void reset_counters(void) {
    g_mesh_calls = 0; g_dhc_calls  = 0;
    g_last_mesh_type = 0xFF; g_last_dhc_addr = 0;
}

/* ── make DetachEntry ────────────────────────────────────────────── */
static DetachEntry make_de(uint64_t addr, uint8_t reason, uint8_t phase18) {
    DetachEntry e; memset(&e,0,sizeof(e));
    e.angular_addr=addr; e.reason=reason; e.phase18=phase18;
    e.phase288=50; e.phase306=60;
    return e;
}

/* ══════════════════════════════════════════════════════════════════
 * Group 1: DetachCallbacks init & wiring
 * ══════════════════════════════════════════════════════════════════ */

static void t01_init(void) {
    section("T01  DetachCallbacks init");
    DetachCallbacks cb; detach_callbacks_init(&cb);
    check(cb.mesh_cb  == NULL,  "mesh_cb=NULL",   "wrong");
    check(cb.dhc_cb   == NULL,  "dhc_cb=NULL",    "wrong");
    check(cb.mesh_ctx == NULL,  "mesh_ctx=NULL",  "wrong");
    check(cb.mesh_fired == 0,   "mesh_fired=0",   "wrong");
    check(cb.dhc_fired  == 0,   "dhc_fired=0",    "wrong");
}

static void t02_set_callbacks(void) {
    section("T02  set mesh_cb and dhc_cb");
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_mesh_cb(&cb, test_mesh_cb, (void*)0x1);
    detach_set_dhc_cb (&cb, test_dhc_cb,  (void*)0x2);
    check(cb.mesh_cb  == test_mesh_cb, "mesh_cb set",  "wrong");
    check(cb.dhc_cb   == test_dhc_cb,  "dhc_cb set",   "wrong");
    check(cb.mesh_ctx == (void*)0x1,   "mesh_ctx set", "wrong");
    check(cb.dhc_ctx  == (void*)0x2,   "dhc_ctx set",  "wrong");
}

/* ══════════════════════════════════════════════════════════════════
 * Group 2: _detach_fire_callbacks
 * ══════════════════════════════════════════════════════════════════ */

static void t03_both_callbacks_fire(void) {
    section("T03  Both callbacks fire for anomaly entry");
    reset_counters();
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_mesh_cb(&cb, test_mesh_cb, NULL);
    detach_set_dhc_cb (&cb, test_dhc_cb,  NULL);

    DetachEntry ring[8]; memset(ring,0,sizeof(ring));
    ring[0] = make_de(0x1000ULL, DETACH_REASON_GEO_INVALID, 5);

    _detach_fire_callbacks(&cb, ring, 0, 1);

    check(g_mesh_calls == 1, "mesh_cb fired once",  "wrong");
    check(g_dhc_calls  == 1, "dhc_cb fired once",   "wrong");
    check(cb.mesh_fired == 1,"mesh_fired=1",         "wrong");
    check(cb.dhc_fired  == 1,"dhc_fired=1",          "wrong");
}

static void t04_mesh_anomaly_guard(void) {
    section("T04  mesh_cb: anomaly guard filters non-anomaly entries");
    reset_counters();
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_mesh_cb(&cb, test_mesh_cb, NULL);
    /* dhc_cb intentionally NULL */

    DetachEntry ring[8]; memset(ring,0,sizeof(ring));
    /* entry 0: has reason → anomaly → fires */
    ring[0] = make_de(0x1000ULL, DETACH_REASON_GHOST_DRIFT, 3);
    /* entry 1: no reason → not anomaly → filtered */
    ring[1] = make_de(0x2000ULL, 0, 5);

    _detach_fire_callbacks(&cb, ring, 0, 2);

    check(g_mesh_calls == 1,     "mesh fired 1 (not 2)",      "wrong");
    check(cb.mesh_skipped == 1,  "mesh_skipped=1",             "wrong");
    check(g_dhc_calls == 0,      "dhc not fired (NULL)",       "wrong");
}

static void t05_dhc_no_guard(void) {
    section("T05  dhc_cb: no anomaly guard — fires for ALL entries");
    reset_counters();
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_dhc_cb(&cb, test_dhc_cb, NULL);

    DetachEntry ring[8]; memset(ring,0,sizeof(ring));
    ring[0] = make_de(0x1000ULL, DETACH_REASON_GEO_INVALID, 3);
    ring[1] = make_de(0x2000ULL, 0, 7);  /* no reason — DHC still gets it */
    ring[2] = make_de(0x3000ULL, DETACH_REASON_GHOST_DRIFT, 1);

    _detach_fire_callbacks(&cb, ring, 0, 3);

    check(g_dhc_calls == 3, "dhc fired 3 (all entries)", "wrong");
    check(g_mesh_calls == 0, "mesh not fired (NULL)",     "wrong");
}

static void t06_callback_order(void) {
    section("T06  Callback order: mesh first, dhc second");
    uint64_t order[2] = {0, 0};
    uint64_t seq = 0;

    /* use closures via static state */
    static uint64_t *g_order; static uint64_t *g_seq;
    g_order = order; g_seq = &seq;

    void mesh_order_cb(const MeshEntry *e, void *ctx) {
        (void)e; (void)ctx; g_order[0] = ++(*g_seq);
    }
    void dhc_order_cb(const DetachEntry *e, void *ctx) {
        (void)e; (void)ctx; g_order[1] = ++(*g_seq);
    }

    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_mesh_cb(&cb, mesh_order_cb, NULL);
    detach_set_dhc_cb (&cb, dhc_order_cb,  NULL);

    DetachEntry ring[4]; memset(ring,0,sizeof(ring));
    ring[0] = make_de(0x5000ULL, DETACH_REASON_GEO_INVALID, 5);

    _detach_fire_callbacks(&cb, ring, 0, 1);

    check(order[0] == 1, "mesh fired first (seq=1)", "wrong");
    check(order[1] == 2, "dhc fired second (seq=2)", "wrong");
}

static void t07_null_safety(void) {
    section("T07  NULL safety for callbacks");
    DetachCallbacks cb; detach_callbacks_init(&cb);
    DetachEntry ring[4]; memset(ring,0,sizeof(ring));
    ring[0] = make_de(0x1000ULL, DETACH_REASON_GEO_INVALID, 3);

    /* both NULL: no crash */
    _detach_fire_callbacks(&cb,  ring, 0, 1);
    _detach_fire_callbacks(NULL, ring, 0, 1);
    _detach_fire_callbacks(&cb,  NULL, 0, 1);

    detach_set_mesh_cb(NULL, test_mesh_cb, NULL);
    detach_set_dhc_cb (NULL, test_dhc_cb,  NULL);
    check(1, "all NULL paths survived", "crash");
}

static void t08_ring_wrap(void) {
    section("T08  Ring mask wrap-around (tail + count crosses boundary)");
    reset_counters();
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_dhc_cb(&cb, test_dhc_cb, NULL);

    /* ring of exactly DETACH_RING_MASK+1 = 4096 entries */
    static DetachEntry ring[4096];
    memset(ring, 0, sizeof(ring));

    /* place entries at tail = 4094, 4095, 0 (wraps) */
    ring[4094] = make_de(0xA000ULL, DETACH_REASON_GEO_INVALID, 1);
    ring[4095] = make_de(0xB000ULL, DETACH_REASON_GEO_INVALID, 2);
    ring[0]    = make_de(0xC000ULL, DETACH_REASON_GEO_INVALID, 3);

    _detach_fire_callbacks(&cb, ring, 4094, 3);  /* wraps at 4095→0 */

    check(g_dhc_calls == 3, "3 entries across ring wrap", "wrong");
    check(g_last_dhc_addr == 0xC000ULL, "last entry addr correct", "wrong");
}

/* ══════════════════════════════════════════════════════════════════
 * Group 3: SliceTag
 * ══════════════════════════════════════════════════════════════════ */

static void t09_slice_of_lane(void) {
    section("T09  slice_of_lane: 0-17→0, 18-35→1, 36-53→2");
    check(slice_of_lane(0)  == 0, "lane 0 → slice 0",  "wrong");
    check(slice_of_lane(17) == 0, "lane 17 → slice 0", "wrong");
    check(slice_of_lane(18) == 1, "lane 18 → slice 1", "wrong");
    check(slice_of_lane(35) == 1, "lane 35 → slice 1", "wrong");
    check(slice_of_lane(36) == 2, "lane 36 → slice 2", "wrong");
    check(slice_of_lane(53) == 2, "lane 53 → slice 2", "wrong");
}

static void t10_ghost_slice(void) {
    section("T10  ghost always crosses slice (100%)");
    int same = 0;
    for (uint8_t lane=0; lane<54; lane++) {
        if (slice_of_lane(lane) == slice_ghost_of_lane(lane)) same++;
    }
    check(same == 0, "0 lanes stay in same slice (100% cross)", "some stay");
}

static void t11_tag_write_read(void) {
    section("T11  slice_tag_block + slice_read_tag roundtrip");
    WireBlock blk; memset(&blk, 0, sizeof(blk));

    slice_tag_block(&blk, 1, 0, 12345u);
    SliceTag t = slice_read_tag(&blk);

    check(t.valid      == 1,     "tag valid",           "not valid");
    check(t.slice_id   == 1,     "slice_id=1",          "wrong");
    check(t.hop_count  == 0,     "hop_count=0",         "wrong");
    check(t.engine_id  == 1,     "engine_id=1",         "wrong");
    check(t.op_seq     == 12345u,"op_seq=12345",        "wrong");
}

static void t12_tag_all_slices(void) {
    section("T12  Tag all 3 slices correctly");
    for (uint8_t s=0; s<3; s++) {
        WireBlock blk; memset(&blk, 0, sizeof(blk));
        slice_tag_block(&blk, s, s, (uint32_t)s * 1000u);
        SliceTag t = slice_read_tag(&blk);
        check(t.slice_id  == s,           "slice_id correct",  "wrong");
        check(t.hop_count == s,           "hop_count correct", "wrong");
        check(t.op_seq    == s*1000u,     "op_seq correct",    "wrong");
    }
}

static void t13_ghost_block_detect(void) {
    section("T13  slice_block_is_ghost detects hop_count > 0");
    WireBlock main_blk; memset(&main_blk, 0, sizeof(main_blk));
    WireBlock ghost_blk; memset(&ghost_blk, 0, sizeof(ghost_blk));

    slice_tag_block(&main_blk,  0, 0, 100u);  /* hop=0: not ghost */
    slice_tag_block(&ghost_blk, 1, 1, 200u);  /* hop=1: ghost     */

    check(!slice_block_is_ghost(&main_blk),  "main: not ghost",  "wrong");
    check( slice_block_is_ghost(&ghost_blk), "ghost: is ghost",  "wrong");
}

static void t14_fast_id_extract(void) {
    section("T14  slice_block_id fast extraction");
    WireBlock blk; memset(&blk, 0, sizeof(blk));
    slice_tag_block(&blk, 2, 1, 999u);
    check(slice_block_id(&blk) == 2, "slice_id=2 fast extract", "wrong");
}

static void t15_untagged_block(void) {
    section("T15  Untagged block (data[5]=0) → valid=0");
    WireBlock blk; memset(&blk, 0, sizeof(blk));  /* all zeros */
    SliceTag t = slice_read_tag(&blk);
    check(t.valid == 0, "untagged: valid=0", "wrong");
    check(!slice_block_is_ghost(&blk), "untagged: not ghost", "wrong");
    check(slice_block_id(&blk) == 0,   "untagged: id=0",      "wrong");
}

static void t16_tag_preserves_other_words(void) {
    section("T16  Tagging preserves WireBlock.data[0..4,6,7]");
    WireBlock blk; memset(&blk, 0, sizeof(blk));
    /* set some existing data */
    blk.data[0] = 0xDEADBEEFULL;
    blk.data[1] = 0xCAFEBABEULL;
    blk.data[7] = 0x12345678ULL;

    slice_tag_block(&blk, 0, 0, 42u);

    check(blk.data[0] == 0xDEADBEEFULL, "data[0] preserved", "clobbered");
    check(blk.data[1] == 0xCAFEBABEULL, "data[1] preserved", "clobbered");
    check(blk.data[7] == 0x12345678ULL, "data[7] preserved", "clobbered");
}

static void t17_null_safety_tag(void) {
    section("T17  NULL safety for slice tag");
    slice_tag_block(NULL, 0, 0, 0);
    SliceTag t = slice_read_tag(NULL);
    check(t.valid == 0,                 "read_tag(NULL): valid=0",  "crash");
    check(!slice_block_is_ghost(NULL),  "is_ghost(NULL)=0",         "crash");
    check(slice_block_id(NULL) == 0,    "block_id(NULL)=0",         "crash");
    check(1, "all NULL survived", "crash");
}

/* ══════════════════════════════════════════════════════════════════
 * Group 4: integration — tag flows into callback
 * ══════════════════════════════════════════════════════════════════ */

static void t18_pipeline_simulation(void) {
    section("T18  Simulated pipeline: tag → write → detach → callback");
    reset_counters();

    /* 1. Create WireBlock and tag it (lane 20 = slice 1) */
    WireBlock blk; memset(&blk, 0, sizeof(blk));
    blk.data[0] = 0xDEADBEEFULL;   /* value */
    blk.data[1] = 0x1000ULL;       /* angular_addr */
    slice_tag_block(&blk, slice_of_lane(20), 0, 500u);

    /* 2. Read tag back (Mesh would do this) */
    SliceTag tag = slice_read_tag(&blk);
    check(tag.slice_id == 1, "tagged as slice 1 (lane 20)", "wrong");
    check(tag.op_seq == 500u, "op_seq=500", "wrong");

    /* 3. Simulate detach entry from same address */
    DetachCallbacks cb; detach_callbacks_init(&cb);
    detach_set_mesh_cb(&cb, test_mesh_cb, NULL);
    detach_set_dhc_cb (&cb, test_dhc_cb,  NULL);

    DetachEntry ring[4]; memset(ring,0,sizeof(ring));
    ring[0] = make_de(blk.data[1], DETACH_REASON_GEO_INVALID, 5);

    _detach_fire_callbacks(&cb, ring, 0, 1);

    check(g_mesh_calls == 1, "mesh callback fired",  "wrong");
    check(g_dhc_calls  == 1, "dhc callback fired",   "wrong");
    printf("    (slice=%u op_seq=%u mesh_type=%u)\n",
           tag.slice_id, tag.op_seq, (uint32_t)g_last_mesh_type);
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Quick Wins — DHC Callbacks + SliceTag\n");
    printf("══════════════════════════════════════════════════\n");

    printf("\n=== Group 1: DetachCallbacks init ===\n");
    t01_init(); t02_set_callbacks();

    printf("\n=== Group 2: _detach_fire_callbacks ===\n");
    t03_both_callbacks_fire();
    t04_mesh_anomaly_guard();
    t05_dhc_no_guard();
    t06_callback_order();
    t07_null_safety();
    t08_ring_wrap();

    printf("\n=== Group 3: SliceTag ===\n");
    t09_slice_of_lane();
    t10_ghost_slice();
    t11_tag_write_read();
    t12_tag_all_slices();
    t13_ghost_block_detect();
    t14_fast_id_extract();
    t15_untagged_block();
    t16_tag_preserves_other_words();
    t17_null_safety_tag();

    printf("\n=== Group 4: integration ===\n");
    t18_pipeline_simulation();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  v ALL PASS — Quick wins done ⚡\n",
               g_pass, g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
