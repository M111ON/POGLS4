/**
 * test_qrpn_hex.c — ตรวจสอบ qrpn_phi_scatter_hex()
 *
 * Test 1: Determinism — input เดิม → output เดิมเสมอ
 * Test 2: Entropy separation — Cg_hex ≠ Cq (radial) มากพอ
 *         ต้องการ collision rate < 1% (ถ้า == แปลว่า path ไม่ independent)
 * Test 3: Distribution — output กระจายทั้ง 32-bit ไม่ cluster
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "pogls_platform.h"
#include "pogls_qrpn.h"

#define N_TEST  100000u

/* splitmix64 */
static uint64_t _s = 0x123456789ABCDEFULL;
static inline uint64_t rng(void) {
    _s ^= _s >> 30; _s *= 0xbf58476d1ce4e5b9ULL;
    _s ^= _s >> 27; _s *= 0x94d049bb133111ebULL;
    _s ^= _s >> 31; return _s;
}

int main(void)
{
    int pass = 1;

    /* ── Test 1: Determinism ── */
    printf("[T1] Determinism... ");
    {
        uint64_t v = 0xDEADBEEFCAFEBABEULL;
        uint32_t a = qrpn_phi_scatter_hex(v);
        uint32_t b = qrpn_phi_scatter_hex(v);
        uint32_t c = qrpn_phi_scatter_hex(v);
        if (a == b && b == c) {
            printf("PASS (0x%08X)\n", a);
        } else {
            printf("FAIL a=%08X b=%08X c=%08X\n", a, b, c);
            pass = 0;
        }
    }

    /* ── Test 2: Entropy separation vs radial Cq ── */
    printf("[T2] Entropy separation (hex vs radial)... ");
    {
        qrpn_ctx_t ctx;
        qrpn_ctx_init(&ctx, 8u);

        uint32_t collision = 0;
        uint32_t total     = N_TEST;

        for (uint32_t i = 0; i < total; i++) {
            uint64_t v  = rng();

            /* Cq: radial path (CPU) */
            uint32_t A  = qrpn_radial(v, ctx.N, ctx.seedA);
            uint32_t B  = qrpn_radial(v, ctx.N, ctx.seedB);
            uint64_t c  = (uint64_t)A*A + (uint64_t)B*B;
            uint32_t Cq = qrpn_mix32(c);

            /* Cg: hex path (GPU stub) */
            uint32_t Cg = qrpn_phi_scatter_hex(v);

            if (Cq == Cg) collision++;
        }

        double rate = (double)collision * 100.0 / (double)total;
        /* expected collision ~ 1/2^32 × N ≈ 0.002% */
        if (rate < 1.0) {
            printf("PASS collision=%.4f%% (%u/%u)\n", rate, collision, total);
        } else {
            printf("FAIL collision too high: %.4f%%\n", rate);
            pass = 0;
        }
    }

    /* ── Test 3: Distribution — check bit spread ── */
    printf("[T3] Distribution (bit spread)... ");
    {
        uint32_t bit_count[32] = {0};
        uint32_t n = N_TEST;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t out = qrpn_phi_scatter_hex(rng());
            for (int b = 0; b < 32; b++)
                if (out & (1u << b)) bit_count[b]++;
        }

        /* each bit should be ~50% ± 2% */
        int bad = 0;
        for (int b = 0; b < 32; b++) {
            double pct = (double)bit_count[b] * 100.0 / (double)n;
            if (pct < 45.0 || pct > 55.0) {
                printf("\n  bit[%d]=%.1f%% SKEWED", b, pct);
                bad++;
            }
        }
        if (bad == 0) {
            printf("PASS all 32 bits 45-55%%\n");
        } else {
            printf("\nFAIL %d bits skewed\n", bad);
            pass = 0;
        }
    }

    /* ── Test 4: phi_scatter_hex vs phi_scatter (ต้องต่างกัน) ── */
    printf("[T4] hex_scatter ≠ phi_scatter... ");
    {
        uint32_t same = 0;
        for (uint32_t i = 0; i < 10000; i++) {
            uint64_t v = rng();
            if (qrpn_phi_scatter_hex(v) == qrpn_phi_scatter(v)) same++;
        }
        double rate = (double)same * 100.0 / 10000.0;
        if (rate < 1.0) {
            printf("PASS different paths (collision=%.3f%%)\n", rate);
        } else {
            printf("FAIL paths too similar: %.3f%%\n", rate);
            pass = 0;
        }
    }

    printf("\n%s\n", pass ? "ALL TESTS PASS" : "SOME TESTS FAILED");
    return pass ? 0 : 1;
}
