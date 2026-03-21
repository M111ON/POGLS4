#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_engine_slice.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n",s)
#define check(c,ok,fail) do{ if(c){printf("    v %s\n",ok);g_pass++;}else{printf("    x FAIL: %s\n",fail);g_fail++;}}while(0)

static void t01_init(void){
    section("T01  Slice init + size");
    check(sizeof(EngineSlice)==24u,"EngineSlice=24B","size wrong");
    EngineSliceSet ss; slice_set_init(&ss);
    for(int i=0;i<3;i++){
        EngineSlice *s=&ss.slices[i];
        check(s->engine_id==i,"engine_id ok","wrong");
        check(s->lane_start==i*18,"lane_start ok","wrong");
        check(s->lane_count==18,"lane_count=18","wrong");
        check(s->node_start==i*54,"node_start ok","wrong");
        check(s->node_count==54,"node_count=54","wrong");
        check(s->active==1,"active=1","wrong");
        check(s->magic==SLICE_MAGIC,"magic ok","wrong");
    }
}

static void t02_lane_owner(void){
    section("T02  Lane ownership");
    check(slice_owner_of_lane(0)==0,"lane 0 -> A","wrong");
    check(slice_owner_of_lane(17)==0,"lane 17 -> A","wrong");
    check(slice_owner_of_lane(18)==1,"lane 18 -> B","wrong");
    check(slice_owner_of_lane(35)==1,"lane 35 -> B","wrong");
    check(slice_owner_of_lane(36)==2,"lane 36 -> C","wrong");
    check(slice_owner_of_lane(53)==2,"lane 53 -> C","wrong");
}

static void t03_ghost_cross(void){
    section("T03  Ghost cross-slice (100% cross)");
    int all_cross=1;
    for(int lane=0;lane<54;lane++){
        uint8_t src=slice_owner_of_lane((uint8_t)lane);
        uint8_t dst=slice_ghost_dst((uint8_t)lane);
        if(src==dst){all_cross=0; printf("    ! lane %d stays in slice %d\n",lane,src);}
    }
    check(all_cross,"100% ghost lanes cross slice","some stayed");

    /* K3: each slice connects to both others */
    int a_to_b=0,a_to_c=0;
    for(int lane=0;lane<18;lane++){
        uint8_t dst=slice_ghost_dst((uint8_t)lane);
        if(dst==1)a_to_b++;
        if(dst==2)a_to_c++;
    }
    check(a_to_b==9,"A->B: 9 lanes","wrong");
    check(a_to_c==9,"A->C: 9 lanes","wrong");
}

static void t04_hop_guard(void){
    section("T04  Hop guard");
    EngineSlice s; slice_init(&s,0);
    check(slice_hop_ok(&s,0),"hop 0 ok","wrong");
    check(slice_hop_ok(&s,1),"hop 1 ok","wrong");
    check(!slice_hop_ok(&s,2),"hop 2 blocked","wrong");
    check(!slice_hop_ok(&s,99),"hop 99 blocked","wrong");
}

static void t05_ghost_tag(void){
    section("T05  Ghost tag pack/unpack");
    uint16_t tag=slice_ghost_tag(2,1);
    check(slice_tag_origin(tag)==2,"origin=2","wrong");
    check(slice_tag_hops(tag)==1,"hops=1","wrong");
    tag=slice_ghost_tag(0,0);
    check(slice_tag_origin(tag)==0,"origin=0","wrong");
    check(slice_tag_hops(tag)==0,"hops=0","wrong");
}

static void t06_owns(void){
    section("T06  slice_owns_lane / slice_owns_node");
    EngineSlice s; slice_init(&s,1); /* B: lanes 18-35, nodes 54-107 */
    check(slice_owns_lane(&s,18),"owns lane 18","wrong");
    check(slice_owns_lane(&s,35),"owns lane 35","wrong");
    check(!slice_owns_lane(&s,17),"not owns lane 17","wrong");
    check(!slice_owns_lane(&s,36),"not owns lane 36","wrong");
    check(slice_owns_node(&s,54),"owns node 54","wrong");
    check(!slice_owns_node(&s,53),"not owns node 53","wrong");
}

static void t07_null(void){
    section("T07  NULL safety");
    slice_init(NULL,0);
    slice_set_init(NULL);
    check(!slice_owns_lane(NULL,0),"owns_lane(NULL)=0","crash");
    check(!slice_hop_ok(NULL,0),"hop_ok(NULL)=0","crash");
    check(1,"null paths ok","crash");
}

int main(void){
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS EngineSlice — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");
    t01_init(); t02_lane_owner(); t03_ghost_cross();
    t04_hop_guard(); t05_ghost_tag(); t06_owns(); t07_null();
    printf("\n══════════════════════════════════════════════════\n");
    if(g_fail==0) printf("  %d / %d PASS  v ALL PASS — EngineSlice live [S]\n",g_pass,g_pass);
    else printf("  %d / %d PASS  x %d FAILED\n",g_pass,g_pass+g_fail,g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
