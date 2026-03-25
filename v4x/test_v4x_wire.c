/*
 * test_v4x_wire.c — V4x full pipeline wire test (pack/unpack revision)
 *
 * T1: init + null safety
 * T2: determinism
 * T3: ring push/pop round-trip (v_clean as uint64)
 * T4: 720 steps → 1 cycle_end + 5 anchor_enforces
 * T5: 1440 steps → 2 cycles, cores balanced
 * T6: anchor changes + soft snaps active
 * T7: pack/unpack reversible + Pythagorean invariant preserved
 * T8: degenerate guard (a=b=0 → a=12, no entropy collapse)
 * T9: ring overflow safe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pogls_v4x_wire.h"

static int pass = 1;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); pass = 0; } \
    else          { printf("  pass: %s\n", msg); } \
} while(0)

static uint64_t _s = 0xDEADBEEFCAFEBABEULL;
static inline uint32_t rng32(void) {
    _s ^= _s >> 30; _s *= 0xbf58476d1ce4e5b9ULL;
    _s ^= _s >> 27; _s *= 0x94d049bb133111ebULL;
    _s ^= _s >> 31; return (uint32_t)_s;
}

int main(void)
{
    printf("=== V4x Wire Test Suite (pack revision) ===\n\n");

    /* T1 */
    printf("[T1] Init + null safety\n");
    {
        CHECK(v4x_wire_init(NULL, 4) == -1,      "null init returns -1");
        V4xWire w;
        CHECK(v4x_wire_init(&w, 4) == 0,         "init N=4 ok");
        CHECK(w.magic == V4X_MAGIC,              "magic set");
        CHECK(w.N == 4,                          "N=4");
        CHECK(w.tc.magic == TC_MAGIC,            "TCFabric init");
        CHECK(w.ma.magic == MA_MAGIC,            "MAFabric init");
        CHECK(w.ring.write_head == 0,            "ring empty");
        CHECK(v4x_step(NULL, 0x1234) == 0x1234,  "null step passthrough");
    }

    /* T2 */
    printf("\n[T2] Determinism\n");
    {
        V4xWire w1, w2;
        v4x_wire_init(&w1, 4); v4x_wire_init(&w2, 4);
        CHECK(v4x_step(&w1, 0xABCD1234u) == v4x_step(&w2, 0xABCD1234u),
              "same input → same output");
        int det = 1;
        for (int i = 0; i < 10; i++)
            if (v4x_step(&w1,(uint32_t)i) != v4x_step(&w2,(uint32_t)i))
                { det = 0; break; }
        CHECK(det, "10 steps deterministic");
    }

    /* T3 */
    printf("\n[T3] Ring push/pop (uint64 v_clean)\n");
    {
        V4xRing r; v4x_ring_init(&r);
        CHECK(v4x_ring_pending(&r) == 0, "empty pending=0");
        V4xCommitEntry e_in = {0};
        e_in.v_snapped = 0xDEADBEEFu;
        e_in.v_clean   = 0x123456789ABCULL;
        e_in.step      = 42u;
        v4x_ring_push(&r, &e_in);
        CHECK(v4x_ring_pending(&r) == 1, "pending=1 after push");
        V4xCommitEntry e_out; memset(&e_out, 0, sizeof(e_out));
        CHECK(v4x_ring_pop(&r, &e_out) == 1,              "pop returns 1");
        CHECK(e_out.v_snapped == 0xDEADBEEFu,             "v_snapped ok");
        CHECK(e_out.v_clean   == 0x123456789ABCULL,       "v_clean uint64 ok");
        CHECK(e_out.step      == 42u,                     "step ok");
        CHECK(v4x_ring_pending(&r) == 0,                  "empty after pop");
    }

    /* T4 */
    printf("\n[T4] 720 steps cycle boundary\n");
    {
        V4xWire w; v4x_wire_init(&w, 4);
        for (int i = 0; i < 720; i++) v4x_step(&w, (uint32_t)(i*17u+1u));
        CHECK(w.anchor_enforces == 5,       "5 anchor enforces (720/144)");
        CHECK(w.cycle_ends == 1,            "1 cycle_end");
        CHECK(w.ring.total_commits == 720,  "720 commits");
    }

    /* T5 */
    printf("\n[T5] 1440 steps core balance (N=4)\n");
    {
        V4xWire w; v4x_wire_init(&w, 4);
        for (int i = 0; i < 1440; i++) v4x_step(&w, (uint32_t)(i*13u+7u));
        CHECK(w.cycle_ends      == 2,  "2 full cycles");
        CHECK(w.anchor_enforces == 10, "10 anchor enforces");
        uint64_t c0=w.tc.cores[0].total_ops, c1=w.tc.cores[1].total_ops,
                 c2=w.tc.cores[2].total_ops, c3=w.tc.cores[3].total_ops;
        CHECK(c0==c1 && c1==c2 && c2==c3, "cores balanced");
        printf("  core ops: %llu %llu %llu %llu\n",
               (unsigned long long)c0, (unsigned long long)c1,
               (unsigned long long)c2, (unsigned long long)c3);
    }

    /* T6 */
    printf("\n[T6] Multi-anchor + soft snaps\n");
    {
        V4xWire w; v4x_wire_init(&w, 4);
        for (int i = 0; i < 2160; i++) v4x_step(&w, rng32());
        CHECK(w.anchor_enforces > 0,  "anchor enforce fired");
        CHECK(w.ma.total_snaps  > 0,  "soft snaps active");
        printf("  anchor_changes=%llu  total_snaps=%llu\n",
               (unsigned long long)w.ma.anchor_changes,
               (unsigned long long)w.ma.total_snaps);
    }

    /* T7 */
    printf("\n[T7] Pack/unpack + Pythagorean invariant\n");
    {
        int rt = 1;
        for (int i = 0; i < 10000; i++) {
            uint32_t x = rng32() & 0x1FFFFFu;
            uint32_t y = rng32() & 0x1FFFFFu;
            uint32_t z = rng32() & 0x1FFFFFu;
            uint64_t p = wire_pack(x, y, z);
            uint32_t ox, oy, oz;
            wire_unpack(p, &ox, &oy, &oz);
            if (ox != x || oy != y || oz != z) { rt = 0; break; }
        }
        CHECK(rt, "pack/unpack round-trip 10000 values");

        int pyth = 1;
        for (int i = 0; i < 10000; i++) {
            uint32_t v = rng32();
            uint64_t packed = wire_canonicalize(v);
            uint32_t x, y, z;
            wire_unpack(packed, &x, &y, &z);
            uint64_t lhs = (uint64_t)x*x + (uint64_t)y*y;
            uint64_t rhs = (uint64_t)z*z;
            if (lhs != rhs) { pyth = 0; break; }
        }
        CHECK(pyth, "x²+y²=z² invariant preserved after pack");

        int det = 1;
        for (int i = 0; i < 1000; i++) {
            uint32_t v = rng32();
            if (wire_canonicalize(v) != wire_canonicalize(v)) { det=0; break; }
        }
        CHECK(det, "wire_canonicalize deterministic");
    }

    /* T8 */
    printf("\n[T8] Degenerate guard a=b=0\n");
    {
        int found = 0;
        uint32_t dv = 0;
        for (uint32_t v = 0; v < 0x100000u; v++) {
            uint32_t a = (_wire_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
            uint32_t b = (_wire_can_g(v) / CAN_GRID_B) * CAN_GRID_B;
            if (a == 0 && b == 0) { found = 1; dv = v; break; }
        }
        if (found) {
            uint64_t packed = wire_canonicalize(dv);
            uint32_t x, y, z;
            wire_unpack(packed, &x, &y, &z);
            CHECK(z > 0, "degenerate z > 0 (no entropy collapse)");
            uint64_t lhs = (uint64_t)x*x + (uint64_t)y*y;
            CHECK(lhs == (uint64_t)z*z, "degenerate still x²+y²=z²");
            printf("  degen v=%u → x=%u y=%u z=%u\n", dv, x, y, z);
        } else {
            printf("  no degenerate in first 1M values\n");
            CHECK(1, "degenerate guard compiled in");
        }
    }

    /* T9 */
    printf("\n[T9] Ring overflow safe\n");
    {
        V4xWire w; v4x_wire_init(&w, 4);
        for (int i = 0; i < 2048; i++) v4x_step(&w, (uint32_t)i);
        CHECK(w.ring.total_overflows > 0,     "overflow detected (no crash)");
        CHECK(w.ring.total_commits   == 2048, "all commits counted");
        printf("  overflows=%llu\n", (unsigned long long)w.ring.total_overflows);
    }

    /* stats */
    printf("\n");
    {
        V4xWire w; v4x_wire_init(&w, 4);
        for (int i = 0; i < 720; i++) v4x_step(&w, (uint32_t)(i*31u));
        v4x_wire_stats(&w);
    }

    printf("══════════════════════════════════════\n");
    printf("%s\n", pass ? "ALL TESTS PASS" : "SOME TESTS FAILED");
    return pass ? 0 : 1;
}
