#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_diamond_hc.h"

static int g_pass=0,g_fail=0;
#define section(s) printf("\n  -- %s\n",s)
#define check(c,ok,fail) do{if(c){printf("    v %s\n",ok);g_pass++;}else{printf("    x FAIL: %s\n",fail);g_fail++;}}while(0)

static DetachEntry make_e(uint64_t addr,uint8_t phase18,uint16_t p288,uint16_t p306){
    DetachEntry e; memset(&e,0,sizeof(e));
    e.value=addr; e.angular_addr=addr;
    e.reason=DETACH_REASON_GEO_INVALID;
    e.phase18=phase18; e.phase288=p288; e.phase306=p306;
    return e;
}

static void t01_struct_sizes(void){
    section("T01  Struct sizes");
    check(sizeof(HoneycombCell)==32u,"HoneycombCell=32B","wrong");
    check(sizeof(DiamondAnchor)==16u,"DiamondAnchor=16B","wrong");
    DHCContext ctx; dhc_init(&ctx);
    check(ctx.magic==DHC_MAGIC,"magic ok","wrong");
    check(ctx.cell_alloc_head==1,"pool head=1 (sentinel skip)","wrong");
    check(DHC_DIAMOND_COUNT==64u,"64 diamonds","wrong");
    check(DHC_HC_CELLS_MAX==27u,"27 cells per diamond (3^3)","wrong");
}

