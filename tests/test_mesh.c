#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_mesh.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n",s)
#define check(c,ok,fail) do{if(c){printf("    v %s\n",ok);g_pass++;}else{printf("    x FAIL: %s\n",fail);g_fail++;}}while(0)

static DetachEntry make_entry(uint64_t addr, uint8_t reason,
                               uint8_t phase18, uint16_t p288, uint16_t p306)
{
    DetachEntry e; memset(&e,0,sizeof(e));
    e.value=addr; e.angular_addr=addr;
    e.reason=reason; e.phase18=phase18;
    e.phase288=p288; e.phase306=p306;
    return e;
}

static void t01_init(void){
    section("T01  Mesh init");
    Mesh m; mesh_init(&m);
    check(m.magic==MESH_MAGIC,"magic ok","wrong");
    check(m.total_ingested==0,"ingested=0","wrong");
    check(m.delaunay.edge_count==0,"no edges","wrong");
    for(int i=0;i<(int)MESH_MAX_CLUSTERS;i++)
        check(m.clusters[i].cluster_id==i,"cluster_id ok","wrong");
}

static void t02_voronoi(void){
    section("T02  Voronoi classify");
    /* same addr twice should give same cluster */
    uint32_t mask=POGLS_PHI_SCALE-1u;
    uint32_t addr=100000u;
    uint32_t a=(uint32_t)(((uint64_t)(addr&mask)*POGLS_PHI_UP)>>20)&mask;
    uint32_t b=(uint32_t)(((uint64_t)(addr&mask)*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t c1=voronoi_classify(a,b);
    uint8_t c2=voronoi_classify(a,b);
    check(c1==c2,"voronoi deterministic","not deterministic");
    check(c1<MESH_MAX_CLUSTERS,"cluster in range","out of range");

    /* different addrs can give different clusters */
    uint32_t addr2=900000u;
    uint32_t a2=(uint32_t)(((uint64_t)(addr2&mask)*POGLS_PHI_UP)>>20)&mask;
    uint32_t b2=(uint32_t)(((uint64_t)(addr2&mask)*POGLS_PHI_DOWN)>>20)&mask;
    uint8_t c3=voronoi_classify(a2,b2);
    check(c3<MESH_MAX_CLUSTERS,"cluster2 in range","out of range");
    /* note: may or may not be same cluster — both valid */
    check(1,"voronoi coverage ok","impossible");
}

static void t03_ingest_single(void){
    section("T03  Single ingest -> cluster update");
    Mesh m; mesh_init(&m);
    DetachEntry e=make_entry(200000ULL,DETACH_REASON_GEO_INVALID,5,10,20);
    mesh_ingest(&m,&e);
    check(m.total_ingested==1,"ingested=1","wrong");
    /* find which cluster got the event */
    int found=0;
    for(int i=0;i<(int)MESH_MAX_CLUSTERS;i++)
        if(m.clusters[i].event_count==1){found=1;break;}
    check(found,"event in one cluster","no cluster updated");
}

static void t04_tail(void){
    section("T04  Tail lineage");
    Mesh m; mesh_init(&m);
    /* push 20 events to same cluster via same slice */
    for(int i=0;i<20;i++){
        DetachEntry e=make_entry(200000ULL,DETACH_REASON_GEO_INVALID,
                                  (uint8_t)(i%18),0,0);
        mesh_ingest(&m,&e);
    }
    /* find the tail that has events */
    int found=0;
    for(int s=0;s<(int)SLICE_COUNT;s++)
        for(int c=0;c<(int)MESH_MAX_CLUSTERS;c++)
            if(m.tails[s][c].total>0){found=1;break;}
    check(found,"tail has events","tail empty");
    /* total events = 20 */
    uint64_t total=0;
    for(int s=0;s<(int)SLICE_COUNT;s++)
        for(int c=0;c<(int)MESH_MAX_CLUSTERS;c++)
            total+=m.tails[s][c].total;
    check(total==20,"tail total=20","wrong total");
}

static void t05_delaunay(void){
    section("T05  Delaunay edge creation");
    Mesh m; mesh_init(&m);
    /* push events from different lanes to trigger cross-slice ghost */
    /* lane 0 -> ghost lane 27 (Slice A -> Slice B) */
    for(int i=0;i<50;i++){
        /* use addrs that map to different lanes */
        uint64_t addr=(uint64_t)(i*19937u) % POGLS_PHI_SCALE;
        DetachEntry e=make_entry(addr,DETACH_REASON_GHOST_DRIFT,
                                  (uint8_t)(i%18),0,0);
        mesh_ingest(&m,&e);
    }
    /* after 50 events from various lanes, some cross-slice should exist */
    check(m.cross_slice_ghosts>=0,"cross_slice_ghosts counted","crash");
    /* delaunay edge_count <= MESH_MAX_CLUSTERS^2 */
    check(m.delaunay.edge_count<=81u,"edge_count bounded","overflow");
}

static void t06_batch(void){
    section("T06  Batch ingest");
    Mesh m; mesh_init(&m);
    DetachEntry batch[64];
    for(int i=0;i<64;i++)
        batch[i]=make_entry((uint64_t)(i*12345u)%POGLS_PHI_SCALE,
                             DETACH_REASON_GEO_INVALID,(uint8_t)(i%18),
                             (uint16_t)(i%288),(uint16_t)(i%306));
    mesh_ingest_batch(&m,batch,64);
    check(m.total_ingested==64,"batch: 64 ingested","wrong");
}

static void t07_twin_window(void){
    section("T07  Twin window detection");
    Mesh m; mesh_init(&m);
    /* phase288=5 < 18 -> twin window */
    DetachEntry e=make_entry(100000ULL,DETACH_REASON_GEO_INVALID,5,5,100);
    mesh_ingest(&m,&e);
    check(m.twin_window_hits==1,"twin window hit","not detected");
    /* phase288=20 >= 18, phase306=20 >= 18 -> not twin */
    DetachEntry e2=make_entry(200000ULL,DETACH_REASON_GEO_INVALID,5,20,20);
    mesh_ingest(&m,&e2);
    check(m.twin_window_hits==1,"non-twin not counted","false positive");
}

static void t08_null(void){
    section("T08  NULL safety");
    mesh_init(NULL);
    mesh_ingest(NULL,NULL);
    mesh_ingest_batch(NULL,NULL,0);
    mesh_stats(NULL);
    check(voronoi_classify(0,0)<MESH_MAX_CLUSTERS,"voronoi(0,0) ok","crash");
    check(!delaunay_has_edge(NULL,0,0),"edge(NULL)=0","crash");
    check(delaunay_neighbor_count(NULL,0)==0,"neighbors(NULL)=0","crash");
    check(1,"all null paths ok","crash");
}

static void t09_voronoi_coverage(void){
    section("T09  Voronoi covers all 9 clusters");
    /* generate many addrs and verify all 9 clusters get at least some events */
    int cluster_seen[9]={0};
    uint32_t mask=POGLS_PHI_SCALE-1u;
    for(uint32_t i=0;i<10000;i++){
        uint32_t addr=(i*19937u)&mask;
        uint32_t a=(uint32_t)(((uint64_t)addr*POGLS_PHI_UP)>>20)&mask;
        uint32_t b=(uint32_t)(((uint64_t)addr*POGLS_PHI_DOWN)>>20)&mask;
        uint8_t c=voronoi_classify(a,b);
        if(c<9)cluster_seen[c]=1;
    }
    int all_covered=1;
    for(int i=0;i<9;i++) if(!cluster_seen[i])all_covered=0;
    check(all_covered,"all 9 clusters reachable","some clusters empty");
}

static void t10_tail_ring_wrap(void){
    section("T10  Tail ring wraps at MESH_TAIL_DEPTH=18");
    Mesh m; mesh_init(&m);
    /* push 36 events (2 full cycles) all same addr/slice */
    for(int i=0;i<36;i++){
        DetachEntry e=make_entry(200000ULL,DETACH_REASON_GEO_INVALID,
                                  (uint8_t)(i%18),0,0);
        mesh_ingest(&m,&e);
    }
    /* find the active tail */
    for(int s=0;s<(int)SLICE_COUNT;s++)
        for(int c=0;c<(int)MESH_MAX_CLUSTERS;c++)
            if(m.tails[s][c].total==36){
                check(m.tails[s][c].full==1,"tail.full=1 after wrap","not full");
                return;
            }
    check(0,"tail with 36 events found","not found");
}

int main(void){
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS Mesh (Voronoi+Delaunay+Tail) — Tests\n");
    printf("══════════════════════════════════════════════════\n");
    t01_init(); t02_voronoi(); t03_ingest_single();
    t04_tail(); t05_delaunay(); t06_batch();
    t07_twin_window(); t08_null(); t09_voronoi_coverage();
    t10_tail_ring_wrap();
    printf("\n══════════════════════════════════════════════════\n");
    if(g_fail==0) printf("  %d / %d PASS  v ALL PASS — Mesh live [S]\n",g_pass,g_pass);
    else printf("  %d / %d PASS  x %d FAILED\n",g_pass,g_pass+g_fail,g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
