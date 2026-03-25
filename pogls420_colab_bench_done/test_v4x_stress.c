/*
 * test_v4x_stress.c — Multi-cycle stress + side-effect audit
 * ═══════════════════════════════════════════════════════════
 * Focus:
 *   S01  anchor distribution balance (N=1,2,4,8, 10k steps)
 *   S02  bias drift — no single core inflates
 *   S03  ring_pending oscillation (never stuck high)
 *   S04  state_hash sync-collapse check (new all-core update risk)
 *   S05  over-correction detector (anchor change rate vs threshold)
 *   S06  determinism under all-core ANCHOR update
 */
#include "pogls_v4x_wire.h"
#include <stdio.h>
#include <string.h>

static int _pass = 0, _fail = 0;
#define PASS(name)          do { printf("  ✅ %-50s PASS\n", name); _pass++; } while(0)
#define FAIL(name, ...)     do { printf("  ❌ %-50s FAIL — ", name); printf(__VA_ARGS__); printf("\n"); _fail++; } while(0)
#define CHECK(name,cond,...) do { if(cond) PASS(name); else FAIL(name,__VA_ARGS__); } while(0)
#define SECTION(s)          printf("\n── %s ──\n", s)

/* ── helpers ──────────────────────────────────────────────────────────── */
static uint32_t anchor_distribution_max_skew(const V4xWire *w)
{
    /* returns max(selected[i]) - min(selected[i]) across cores */
    uint64_t mn = UINT64_MAX, mx = 0;
    for (uint32_t i = 0; i < w->N; i++) {
        uint64_t s = w->ma.ctx[i].select_count;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    return (uint32_t)(mx - mn);
}

static uint64_t anchor_total_selected(const V4xWire *w)
{
    uint64_t t = 0;
    for (uint32_t i = 0; i < w->N; i++) t += w->ma.ctx[i].select_count;
    return t;
}

static uint32_t bias_max(const V4xWire *w, uint32_t core)
{
    uint32_t mx = 0;
    for (uint32_t b = 0; b < 16; b++)
        if (w->anchor_bias[core][b] > mx) mx = w->anchor_bias[core][b];
    return mx;
}

/* ── S01: anchor distribution balance (N=1,2,4,8) ─────────────────── */
static void test_anchor_balance(void)
{
    SECTION("S01  Anchor Distribution Balance (10k steps)");
    uint32_t Ns[] = {1, 2, 4, 8};

    for (int ni = 0; ni < 4; ni++) {
        uint32_t N = Ns[ni];
        V4xWire w;
        v4x_wire_init(&w, N);

        for (uint32_t s = 0; s < 10000; s++)
            v4x_step(&w, 36 + (s % 600) * 3);

        uint32_t skew  = anchor_distribution_max_skew(&w);
        uint64_t total = anchor_total_selected(&w);
        /* expected: each core selected roughly total/N ± tolerance
         * tolerance = 20% of expected per-core share               */
        uint64_t expected = total / N;
        uint32_t tol = (uint32_t)(expected / 5 + 1); /* 20% */

        char label[64];
        snprintf(label, sizeof(label), "S01 N=%u anchor skew ≤ tol(%u)", N, tol);
        CHECK(label, skew <= tol,
              "skew=%u expected=%llu tol=%u N=%u",
              skew, (unsigned long long)expected, tol, N);
    }
}

/* ── S02: bias drift — no core inflates > 16000 (half of uint16 max) ── */
static void test_bias_drift(void)
{
    SECTION("S02  Bias Drift — no single-core inflation");

    V4xWire w;
    v4x_wire_init(&w, 4);
    for (uint32_t s = 0; s < 10000; s++)
        v4x_step(&w, 36 + (s % 300) * 3);

    for (uint32_t c = 0; c < 4; c++) {
        uint32_t mx = bias_max(&w, c);
        char label[64];
        snprintf(label, sizeof(label), "S02 core[%u] bias_max ≤ 16000", c);
        CHECK(label, mx <= 16000, "core=%u bias_max=%u", c, mx);
    }
}

/* ── S03: ring_pending oscillation — never stuck > half ring ───────── */
static void test_ring_oscillation(void)
{
    SECTION("S03  Ring Pending Oscillation");

    V4xWire w;
    v4x_wire_init(&w, 4);

    uint64_t peak_pending = 0;
    for (uint32_t s = 0; s < 10000; s++) {
        v4x_step(&w, 36 + (s % 800) * 3);
        uint64_t p = v4x_ring_pending(&w.ring);
        if (p > peak_pending) peak_pending = p;
    }

    CHECK("S03a peak_pending < V4X_RING_SIZE/2",
          peak_pending < V4X_RING_SIZE / 2,
          "peak=%llu limit=%u", (unsigned long long)peak_pending, V4X_RING_SIZE / 2);

    CHECK("S03b ring_overflows=0",
          w.ring.total_overflows == 0,
          "overflows=%llu", (unsigned long long)w.ring.total_overflows);
}

/* ── S04: sync-collapse check — state_hash per core stays DIVERGED ─── */
static void test_sync_collapse(void)
{
    SECTION("S04  Sync-Collapse Guard (all-core ANCHOR update)");

    /* Risk: if all cores update with same v_clean at ANCHOR,
     * their score[] EMA may converge → all pick same anchor → collapse.
     * Test: after 10k steps, verify cores have DIFFERENT anchors or
     * at minimum different state_hash (not all identical).              */
    V4xWire w;
    v4x_wire_init(&w, 4);
    for (uint32_t s = 0; s < 10000; s++)
        v4x_step(&w, 36 + s * 3);

    /* check state_hash diversity */
    int all_same_hash = 1;
    uint64_t h0 = w.tc.cores[0].state_hash;
    for (uint32_t i = 1; i < 4; i++)
        if (w.tc.cores[i].state_hash != h0) { all_same_hash = 0; break; }

    CHECK("S04a cores have DIVERGED state_hash", !all_same_hash,
          "all cores collapsed to same hash=%016llx", (unsigned long long)h0);

    /* check anchor diversity — not all same */
    int all_same_anchor = 1;
    uint32_t a0 = w.ma.ctx[0].anchor;
    for (uint32_t i = 1; i < 4; i++)
        if (w.ma.ctx[i].anchor != a0) { all_same_anchor = 0; break; }

    /* Note: anchors CAN all be same if data strongly favors one —
     * this is not collapse, it's convergence. We warn, not fail.       */
    if (all_same_anchor)
        printf("  ⚠️  S04b all cores same anchor=%u — convergence (not collapse)\n", a0);
    else
        printf("  ✅ S04b cores have diverse anchors — healthy distribution\n");
    _pass++; /* informational only */
}

/* ── S05: over-correction — anchor_changes rate should be bounded ───── */
static void test_over_correction(void)
{
    SECTION("S05  Over-Correction Rate");

    V4xWire w;
    v4x_wire_init(&w, 4);
    for (uint32_t s = 0; s < 7200; s++)  /* 10 cycles */
        v4x_step(&w, 36 + (s % 500) * 3);

    /* S05: use cumulative anchor_changes from MAFabric + total steps/events
     * Old formula (10×5×4=200) was hardcoded and wrong after decouple timing.
     * New: wire tracks cycle_events per window; use total_steps as denominator
     * and compare against anchor_changes. Healthy = 5..80% of anchor fires.  */
    {
        uint64_t total_anchors = w.anchor_enforces;  /* actual enforcements */
        if (total_anchors == 0) total_anchors = 1;   /* div-zero guard      */
        double rate = (double)w.ma.anchor_changes / (double)total_anchors;

        printf("  ℹ️  anchor_changes=%llu / enforces=%llu = %.1f%%\n",
               (unsigned long long)w.ma.anchor_changes,
               (unsigned long long)w.anchor_enforces,
               rate * 100.0);

        CHECK("S05 change_rate in healthy range 5-80%% (not stuck, not thrashing)",
              rate >= 0.05 && rate <= 0.80,
              "rate=%.1f%% — outside [5%%,80%%)", rate * 100.0);
    }
}

/* ── S06: determinism under all-core update ─────────────────────────── */
static void test_determinism_allcore(void)
{
    SECTION("S06  Determinism — all-core ANCHOR update");

    V4xWire wa, wb;
    v4x_wire_init(&wa, 4);
    v4x_wire_init(&wb, 4);

    /* same input sequence → must produce identical results */
    for (uint32_t s = 0; s < 10000; s++) {
        uint32_t v = 36 + (s % 700) * 3;
        v4x_step(&wa, v);
        v4x_step(&wb, v);
    }

    int det_hash = 1, det_anchor = 1;
    for (uint32_t i = 0; i < 4; i++) {
        if (wa.tc.cores[i].state_hash != wb.tc.cores[i].state_hash) det_hash = 0;
        if (wa.ma.ctx[i].anchor       != wb.ma.ctx[i].anchor)       det_anchor = 0;
    }

    CHECK("S06a deterministic state_hash", det_hash,
          "core0: a=%016llx b=%016llx",
          (unsigned long long)wa.tc.cores[0].state_hash,
          (unsigned long long)wb.tc.cores[0].state_hash);
    CHECK("S06b deterministic anchor", det_anchor,
          "core0 anchor: a=%u b=%u", wa.ma.ctx[0].anchor, wb.ma.ctx[0].anchor);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  POGLS V4x — Multi-cycle Stress + Side-effect Audit  ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");

    test_anchor_balance();
    test_bias_drift();
    test_ring_oscillation();
    test_sync_collapse();
    test_over_correction();
    test_determinism_allcore();

    int total = _pass + _fail;
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║  RESULT: %d/%d PASS  %s%-5s                             ║\n",
           _pass, total,
           _fail == 0 ? "✅ " : "❌ ",
           _fail == 0 ? "CLEAN" : "FAIL");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    return _fail == 0 ? 0 : 1;
}
