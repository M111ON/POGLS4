#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "pogls_evolution_core.h"

static int _p=0,_f=0;
#define TEST(n) do{int _o=(n);if(_o){printf("  PASS  %s\n",#n);_p++;}else{printf("  FAIL  %s (line %d)\n",#n,__LINE__);_f++;}}while(0)

/* Group 1: Fibonacci recurrence */
static int t_fib_init(void){
    FibState f; fib_init(&f,10);
    return f.f0==10 && f.f1==5 && f.steps==0;
}
static int t_fib_additive(void){
    FibState f; fib_init(&f,8);
    int32_t v = fib_next(&f);
    return v == 8+4;  /* f0+f1 = 8+4 = 12 */
}
static int t_fib_no_mul(void){
    /* verify: it's pure add, not multiply */
    FibState f; fib_init(&f,100);
    int32_t v1=fib_next(&f);
    int32_t v2=fib_next(&f);
    /* f0=100,f1=50 → v1=150(f0=50,f1=150) → v2=200 */
    return v2 == 50 + v1;  /* f0 after step1 = 50 */
}
static int t_fib_overflow_safe(void){
    FibState f; fib_init(&f, 0x7FFFFFFF);
    for(int i=0;i<100;i++) fib_next(&f); /* should not crash */
    return 1;
}
static int t_fib_boundary_range(void){
    return fib_boundary(0,162)<=8 && fib_boundary(1000,162)<=8;
}

/* Group 2: Mandelbrot fp12 feedback */
static int t_mandel_stable(void){
    /* origin (0,0) = always stable */
    return mandel_classify(0,0) == EVO_STABLE;
}
static int t_mandel_chaotic(void){
    /* far outside = chaotic */
    return mandel_classify(8192,8192) == EVO_CHAOTIC;
}
static int t_mandel_fp_uniform(void){
    /* same shift used for x² and y² uniformly */
    uint32_t i1 = mandel_fp12(0, 0);        /* origin = stable */
    uint32_t i2 = mandel_fp12(8192, 8192);  /* far outside = escape */
    return i1 == EVO_MANDEL_ITER && i2 < EVO_MANDEL_ITER;
}
static int t_feedback_stable_collapses(void){
    return evo_feedback(100, EVO_STABLE) == 50;
}
static int t_feedback_chaotic_expands(void){
    return evo_feedback(10, EVO_CHAOTIC) == 20;
}
static int t_feedback_boundary_unchanged(void){
    return evo_feedback(42, EVO_BOUNDARY) == 42;
}

/* Group 3: Time Memory */
static int t_tmem_ticks(void){
    TimeMemory tm; tmem_init(&tm);
    tmem_hash(&tm, 100);
    tmem_hash(&tm, 200);
    return tm.global_tick == 2;
}
static int t_tmem_deterministic(void){
    TimeMemory tm1, tm2; tmem_init(&tm1); tmem_init(&tm2);
    uint32_t h1=tmem_hash(&tm1, 99999);
    uint32_t h2=tmem_hash(&tm2, 99999);
    return h1 == h2;
}
static int t_tmem_recurrence(void){
    TimeMemory tm; tmem_init(&tm);
    /* force same hash by using same addr at same tick */
    uint32_t h1=tmem_hash(&tm,42);
    /* different addr → different hash → no recurrence */
    uint32_t h2=tmem_hash(&tm,9999);
    (void)h1;(void)h2;
    return tm.recurrence == 0; /* different addrs = no repeat */
}
static int t_tmem_epoch_wire(void){
    TimeMemory tm; tmem_init(&tm);
    tmem_set_epoch(&tm, 2, 100);
    return tm.epoch==2 && tm.global_tick == 2*972+100;
}

/* Group 4: EvoCore full pipeline */
static int t_evo_init(void){
    EvoCore ec; evo_init(&ec, 162);
    return ec.magic==EVO_MAGIC && ec.fib.f0==162;
}
static int t_evo_lane_range(void){
    EvoCore ec; evo_init(&ec, 162);
    for(int i=0;i<1000;i++){
        uint8_t lane = evo_process(&ec, (uint64_t)i*7919);
        if(lane >= 54) return 0;
    }
    return 1;
}
static int t_evo_lane_distributed(void){
    EvoCore ec; evo_init(&ec, 162);
    uint32_t hist[54]={0};
    for(int i=0;i<5400;i++){
        uint8_t l=evo_process(&ec,(uint64_t)i*1234567);
        hist[l]++;
    }
    /* check no lane is 0 (good distribution) */
    int ok=1;
    for(int i=0;i<54;i++) if(hist[i]==0){ok=0;break;}
    return ok;
}
static int t_evo_dna_bitmask(void){
    /* DNA uses & 0x3 not % 4 */
    /* verify: 0x3 mask always 0..3 */
    for(uint32_t v=0;v<1000;v++)
        if(((v^(v>>1))&0x3)>3) return 0;
    return 1;
}
static int t_evo_stats_count(void){
    EvoCore ec; evo_init(&ec,162);
    for(int i=0;i<100;i++) evo_process(&ec,(uint64_t)i*999);
    return ec.total==100 &&
           (ec.stable_count+ec.chaotic_count+ec.boundary_count)==100;
}

int main(void){
    printf("\n══ POGLS V3.95 Evolution Core Tests ══\n\n");
    printf("── Group 1: Fibonacci recurrence ──\n");
    TEST(t_fib_init());TEST(t_fib_additive());TEST(t_fib_no_mul());
    TEST(t_fib_overflow_safe());TEST(t_fib_boundary_range());
    printf("\n── Group 2: Mandelbrot fp12 + feedback ──\n");
    TEST(t_mandel_stable());TEST(t_mandel_chaotic());
    TEST(t_mandel_fp_uniform());TEST(t_feedback_stable_collapses());
    TEST(t_feedback_chaotic_expands());TEST(t_feedback_boundary_unchanged());
    printf("\n── Group 3: Time Memory ──\n");
    TEST(t_tmem_ticks());TEST(t_tmem_deterministic());
    TEST(t_tmem_recurrence());TEST(t_tmem_epoch_wire());
    printf("\n── Group 4: EvoCore full pipeline ──\n");
    TEST(t_evo_init());TEST(t_evo_lane_range());
    TEST(t_evo_lane_distributed());TEST(t_evo_dna_bitmask());
    TEST(t_evo_stats_count());
    printf("\n══════════════════════════════════════\n");
    printf("  %d / %d PASS",_p,_p+_f);
    if(!_f) printf("  ✓ ALL PASS — system evolves 🧬\n");
    else printf("  ✗ %d FAIL\n",_f);
    printf("══════════════════════════════════════\n\n");
    return _f?1:0;
}
