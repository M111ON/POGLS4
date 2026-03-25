/**
 * test_qrpn_shadow.c — QRPN shadow mode integration test
 *
 * Goal: รัน 1M ops ผ่าน pipeline_wire_process()
 *       verify shadow_fail = 0 (ไม่มี false positive)
 *
 * Pattern: seq / phi / burst / chaos (สัดส่วนเหมือน production)
 * Pass condition: qrpn.shadow_fail == 0
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <sys/stat.h>

/* stubs ที่ test ต้องการ */

#include "../pogls_pipeline_wire.h"

#define OPS_TOTAL   1000000u
#define OPS_SEQ     500000u   /* 50% sequential */
#define OPS_PHI     200000u   /* 20% phi-delta  */
#define OPS_BURST   200000u   /* 20% burst      */
#define OPS_CHAOS   100000u   /* 10% chaos      */

/* simple splitmix64 */
static uint64_t _rng = 0xDEADBEEFCAFEBABEULL;
static inline uint64_t rng64(void) {
    _rng ^= _rng >> 30;
    _rng *= 0xbf58476d1ce4e5b9ULL;
    _rng ^= _rng >> 27;
    _rng *= 0x94d049bb133111ebULL;
    _rng ^= _rng >> 31;
    return _rng;
}

int main(void)
{
    printf("[QRPN shadow] init pipeline...\n");

    /* tmp dir */
    mkdir("/tmp/qrpn_shadow_test", 0755);

    static PipelineWire pw;
    int r = pipeline_wire_init(&pw, "/tmp/qrpn_shadow_test");
    assert(r == 0);
    assert(pw.qrpn.mode == QRPN_SHADOW);

    uint64_t addr = 0x1000;
    uint64_t value = 0x0000000100000001ULL;  /* small structured value */

    uint32_t i;

    /* SEQ: value เพิ่มทีละน้อย < PW_DS_THRESH=65536, addr local step <=16 */
    printf("[QRPN shadow] SEQ  %u ops...\n", OPS_SEQ);
    for (i = 0; i < OPS_SEQ; i++) {
        addr  += (i & 0xF) + 1;   /* delta 1..16 → DualSensor local pass */
        value += 1000;             /* delta=1000 < 65536 → DeltaSensor small */
        pipeline_wire_process(&pw, value, addr);
    }

    /* PHI: addr step = PHI_DOWN → phi_b pass, value structured */
    printf("[QRPN shadow] PHI  %u ops...\n", OPS_PHI);
    addr = 0x80000;
    value = 0x0000000200000002ULL;
    for (i = 0; i < OPS_PHI; i++) {
        addr  = (addr + POGLS_PHI_DOWN) & 0xFFFFFu;
        value += 500;              /* structured small delta */
        pipeline_wire_process(&pw, value, addr);
    }

    /* BURST: addr อยู่ในช่วงแคบ delta <=16, value เพิ่มน้อยๆ */
    printf("[QRPN shadow] BURST %u ops...\n", OPS_BURST);
    addr = 0x200000;
    value = 0x0000000300000003ULL;
    for (i = 0; i < OPS_BURST; i++) {
        addr  += (i & 0x7);        /* delta 0..7 → ultra-local */
        value += 256;              /* delta=256 < 65536 → small */
        pipeline_wire_process(&pw, value, addr);
    }

    /* CHAOS: addr กระโดดสุ่ม (ยังคง chaos สำหรับ test ครบ path) */
    printf("[QRPN shadow] CHAOS %u ops...\n", OPS_CHAOS);
    for (i = 0; i < OPS_CHAOS; i++) {
        addr  = rng64();
        value = rng64();
        pipeline_wire_process(&pw, value, addr);
    }

    pipeline_wire_flush(&pw);

    /* ── results ── */
    uint64_t total_ops    = atomic_load(&pw.qrpn.total_ops);
    uint64_t shadow_fail  = atomic_load(&pw.qrpn.shadow_fail);

    printf("\n");
    pipeline_wire_stats(&pw);

    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("[QRPN] total_ops   = %llu\n", (unsigned long long)total_ops);
    printf("[QRPN] shadow_fail = %llu\n", (unsigned long long)shadow_fail);

    /* pass condition */
    if (shadow_fail == 0) {
        printf("[QRPN] PASS — shadow_fail = 0, baseline confirmed\n");
    } else {
        double rate = (double)shadow_fail * 100.0 / (double)(total_ops ? total_ops : 1);
        printf("[QRPN] INFO — shadow_fail = %llu (%.4f%%) — investigate before Phase E\n",
               (unsigned long long)shadow_fail, rate);
    }
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    pipeline_wire_close(&pw);
    return (shadow_fail == 0) ? 0 : 1;
}
