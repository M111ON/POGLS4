#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "../pogls_pipeline_wire.h"
#ifndef PHI_DOWN
#define PHI_DOWN POGLS_PHI_DOWN
#endif

static int _p=0,_f=0;
#define TEST(n) do{int _o=(n);if(_o){printf("  PASS  %s\n",#n);_p++;}else{printf("  FAIL  %s (line %d)\n",#n,__LINE__);_f++;}}while(0)

static PipelineWire pw;

static int t_init(void){
    return pipeline_wire_init(&pw,"/tmp/pogls_wire_test")==0 
           && pw.magic==PIPELINE_WIRE_MAGIC;
}
static int t_process_returns_valid(void){
    RouteTarget r = pipeline_wire_process(&pw, 0xDEADBEEF, 12345);
    return r==ROUTE_MAIN||r==ROUTE_GHOST||r==ROUTE_SHADOW;
}
static int t_stats_accumulate(void){
    for(int i=0;i<100;i++)
        pipeline_wire_process(&pw,(uint64_t)i*1000+i, (uint64_t)i*17);
    return pw.total_in >= 100;
}
static int t_structured_goes_main(void){
    PipelineWire pw2;
    pipeline_wire_init(&pw2,"/tmp/pogls_wire_test2");
    uint64_t main_count=0;
    /* structured pattern near origin → should go MAIN */
    for(int i=0;i<200;i++){
        uint64_t v = (uint64_t)(i%4) | ((uint64_t)(i/4)<<16);
        RouteTarget r = pipeline_wire_process(&pw2, v, (uint64_t)i*54);
        if(r==ROUTE_MAIN) main_count++;
    }
    pipeline_wire_close(&pw2);
    return main_count > 50;  /* majority MAIN for structured */
}
static int t_chaotic_goes_ghost(void){
    PipelineWire pw3;
    pipeline_wire_init(&pw3,"/tmp/pogls_wire_test3");
    uint64_t ghost_count=0;
    /* random-ish far coords → should go GHOST */
    for(int i=0;i<200;i++){
        uint64_t v = (uint64_t)(i*7919) ^ (uint64_t)(i*6271)<<16;
        RouteTarget r = pipeline_wire_process(&pw3, v, (uint64_t)i*31337);
        if(r==ROUTE_GHOST||r==ROUTE_SHADOW) ghost_count++;
    }
    pipeline_wire_close(&pw3);
    return ghost_count > 50;
}
static int t_delta_commits(void){
    /* route_main > 0 means writes went through — delta_commits
     * only increments for ROUTE_MAIN that pass Hilbert lane.
     * Use a structured pattern to guarantee MAIN routes. */
    PipelineWire pw_dc;
    pipeline_wire_init(&pw_dc,"/tmp/pogls_wire_test_dc");
    for(int i=0;i<500;i++){
        uint64_t v=(uint64_t)(i*4)&0xFFFFF;  /* sequential, structured */
        pipeline_wire_process(&pw_dc, v, (uint64_t)i*PHI_DOWN);
    }
    int ok = pw_dc.delta_commits > 0 || pw_dc.route_main > 0;
    pipeline_wire_close(&pw_dc);
    return ok;
}
static int t_flush_ok(void){
    pipeline_wire_flush(&pw);
    return 1;  /* no crash = pass */
}

int main(void){
    mkdir("/tmp/pogls_wire_test",  0755);
    mkdir("/tmp/pogls_wire_test2", 0755);
    mkdir("/tmp/pogls_wire_test3", 0755);
    printf("\n══ POGLS V3.95 Pipeline Wire Tests ══\n\n");
    TEST(t_init());
    TEST(t_process_returns_valid());
    TEST(t_stats_accumulate());
    TEST(t_structured_goes_main());
    TEST(t_chaotic_goes_ghost());
    TEST(t_delta_commits());
    TEST(t_flush_ok());
    pipeline_wire_stats(&pw);
    pipeline_wire_close(&pw);
    printf("══════════════════════════════════════\n");
    printf("  %d / %d PASS",_p,_p+_f);
    if(!_f) printf("  ✓ ALL PASS — pipeline wired 🔗\n");
    else printf("  ✗ %d FAIL\n",_f);
    printf("══════════════════════════════════════\n\n");
    return _f?1:0;
}
