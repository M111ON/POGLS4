/*
 * test_diamond_v2.c — Diamond Layer v1.2 full test suite
 * Expected: ALL PASS
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_diamond_layer.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n", s)
#define check(c,ok,fail) do{ \
    if(c){printf("    v %s\n",ok);g_pass++;} \
    else {printf("    x FAIL: %s (line %d)\n",fail,__LINE__);g_fail++;} \
}while(0)

static MeshEntry me(uint64_t addr, uint8_t type, int16_t delta) {
    MeshEntry m; memset(&m,0,sizeof(m));
    m.addr=addr; m.type=type; m.delta=delta;
    return m;
}

/* ── Group 1: baseline ───────────────────────────────────────── */

static void t01_id_range(void) {
    section("T01  diamond_id in [0..63]");
    int bad=0;
    for(int32_t a=0;a<(1<<20);a+=4096)
        for(int32_t b=0;b<(1<<20);b+=4096)
            if(diamond_id(a,b,0)>=DIAMOND_COUNT) bad++;
    check(bad==0,"all ids in [0..63]","out of range");
}

static void t02_distribution(void) {
    section("T02  Distribution 64/64, skew < 5x");
    uint32_t hist[64]={0};
    for(uint32_t addr=0;addr<(1u<<20);addr+=128) {
        uint32_t mask=(1u<<20)-1u;
        int32_t a=(int32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&(int32_t)mask;
        int32_t b=(int32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&(int32_t)mask;
        hist[diamond_id(a,b,0)]++;
    }
    uint32_t filled=0,mn=~0u,mx=0;
    for(int i=0;i<64;i++){if(hist[i]>0)filled++;if(hist[i]<mn)mn=hist[i];if(hist[i]>mx)mx=hist[i];}
    double skew=(mn>0)?(double)mx/mn:999.0;
    printf("    (filled %u/64, skew %.2fx)\n",filled,skew);
    check(filled==64,"all 64 reachable","empty");
    check(skew<5.0,"skew<5x","skewed");
}

static void t03_cell_size(void) {
    section("T03  DiamondCell=6B (added last_type)");
    check(sizeof(DiamondCell)==6u,"DiamondCell=6B","wrong");
}

static void t04_init(void) {
    section("T04  Init: zeroed + last_type=0xFF");
    DiamondLayer dl; diamond_init(&dl);
    check(dl.magic==DIAMOND_MAGIC,"magic ok","wrong");
    int ok=1;
    for(int i=0;i<64;i++)
        if(dl.cells[i].bias||dl.cells[i].heat||
           dl.cells[i].count||dl.cells[i].last_type!=0xFFu) ok=0;
    check(ok,"all cells clean","wrong");
}

/* ── Group 2: [FIX-1] clamp ±32 ─────────────────────────────── */

static void t05_clamp_pos(void) {
    section("T05  [FIX-1] bias clamps at +32");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    for(int i=0;i<2000;i++) diamond_update(&dl,0,&s);
    int8_t b=diamond_bias(&dl,0);
    check(b<=32,"bias<=+32","exceeded");
    check(b>0,"still positive","wrong");
    printf("    (converged: %d)\n",(int)b);
}

static void t06_clamp_neg(void) {
    section("T06  [FIX-1] bias clamps at -32");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    for(int i=0;i<2000;i++) diamond_update(&dl,1,&g);
    int8_t b=diamond_bias(&dl,1);
    check(b>=-32,"bias>=-32","exceeded");
    check(b<0,"still negative","wrong");
    printf("    (converged: %d)\n",(int)b);
}

static void t07_clamp_pulls_down(void) {
    section("T07  [FIX-1] clamp pulls pre-set high bias down");
    DiamondLayer dl; diamond_init(&dl);
    dl.cells[2].bias=60;
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    diamond_update(&dl,2,&s);
    check(dl.cells[2].bias<=32,"bias pulled to <=32","wrong");
}

/* ── Group 3: [FIX-2] heat → route boost ───────────────────── */

static void t08_heat_cold(void) {
    section("T08  [FIX-2] heat boost=0 when cold");
    DiamondLayer dl; diamond_init(&dl);
    check(diamond_heat_boost(&dl,0)==0,"cold: no boost","wrong");
}

static void t09_heat_hot(void) {
    section("T09  [FIX-2] heat boost=1 at/above threshold");
    DiamondLayer dl; diamond_init(&dl);
    dl.cells[3].heat = DIAMOND_HEAT_BOOST_THRESH;       /* exactly at = boost */
    check(diamond_heat_boost(&dl,3)==1,"heat=threshold: boost=1","wrong");
    dl.cells[3].heat = DIAMOND_HEAT_BOOST_THRESH + 1;   /* above = boost */
    check(diamond_heat_boost(&dl,3)==1,"heat=threshold+1: boost=1","wrong");
    dl.cells[3].heat = DIAMOND_HEAT_BOOST_THRESH - 1;   /* below = no boost */
    check(diamond_heat_boost(&dl,3)==0,"heat=threshold-1: no boost","wrong");
}

static void t10_heat_in_signal(void) {
    section("T10  [FIX-2] route_signal adds heat boost");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    for(int i=0;i<64;i++) diamond_update(&dl,5,&s);
    int8_t score; int boost;
    int total=diamond_route_signal(&dl,5,&score,&boost);
    int8_t raw=diamond_score(&dl,5);
    check(boost==1,"hot cell: boost=1","wrong");
    check(total==(int)raw+1,"total=score+boost","wrong");
}

/* ── Group 4: [FIX-3] cold start filter ──────────────────────── */

static void t11_cold_returns_zero(void) {
    section("T11  [FIX-3] count<8 → score=0");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    for(int i=0;i<3;i++) diamond_update(&dl,6,&g);
    check(dl.cells[6].count==3,"count=3","wrong");
    check(dl.cells[6].bias<0,"raw bias<0","wrong");
    check(diamond_score(&dl,6)==0,"score=0 (cold)","not filtered");
}

static void t12_warm_returns_bias(void) {
    section("T12  [FIX-3] count>=8 → score=real bias");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    for(int i=0;i<10;i++) diamond_update(&dl,7,&g);
    check(dl.cells[7].count>=8,"count>=8","wrong");
    check(diamond_score(&dl,7)==diamond_bias(&dl,7),"score==bias","mismatch");
}

static void t13_demote_cold_gate(void) {
    section("T13  [FIX-3] no demote when cold, demote when warm");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    for(int i=0;i<5;i++) diamond_update(&dl,8,&g);
    check(!diamond_should_demote(&dl,8),"cold: no demote","false demotion");
    for(int i=0;i<5;i++) diamond_update(&dl,8,&g); // total=10 >= 8
    check(diamond_should_demote(&dl,8),"warm+ghost: demote","not demoting");
}

/* ── Group 5: [FIX-4] last_type pattern ─────────────────────── */

static void t14_last_type_init(void) {
    section("T14  [FIX-4] last_type=0xFF on init");
    DiamondLayer dl; diamond_init(&dl);
    check(dl.cells[9].last_type==0xFFu,"init: 0xFF","wrong");
}

static void t15_last_type_set(void) {
    section("T15  [FIX-4] last_type updated after each event");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    diamond_update(&dl,9,&s);
    check(dl.cells[9].last_type==MESH_TYPE_SEQ,"after SEQ: last_type=SEQ","wrong");
    diamond_update(&dl,9,&g);
    check(dl.cells[9].last_type==MESH_TYPE_GHOST,"after GHOST: last_type=GHOST","wrong");
}

static void t16_reinforce_same(void) {
    section("T16  [FIX-4] same type streak → reinforce");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    diamond_update(&dl,10,&s);
    check(dl.pattern_reinforces==0,"no reinforce on first","wrong");
    diamond_update(&dl,10,&s);
    check(dl.pattern_reinforces==1,"reinforce on 2nd same-type","wrong");
    diamond_update(&dl,10,&s);
    check(dl.pattern_reinforces==2,"reinforce on 3rd same-type","wrong");
}

static void t17_no_reinforce_diff(void) {
    section("T17  [FIX-4] different type = no reinforce");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    diamond_update(&dl,11,&s);
    diamond_update(&dl,11,&g);
    check(dl.pattern_reinforces==0,"no reinforce on type change","wrong");
    diamond_update(&dl,11,&g);
    check(dl.pattern_reinforces==1,"reinforce on GHOST streak","wrong");
}

static void t18_streak_boosts_bias(void) {
    section("T18  [FIX-4] SEQ streak converges higher than alternating");
    DiamondLayer dl_alt, dl_streak;
    diamond_init(&dl_alt); diamond_init(&dl_streak);
    MeshEntry s=me(0,MESH_TYPE_SEQ,0);
    MeshEntry g=me(0,MESH_TYPE_GHOST,0);
    for(int i=0;i<20;i++) {
        diamond_update(&dl_alt,12,(i%2==0)?&s:&g);
        diamond_update(&dl_streak,12,&s);
    }
    int8_t alt_b=diamond_bias(&dl_alt,12);
    int8_t str_b=diamond_bias(&dl_streak,12);
    check(str_b>alt_b,"streak bias > alternating bias","wrong");
    printf("    (alt=%d streak=%d reinforce=%llu)\n",
           (int)alt_b,(int)str_b,(unsigned long long)dl_streak.pattern_reinforces);
}

/* ── Group 6: integration ────────────────────────────────────── */

static void t19_ghost_kills_fast(void) {
    section("T19  GHOST zone demotes within 20 events");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry g=me(0x5000ULL,MESH_TYPE_GHOST,-50);
    int at=-1;
    for(int i=0;i<50&&at<0;i++) {
        diamond_update(&dl,13,&g);
        if(diamond_should_demote(&dl,13)) at=i+1;
    }
    check(at>0,"GHOST triggers demotion","never");
    check(at<=20,"demotion within 20 events","too slow");
    printf("    (demoted at event %d)\n",at);
}

static void t20_seq_gets_boost(void) {
    section("T20  SEQ zone: positive signal after warmup");
    DiamondLayer dl; diamond_init(&dl);
    MeshEntry s=me(0x6000ULL,MESH_TYPE_SEQ,20);
    for(int i=0;i<30;i++) diamond_update(&dl,14,&s);
    int total=diamond_route_signal(&dl,14,NULL,NULL);
    check(total>0,"SEQ zone: positive route signal","wrong");
}

static void t21_combined_page_cluster(void) {
    section("T21  Page + cluster additive signal");
    ReflexBias rb; reflex_init(&rb);
    DiamondLayer dl; diamond_init(&dl);
    uint64_t bad=0x7000ULL;
    MeshEntry g=me(bad,MESH_TYPE_GHOST,-30);
    for(int i=0;i<15;i++) {
        reflex_update(&rb,&g);
        diamond_process(&dl,&g);
    }
    int8_t page=reflex_lookup(&rb,bad);
    uint32_t mask=(1u<<20)-1u;
    uint32_t addr20=(uint32_t)(bad&mask);
    int32_t a=(int32_t)(((uint64_t)addr20*POGLS_PHI_UP)>>20)&(int32_t)mask;
    int32_t b=(int32_t)(((uint64_t)addr20*POGLS_PHI_DOWN)>>20)&(int32_t)mask;
    uint32_t did=diamond_id(a,b,g.delta);
    int cluster=diamond_route_signal(&dl,did,NULL,NULL);
    int combined=(int)page+cluster;
    check(page<0,"page negative","wrong");
    check(cluster<0,"cluster negative","wrong");
    check(combined<(int)page,"combined < page alone","no additive");
    printf("    (page=%d cluster=%d combined=%d)\n",(int)page,cluster,combined);
}

static void t22_null_safety(void) {
    section("T22  NULL safety");
    diamond_init(NULL);
    diamond_update(NULL,0,NULL);
    check(diamond_bias(NULL,0)==0,"bias(NULL)=0","crash");
    check(diamond_score(NULL,0)==0,"score(NULL)=0","crash");
    check(diamond_heat_boost(NULL,0)==0,"boost(NULL)=0","crash");
    check(diamond_route_signal(NULL,0,NULL,NULL)==0,"signal(NULL)=0","crash");
    check(diamond_process(NULL,NULL)==0,"process(NULL)=0","crash");
    check(!diamond_should_demote(NULL,0),"demote(NULL)=0","crash");
    check(!diamond_should_boost(NULL,0),"boost(NULL)=0","crash");
    check(1,"all NULL survived","crash");
}

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  Diamond Layer v1.2 — Test Suite\n");
    printf("  FIX-1:clamp  FIX-2:heat  FIX-3:cold  FIX-4:pattern\n");
    printf("══════════════════════════════════════════════════\n");
    printf("\n=== Group 1: baseline ===\n");
    t01_id_range(); t02_distribution(); t03_cell_size(); t04_init();
    printf("\n=== Group 2: [FIX-1] clamp ±32 ===\n");
    t05_clamp_pos(); t06_clamp_neg(); t07_clamp_pulls_down();
    printf("\n=== Group 3: [FIX-2] heat boost ===\n");
    t08_heat_cold(); t09_heat_hot(); t10_heat_in_signal();
    printf("\n=== Group 4: [FIX-3] cold start ===\n");
    t11_cold_returns_zero(); t12_warm_returns_bias(); t13_demote_cold_gate();
    printf("\n=== Group 5: [FIX-4] pattern ===\n");
    t14_last_type_init(); t15_last_type_set();
    t16_reinforce_same(); t17_no_reinforce_diff(); t18_streak_boosts_bias();
    printf("\n=== Group 6: integration ===\n");
    t19_ghost_kills_fast(); t20_seq_gets_boost();
    t21_combined_page_cluster(); t22_null_safety();

    DiamondLayer dl; diamond_init(&dl);
    MeshEntry demo[4]={me(0,MESH_TYPE_GHOST,-50),me(0,MESH_TYPE_SEQ,20),
                       me(0,MESH_TYPE_BURST,0),me(0,MESH_TYPE_ANOMALY,-10)};
    for(uint32_t addr=0;addr<(1u<<20);addr+=256){
        demo[(addr/256)%4].addr=(uint64_t)addr;
        diamond_process(&dl,&demo[(addr/256)%4]);
    }
    diamond_stats(&dl);

    printf("\n══════════════════════════════════════════════════\n");
    if(g_fail==0)
        printf("  %d / %d PASS  v ALL PASS — Diamond v1.2 live ♦\n",g_pass,g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",g_pass,g_pass+g_fail,g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
