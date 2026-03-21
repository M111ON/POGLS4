#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "pogls_gpu_pipeline.h"

static int _p=0,_f=0;
#define TEST(n) do{int _o=(n);if(_o){printf("  PASS  %s\n",#n);_p++;}else{printf("  FAIL  %s (line %d)\n",#n,__LINE__);_f++;}}while(0)

static int t_morton_zero(void){ return gpu_morton_encode(0,0)==0; }
static int t_morton_nonzero(void){ return gpu_morton_encode(1,0)==1 && gpu_morton_encode(0,1)==2; }
static int t_hilbert_range(void){
    for(uint32_t i=0;i<256;i++)
        if(gpu_hilbert_from_morton(i)>=256) return 0;
    return 1;
}
static int t_angular_theta(void){
    uint32_t t0=gpu_angular_to_theta_fp(0);
    uint32_t t1=gpu_angular_to_theta_fp(1000);
    return t0==0 && t1>0 && t1<(1u<<20);
}
static int t_batch_init(void){
    GpuBatchCtx ctx;
    int r=gpu_batch_init(&ctx,1024);
    gpu_batch_free(&ctx);
    return r==0;
}
static int t_batch_submit_flush(void){
    GpuBatchCtx ctx; gpu_batch_init(&ctx,1024);
    for(int i=0;i<100;i++) gpu_submit(&ctx,(uint64_t)i*12345);
    gpu_flush(&ctx);
    int ok=(ctx.total_coords==100&&ctx.count==0);
    gpu_batch_free(&ctx);
    return ok;
}
static int t_batch_process_correct(void){
    GpuBatchCtx ctx; gpu_batch_init(&ctx,10);
    gpu_submit(&ctx, 0);
    gpu_flush(&ctx);
    GpuCoord *c=gpu_get(&ctx,0);
    int ok=(c && c->morton==0 && c->hilbert<256 && c->lane<54);
    gpu_batch_free(&ctx);
    return ok;
}
static int t_audit_pass(void){
    GpuBatchCtx ctx; gpu_batch_init(&ctx,10);
    /* addr=0 → XOR of bytes = 0 → audit pass */
    gpu_submit(&ctx,0);
    gpu_flush(&ctx);
    GpuCoord *c=gpu_get(&ctx,0);
    int ok=(c && c->audit==0);
    gpu_batch_free(&ctx);
    return ok;
}
static int t_lane_range(void){
    GpuBatchCtx ctx; gpu_batch_init(&ctx,1024);
    for(int i=0;i<1000;i++) gpu_submit(&ctx,(uint64_t)i*7919);
    gpu_flush(&ctx);
    int ok=1;
    for(int i=0;i<1000;i++){
        GpuCoord *c=gpu_get(&ctx,i);
        if(!c||c->lane>=54){ok=0;break;}
    }
    gpu_batch_free(&ctx);
    return ok;
}
static int t_throughput(void){
    GpuBatchCtx ctx; gpu_batch_init(&ctx, GPU_BATCH_OPTIMAL);
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    uint32_t N=GPU_BATCH_OPTIMAL;
    for(uint32_t i=0;i<N;i++) gpu_submit(&ctx,(uint64_t)i*PHI_UP);
    gpu_flush(&ctx);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double ms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;
    double mps=N/ms/1000.0;
    printf("  [bench] %.0fK coords in %.1fms = %.1fM/s (CPU fallback)\n",
           N/1000.0, ms, mps);
    gpu_batch_free(&ctx);
    return mps > 10.0; /* >10M/s minimum */
}

int main(void){
    printf("\n══ POGLS V3.95 GPU Pipeline Tests ══\n\n");
    printf("── Group 1: Math functions ──\n");
    TEST(t_morton_zero()); TEST(t_morton_nonzero());
    TEST(t_hilbert_range()); TEST(t_angular_theta());
    printf("\n── Group 2: Batch context ──\n");
    TEST(t_batch_init()); TEST(t_batch_submit_flush());
    TEST(t_batch_process_correct()); TEST(t_audit_pass());
    TEST(t_lane_range());
    printf("\n── Group 3: Throughput ──\n");
    TEST(t_throughput());
    printf("\n══════════════════════════════════════\n");
    printf("  %d / %d PASS",_p,_p+_f);
    if(!_f) printf("  ✓ ALL PASS — GPU wired (CPU fallback active) 🚀\n");
    else printf("  ✗ %d FAIL\n",_f);
    printf("══════════════════════════════════════\n\n");
    return _f?1:0;
}
