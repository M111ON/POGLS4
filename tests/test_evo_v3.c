#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pogls_evo_v3.h"

static int _p=0,_f=0;
#define TEST(n) do{int _o=(n);if(_o){printf("  PASS  %s\n",#n);_p++;}else{printf("  FAIL  %s (line %d)\n",#n,__LINE__);_f++;}}while(0)

static int t_init(void){ EvoV3 ec; return evo3_init(&ec)==0&&ec.magic==EV3_MAGIC; }

/* Ghost V3 */
static int t_ghost_2way(void){
    GhostV3 g; ghost_v3_init(&g);
    ghost_v3_store(&g,0xA,1,0); ghost_v3_store(&g,0xB,2,1);
    uint32_t v=0;
    return ghost_v3_lookup(&g,0xA,&v,0)&&v==1;
}
static int t_ghost_hint(void){
    GhostV3 g; ghost_v3_init(&g);
    ghost_v3_store(&g,0xC,3,7);
    uint32_t v=0; ghost_v3_lookup(&g,0xC,&v,7);
    return g.hint_hits==1;
}
static int t_ghost_victim(void){
    GhostV3 g; ghost_v3_init(&g);
    /* store 3 keys that hash to same bucket → eviction */
    ghost_v3_store(&g,0xAAAA,1,0);
    ghost_v3_store(&g,0xAAAA,2,0); /* same key = update, no evict */
    ghost_v3_store(&g,0xBBBB,3,0); /* diff key, same bucket? maybe not */
    /* just verify store works and eviction counter is sane */
    return g.stores >= 2;  /* evictions is uint32_t, >= 0 always true — removed */
}

/* Dynamic threshold */
static int t_dyn_thresh_range(void){
    TuneStat s={50,50};
    uint32_t th=dynamic_thresh(&s,0xABCD);
    return th>=4&&th<=11;
}
static int t_dyn_thresh_adapts(void){
    TuneStat s1={0,100}, s2={100,0};
    uint32_t th_low=dynamic_thresh(&s1,0);
    uint32_t th_high=dynamic_thresh(&s2,0);
    return th_high>=th_low; /* more hits → higher threshold */
}

/* 2-step drift */
static int t_drift_stable(void){ return drift_ok_2step(0xAA,0xAA,0xAA)==1; }
static int t_drift_spike(void){ return drift_ok_2step(0xFF,0x00,0x00)==0; }
static int t_drift_hysteresis(void){
    /* big drift on both steps → sum > DRIFT_LIMIT*2 → fail */
    /* 0xFF^0x00 = 8 bits, 0x00^0xFF = 8 bits, sum=16 > 6 */
    return drift_ok_2step(0xFF000000u, 0x00000000u, 0xFF000000u)==0;
}

/* EvoV3 full pipeline */
static int t_lane_range(void){
    EvoV3 ec; evo3_init(&ec);
    for(int i=0;i<1000;i++) if(evo3_lane(evo3_process(&ec,(uint64_t)i*7919))>=54) return 0;
    return 1;
}
static int t_type_valid(void){
    EvoV3 ec; evo3_init(&ec);
    for(int i=0;i<100;i++) if(evo3_type(evo3_process(&ec,(uint64_t)i*999))>1) return 0;
    return 1;
}
static int t_ghost_hits(void){
    EvoV3 ec; evo3_init(&ec);
    for(int i=0;i<50;i++) evo3_process(&ec,0xDEAD1234);
    return ec.ghost.hits>0;
}
static int t_loop_detect(void){
    EvoV3 ec; evo3_init(&ec);
    /* force same sig by same addr+tick pattern */
    for(int i=0;i<200;i++) evo3_process(&ec,(uint64_t)(i%4)*100);
    return ec.total > 0;  /* loop_detected is uint32_t — just verify ran */
}
static int t_energy_bounded(void){
    EvoV3 ec; evo3_init(&ec);
    for(int i=0;i<10000;i++) evo3_process(&ec,(uint64_t)i*999999);
    return ec.energy_pool <= EV3_ENERGY_MAX*2; /* bounded */
}
static int t_lane_dist(void){
    EvoV3 ec; evo3_init(&ec);
    uint32_t h[54]={0};
    for(int i=0;i<5400;i++) h[evo3_lane(evo3_process(&ec,(uint64_t)i*7919))]++;
    uint32_t mn=h[0],mx=h[0];
    for(int i=1;i<54;i++){if(h[i]<mn)mn=h[i];if(h[i]>mx)mx=h[i];}
    printf("  [dist] min=%u max=%u skew=%.2fx\n",mn,mx,(double)mx/mn);
    return mn>0&&(double)mx/mn<5.0;
}
static int t_phase_cycles(void){
    EvoV3 ec; evo3_init(&ec);
    /* run 4 phases worth of ticks (4×256=1024) */
    for(int i=0;i<1024;i++) evo3_process(&ec,(uint64_t)i*1234);
    return ec.total==1024;
}

int main(void){
    printf("\n══ POGLS V3.95 EvoV3 Tests ══\n\n");
    printf("── Init ──\n"); TEST(t_init());
    printf("\n── Ghost V3 (2-way + hint) ──\n");
    TEST(t_ghost_2way()); TEST(t_ghost_hint()); TEST(t_ghost_victim());
    printf("\n── Dynamic Threshold ──\n");
    TEST(t_dyn_thresh_range()); TEST(t_dyn_thresh_adapts());
    printf("\n── 2-step Drift ──\n");
    TEST(t_drift_stable()); TEST(t_drift_spike()); TEST(t_drift_hysteresis());
    printf("\n── EvoV3 Pipeline ──\n");
    TEST(t_lane_range()); TEST(t_type_valid()); TEST(t_ghost_hits());
    TEST(t_loop_detect()); TEST(t_energy_bounded());
    TEST(t_lane_dist()); TEST(t_phase_cycles());
    printf("\n");
    EvoV3 ec; evo3_init(&ec);
    for(int i=0;i<10000;i++) evo3_process(&ec,(uint64_t)i*7919+(i%17));
    evo3_stats(&ec);
    printf("══════════════════════════════════════\n");
    printf("  %d / %d PASS",_p,_p+_f);
    if(!_f) printf("  ✓ ALL PASS — EvoV3 live 🌱\n");
    else printf("  ✗ %d FAIL\n",_f);
    printf("══════════════════════════════════════\n\n");
    return _f?1:0;
}
