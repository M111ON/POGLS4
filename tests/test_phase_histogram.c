/*
 * test_phase_histogram.c — Twin Window Experiment
 *
 * คำถาม: anomaly cluster อยู่ใน twin window (phase288<18 หรือ phase306<18)
 *        จริงไหม หรือกระจายสม่ำเสมอ?
 *
 * ถ้าโน้ตถูก → spike ชัดใน phase18 histogram ตรง crossing
 * ถ้าสุ่ม    → histogram แบน (uniform)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../pogls_pipeline_wire.h"

#define N_OPS   2000000ULL   /* 2M ops — covers many 288/306 cycles */
#define BINS_18   18
#define BINS_288 288
#define BINS_306 306

/* workload — mixed (same as bench) */
static void gen_mixed(uint32_t *out, uint64_t N) {
    uint32_t x=42, ap=0;
    for (uint64_t i=0;i<N;i++) {
        uint32_t r=(uint32_t)(i&3);
        if      (r==0) out[i]=(uint32_t)((i*4)&(POGLS_PHI_SCALE-1u));
        else if (r==1) { ap=(ap+POGLS_PHI_DOWN)&(POGLS_PHI_SCALE-1u); out[i]=ap; }
        else if (r==2) out[i]=(uint32_t)(((i/8)*64)&(POGLS_PHI_SCALE-1u));
        else { x^=x>>13; x*=0x9e3779b9u; x^=x>>17; out[i]=x&(POGLS_PHI_SCALE-1u); }
    }
}

/* histogram of detach entries by phase */
typedef struct {
    uint64_t phase18[BINS_18];       /* [0..17] */
    uint64_t phase288[BINS_288];     /* [0..287] */
    uint64_t phase306[BINS_306];     /* [0..305] */
    uint64_t twin_window;            /* phase288<18 || phase306<18 */
    uint64_t non_twin;
    uint64_t total;
    /* reason breakdown */
    uint64_t geo_invalid;
    uint64_t ghost_drift;
} Histogram;

static void histo_add(Histogram *h, const DetachEntry *e) {
    h->phase18[e->phase18 % BINS_18]++;
    h->phase288[e->phase288 % BINS_288]++;
    h->phase306[e->phase306 % BINS_306]++;
    if (detach_is_twin_window(e)) h->twin_window++;
    else                          h->non_twin++;
    h->total++;
    if (e->reason & DETACH_REASON_GEO_INVALID) h->geo_invalid++;
    if (e->reason & DETACH_REASON_GHOST_DRIFT)  h->ghost_drift++;
}

static void print_phase18_bar(const Histogram *h) {
    uint64_t peak = 0;
    for (int i=0;i<BINS_18;i++) if (h->phase18[i]>peak) peak=h->phase18[i];
    if (!peak) return;
    printf("\n  phase18 histogram (anomaly count per gate step):\n");
    for (int i=0;i<BINS_18;i++) {
        int bar = (int)(h->phase18[i] * 40 / peak);
        printf("  %2d |%-40.*s| %llu\n", i, bar,
               "########################################",
               (unsigned long long)h->phase18[i]);
    }
}

static void print_phase_window(const Histogram *h, const char *name,
                               uint64_t *bins, int nbins) {
    /* show first 36 bins (2 × gate_18) and last few */
    uint64_t peak = 0;
    for (int i=0;i<nbins;i++) if (bins[i]>peak) peak=bins[i];
    if (!peak) return;
    printf("\n  %s (first 36 bins — 2 gate_18 windows):\n", name);
    for (int i=0;i<36 && i<nbins;i++) {
        int bar = (int)(bins[i] * 30 / peak);
        char flag[12] = "";
        if (i < 18) snprintf(flag, sizeof(flag), " ← twin?");
        printf("  %3d |%-30.*s| %llu%s\n", i, bar,
               "##############################",
               (unsigned long long)bins[i], flag);
    }
}