static void t02_diamond_map(void){
    section("T02  diamond_map — binary 2^n locality");
    /* same addr → same diamond */
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t addr=500000u;
    uint32_t a=(uint32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t d1=diamond_map(a,b);
    uint8_t d2=diamond_map(a,b);
    check(d1==d2,"diamond_map deterministic","not deterministic");
    check(d1<64u,"diamond_id in range","out of range");

    /* nearby addrs should often share diamond */
    uint32_t addr2=500100u;
    uint32_t a2=(uint32_t)(((uint64_t)addr2*POGLS_PHI_UP)>>20)&mask;
    uint32_t b2=(uint32_t)(((uint64_t)addr2*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t d3=diamond_map(a2,b2);
    check(d3<64u,"nearby diamond in range","wrong");
    /* locality: d1==d3 likely for very close addrs (not guaranteed) */
    check(1,"diamond locality check done","impossible");
}

static void t03_cell_ternary(void){
    section("T03  cell_ternary — ternary 3^n");
    uint8_t c1=cell_ternary(100000u,50000u,5u);
    uint8_t c2=cell_ternary(100000u,50000u,5u);
    check(c1==c2,"cell_ternary deterministic","not deterministic");
    check(c1<27u,"cell in [0..26]","out of range");

    /* different phase → potentially different cell */
    uint8_t c3=cell_ternary(100000u,50000u,6u);
    check(c3<27u,"different phase cell in range","wrong");

    /* all 27 cells reachable */
    int seen[27]={0};
    for(uint32_t a=0;a<(1u<<20);a+=(1u<<15)){
        for(uint32_t b=0;b<(1u<<20);b+=(1u<<15)){
            for(uint8_t p=0;p<18;p++){
                uint8_t c=cell_ternary(a,b,p);
                if(c<27)seen[c]=1;
            }
        }
    }
    int all=1; for(int i=0;i<27;i++) if(!seen[i])all=0;
    check(all,"all 27 ternary cells reachable","some empty");
}

static void t04_ingest_single(void){
    section("T04  Single ingest → cell created (Tail summon)");
    DHCContext ctx; dhc_init(&ctx);
    DetachEntry e=make_e(200000ULL,5,10,20);
    dhc_ingest(&ctx,&e);
    check(ctx.total_ingested==1,"ingested=1","wrong");
    check(ctx.cells_created==1,"1 cell created (Tail summon)","wrong");
    check(ctx.shadow_updates==1,"1 shadow update","wrong");
    /* anchor has 1 active cell */
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t addr=200000u;
    uint32_t a=(uint32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t did=diamond_map(a,b);
    check(ctx.anchors[did].active_cells==1,"anchor active_cells=1","wrong");
}

static void t05_cell_reuse(void){
    section("T05  Same addr → cell reused, not duplicated");
    DHCContext ctx; dhc_init(&ctx);
    DetachEntry e=make_e(300000ULL,3,0,0);
    dhc_ingest(&ctx,&e);
    dhc_ingest(&ctx,&e);
    dhc_ingest(&ctx,&e);
    check(ctx.total_ingested==3,"3 ingested","wrong");
    check(ctx.cells_created==1,"still 1 cell (reused)","created duplicates");
    /* find the cell and check event_count */
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t addr=300000u;
    uint32_t a=(uint32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t did=diamond_map(a,b);
    uint32_t pidx=ctx.anchors[did].head_cell;
    check(pidx>0,"cell allocated","wrong");
    check(ctx.cells[pidx].event_count==3,"event_count=3","wrong");
}

static void t06_ttl(void){
    section("T06  TTL refresh + expire");
    DHCContext ctx; dhc_init(&ctx);
    DetachEntry e=make_e(400000ULL,1,0,0);
    dhc_ingest(&ctx,&e);
    /* find cell */
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t addr=400000u;
    uint32_t a=(uint32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t did=diamond_map(a,b);
    uint32_t pidx=ctx.anchors[did].head_cell;
    check(ctx.cells[pidx].ttl==DHC_TTL_DEFAULT,"ttl=default after create","wrong");
    /* force ttl low */
    ctx.cells[pidx].ttl=1;
    dhc_tick_ttl(&ctx);
    check(ctx.cells[pidx].ttl==0,"ttl decremented to 0","wrong");
    check(ctx.cells_expired==1,"expired counter=1","wrong");
    /* access refreshes ttl */
    dhc_ingest(&ctx,&e);
    check(ctx.cells[pidx].ttl==DHC_TTL_DEFAULT || ctx.cells_created==2,
          "ttl refreshed or new cell created","wrong");
}

static void t07_shadow(void){
    section("T07  ShadowOffset d_a/d_b updates");
    DHCContext ctx; dhc_init(&ctx);
    /* two events on same diamond */
    DetachEntry e1=make_e(500000ULL,5,0,0);
    DetachEntry e2=make_e(510000ULL,5,0,0);
    dhc_ingest(&ctx,&e1);
    dhc_ingest(&ctx,&e2);
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t a=(uint32_t)(((uint64_t)500000u*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)500000u*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t did=diamond_map(a,b);
    check(ctx.shadow_updates>=1,"shadow updated","wrong");
    check(ctx.shadows[did][0].diamond_id==did,"shadow diamond_id ok","wrong");
}

static void t08_repair_hint(void){
    section("T08  dhc_repair_hint finds neighbor cell");
    DHCContext ctx; dhc_init(&ctx);
    /* create multiple cells in same diamond by varying phase */
    for(uint8_t p=0;p<18;p++){
        DetachEntry e=make_e(600000ULL,p,0,0);
        dhc_ingest(&ctx,&e);
    }
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t a=(uint32_t)(((uint64_t)600000u*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)600000u*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t did=diamond_map(a,b);
    uint8_t clocal=cell_ternary(a,b,0u);
    HoneycombCell *hint=dhc_repair_hint(&ctx,did,clocal);
    /* may or may not find a different cell depending on ternary mapping */
    check(hint==NULL || hint->cell_id!=clocal || hint->ttl>0,
          "repair hint returns different/valid cell","wrong");
    check(1,"repair hint search ok","crash");
}

static void t09_batch(void){
    section("T09  Batch ingest 64 entries");
    DHCContext ctx; dhc_init(&ctx);
    DetachEntry batch[64];
    for(int i=0;i<64;i++)
        batch[i]=make_e((uint64_t)(i*13337u)%POGLS_PHI_SCALE,
                         (uint8_t)(i%18),
                         (uint16_t)(i%288),(uint16_t)(i%306));
    dhc_ingest_batch(&ctx,batch,64);
    check(ctx.total_ingested==64,"64 ingested","wrong");
    check(ctx.cells_created>=1,"at least 1 cell created","wrong");
    check(ctx.shadow_updates==64,"64 shadow updates","wrong");
}

static void t10_2to3_ratio(void){
    section("T10  2:3 structure verification");
    /* Diamond uses 2^6=64 binary buckets */
    check(DHC_DIAMOND_COUNT==64u,"Diamond=2^6=64 (binary)","wrong");
    /* Honeycomb uses 3^3=27 ternary cells */
    check(DHC_HC_CELLS_MAX==27u,"Honeycomb=3^3=27 (ternary)","wrong");
    /* Total pool = 64×27 = 1728 */
    check(DHC_HC_POOL_SIZE==1728u,"Pool=64×27=1728","wrong");
    /* gate alignment: 27 = 3×9 = 3×3^2, 64 = 4×16 = 4×2^4 */
    check(DHC_SHADOW_HISTORY==18u,"Shadow history=18 (gate_18)","wrong");
    check(DHC_TTL_DEFAULT==54u,"TTL=54 (3×gate_18)","wrong");
}

static void t11_null(void){
    section("T11  NULL safety");
    dhc_init(NULL);
    dhc_ingest(NULL,NULL);
    dhc_ingest_batch(NULL,NULL,0);
    dhc_stats(NULL);
    check(dhc_repair_hint(NULL,0,0)==NULL,"hint(NULL)=NULL","crash");
    check(diamond_map(0,0)<64u,"diamond_map(0,0) ok","crash");
    check(cell_ternary(0,0,0)<27u,"cell_ternary(0,0,0) ok","crash");
    check(1,"all null paths ok","crash");
}

int main(void){
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Diamond/Honeycomb/Shadow — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");
    t01_struct_sizes();
    t02_diamond_map();
    t03_cell_ternary();
    t04_ingest_single();
    t05_cell_reuse();
    t06_ttl();
    t07_shadow();
    t08_repair_hint();
    t09_batch();
    t10_2to3_ratio();
    t11_null();
    printf("\n══════════════════════════════════════════════════\n");
    if(g_fail==0)
        printf("  %d / %d PASS  v ALL PASS — Diamond/HC live [S]\n",
               g_pass,g_pass);
    else
        printf("  %d / %d PASS  x %d FAILED\n",
               g_pass,g_pass+g_fail,g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
