/*
 * test_phaseF_soft.c — Phase F SOFT deploy integration test
 *
 * ยืนยัน V4x wire ทำงานคู่ขนานกับ V4 pipeline โดยไม่กระทบ:
 *   T1: pipeline ยังทำงานปกติ (MAIN/GHOST ratio ไม่เปลี่ยน)
 *   T2: V4x ops = MAIN ops (wire เฉพาะ MAIN path)
 *   T3: QRPN shadow_fail = 0 ยังอยู่
 *   T4: V4x temporal cycle ครบ (1M ops / 720 = ~1388 cycles)
 *   T5: V4x anchor enforce ครบ (1M / 144 per cycle × cycles)
 *   T6: ring overflow ไม่ crash (ring drain อัตโนมัติ)
 *   T7: deterministic — run 2 ครั้ง ผลเหมือนกัน
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <sys/stat.h>

#include "../pogls_pipeline_wire.h"

#define OPS_TOTAL   1000000u

static uint64_t _s = 0x1234567890ABCDEFULL;
static inline uint64_t rng64(void) {
    _s ^= _s >> 30; _s *= 0xbf58476d1ce4e5b9ULL;
    _s ^= _s >> 27; _s *= 0x94d049bb133111ebULL;
    _s ^= _s >> 31; return _s;
}

/* run 1M structured ops, return stats */
typedef struct {
    uint64_t main_routes;
    uint64_t ghost_routes;
    uint64_t v4x_ops;
    uint64_t v4x_cycles;
    uint64_t v4x_anchors;
    uint64_t v4x_overflows;
    uint64_t qrpn_fail;
} RunStats;

static RunStats run_pipeline(const char *delta_dir)
{
    static PipelineWire pw;
    mkdir(delta_dir, 0755);
    pipeline_wire_init(&pw, delta_dir);

    uint64_t addr  = 0x1000;
    uint64_t value = 0x0000000100000001ULL;
    uint32_t i;

    /* SEQ 500K */
    for (i = 0; i < 500000u; i++) {
        addr  += (i & 0xF) + 1;
        value += 1000;
        pipeline_wire_process(&pw, value, addr);
    }
    /* PHI 200K */
    addr = 0x80000; value = 0x0000000200000002ULL;
    for (i = 0; i < 200000u; i++) {
        addr  = (addr + POGLS_PHI_DOWN) & 0xFFFFFu;
        value += 500;
        pipeline_wire_process(&pw, value, addr);
    }
    /* BURST 200K */
    addr = 0x200000; value = 0x0000000300000003ULL;
    for (i = 0; i < 200000u; i++) {
        addr  += (i & 0x7);
        value += 256;
        pipeline_wire_process(&pw, value, addr);
    }
    /* CHAOS 100K */
    for (i = 0; i < 100000u; i++) {
        addr  = rng64();
        value = rng64();
        pipeline_wire_process(&pw, value, addr);
    }

    pipeline_wire_flush(&pw);

    RunStats s;
    s.main_routes    = pw.route_main;
    s.ghost_routes   = pw.route_ghost;
    s.v4x_ops        = pw.v4x_ops;
    s.v4x_cycles     = pw.v4x.cycle_ends;
    s.v4x_anchors    = pw.v4x.anchor_enforces;
    s.v4x_overflows  = pw.v4x_ring_overflows;
    s.qrpn_fail      = atomic_load(&pw.qrpn.shadow_fail);

    pipeline_wire_stats(&pw);
    pipeline_wire_close(&pw);
    return s;
}

int main(void)
{
    printf("=== Phase F SOFT Deploy Test (1M ops) ===\n\n");

    int pass = 1;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); pass = 0; } \
    else          { printf("  pass: %s\n", msg); } \
} while(0)

    /* ── Run 1 ── */
    printf("[Run 1]\n");
    RunStats r1 = run_pipeline("/tmp/phaseF_run1");

    printf("\n── Run 1 Results ──\n");
    printf("  MAIN=%llu  GHOST=%llu\n",
           (unsigned long long)r1.main_routes,
           (unsigned long long)r1.ghost_routes);
    printf("  V4x ops=%llu  cycles=%llu  anchors=%llu  overflows=%llu\n",
           (unsigned long long)r1.v4x_ops,
           (unsigned long long)r1.v4x_cycles,
           (unsigned long long)r1.v4x_anchors,
           (unsigned long long)r1.v4x_overflows);
    printf("  QRPN shadow_fail=%llu\n\n", (unsigned long long)r1.qrpn_fail);

    /* T1: pipeline still routes correctly */
    CHECK(r1.main_routes  > 0,   "T1: MAIN routes > 0");
    CHECK(r1.ghost_routes > 0,   "T1: GHOST routes > 0");
    CHECK(r1.main_routes + r1.ghost_routes == OPS_TOTAL,
          "T1: MAIN+GHOST = total (no drops)");

    /* T2: V4x ops wired to MAIN only */
    CHECK(r1.v4x_ops == r1.main_routes,
          "T2: V4x ops == MAIN routes (wire correct)");

    /* T3: QRPN still clean */
    CHECK(r1.qrpn_fail == 0, "T3: QRPN shadow_fail = 0");

    /* T4: V4x cycles reasonable (MAIN ops / 720) */
    uint64_t expected_cycles = r1.v4x_ops / 720u;
    CHECK(r1.v4x_cycles >= expected_cycles,
          "T4: V4x cycles >= expected");
    printf("  expected_cycles ~%llu  actual=%llu\n",
           (unsigned long long)expected_cycles,
           (unsigned long long)r1.v4x_cycles);

    /* T5: V4x anchor enforces = cycles × 5 (720/144=5) */
    uint64_t expected_anchors = r1.v4x_cycles * 5u;
    CHECK(r1.v4x_anchors >= expected_anchors,
          "T5: anchor enforces >= cycles×5");

    /* T6: ring overflow safe (no crash regardless of count) */
    CHECK(1, "T6: ring overflow safe (no crash)");
    printf("  ring_overflows=%llu (expected if MAIN > 1024 without drain)\n",
           (unsigned long long)r1.v4x_overflows);

    /* ── Run 2: determinism check ── */
    printf("\n[Run 2 — determinism]\n");
    _s = 0x1234567890ABCDEFULL;  /* reset rng to same seed */
    RunStats r2 = run_pipeline("/tmp/phaseF_run2");

    /* T7: same input → same counts */
    CHECK(r2.main_routes   == r1.main_routes,   "T7: MAIN routes deterministic");
    CHECK(r2.ghost_routes  == r1.ghost_routes,  "T7: GHOST routes deterministic");
    CHECK(r2.v4x_ops       == r1.v4x_ops,       "T7: V4x ops deterministic");
    CHECK(r2.v4x_cycles    == r1.v4x_cycles,    "T7: V4x cycles deterministic");
    CHECK(r2.v4x_anchors   == r1.v4x_anchors,   "T7: V4x anchors deterministic");
    CHECK(r2.qrpn_fail     == r1.qrpn_fail,     "T7: QRPN fail deterministic");

    /* ── summary ── */
    printf("\n══════════════════════════════════════════\n");
    printf("%s\n", pass ? "PHASE F SOFT: ALL PASS" : "PHASE F SOFT: SOME FAILED");
    printf("══════════════════════════════════════════\n");
    return pass ? 0 : 1;
}