int main(void) {
    uint32_t *addrs = malloc(N_OPS * sizeof(uint32_t));
    if (!addrs) { puts("malloc failed"); return 1; }
    gen_mixed(addrs, N_OPS);

    /* collect detach events into histogram */
    Histogram h;
    memset(&h, 0, sizeof(h));

    /* simulate pipeline — collect what would go to detach */
    uint32_t p=0, sb=0, pm=0, npm=0;
    uint64_t op=0;

    /* use L3 directly to get real anomaly signal */
    L3Engine l3;
    l3_init(&l3);

    for (uint64_t i=0; i<N_OPS; i++) {
        uint32_t addr = addrs[i];
        RouteTarget rt = l3_process(&l3, addr);

        /* build detach entry for anomalies */
        if (rt == ROUTE_SHADOW) {
            DetachEntry e;
            e.value        = addr;
            e.angular_addr = addr;
            e.reason       = DETACH_REASON_GEO_INVALID;
            e.route_was    = (uint8_t)rt;
            e.shell_n      = 0;
            e.phase18      = (uint8_t)(op % 18u);
            e.phase288     = (uint16_t)(op % 288u);
            e.phase306     = (uint16_t)(op % 306u);
            histo_add(&h, &e);
        } else if (l3.ghost_streak == 0 && l3.streak_resets > 0 &&
                   i > 0) {
            /* streak just reset = drift anomaly */
            DetachEntry e;
            e.value        = addr;
            e.angular_addr = addr;
            e.reason       = DETACH_REASON_GHOST_DRIFT;
            e.route_was    = (uint8_t)rt;
            e.shell_n      = 0;
            e.phase18      = (uint8_t)(op % 18u);
            e.phase288     = (uint16_t)(op % 288u);
            e.phase306     = (uint16_t)(op % 306u);
            histo_add(&h, &e);
        }
        op++;
        (void)p; (void)sb; (void)pm; (void)npm;
    }

    /* ── Results ─────────────────────────────────────────── */
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Twin Window Experiment  N=%lluM ops\n",
           (unsigned long long)N_OPS/1000000);
    printf("══════════════════════════════════════════════════════\n");
    printf("\n  Total anomalies:  %llu\n",  (unsigned long long)h.total);
    printf("  Geo invalid:      %llu\n",   (unsigned long long)h.geo_invalid);
    printf("  Ghost drift:      %llu\n\n", (unsigned long long)h.ghost_drift);

    if (h.total == 0) {
        printf("  No anomalies detected in this workload.\n");
        printf("  (mixed workload has low SHADOW rate — try chaos-only input)\n");
        free(addrs); return 0;
    }

    double twin_pct = h.twin_window * 100.0 / h.total;
    double expected_pct = (18.0*2 - 18.0) / 288.0 * 100.0; /* rough */
    printf("  In twin window (phase288<18 || phase306<18): %llu (%.1f%%)\n",
           (unsigned long long)h.twin_window, twin_pct);
    printf("  Outside twin window:                         %llu (%.1f%%)\n",
           (unsigned long long)h.non_twin,
           h.non_twin * 100.0 / h.total);
    printf("  Expected if uniform: ~%.1f%%\n", 18.0*100.0/288.0);

    if (twin_pct > 18.0 * 100.0 / 288.0 * 1.5)
        printf("\n  RESULT: SPIKE detected — twin window has %.1fx more anomalies\n"
               "          than uniform baseline. Hypothesis CONFIRMED.\n",
               twin_pct / (18.0*100.0/288.0));
    else
        printf("\n  RESULT: Distribution appears UNIFORM.\n"
               "          Hypothesis needs more evidence.\n");

    print_phase18_bar(&h);
    print_phase_window(&h, "phase288", h.phase288, BINS_288);

    printf("\n══════════════════════════════════════════════════════\n");
    free(addrs);
    return 0;
}
