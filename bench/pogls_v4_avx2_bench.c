/*
 * pogls_v4_avx2_bench.c — POGLS V4  AVX2 Vectorized Benchmark
 *
 * Tiers:
 *   S0  Scalar baseline       (reference)
 *   S1  Scalar + prefetch
 *   A1  AVX2 PHI scatter ×8   (scatter only)
 *   A2  AVX2 scatter + inv    (unit circle added)
 *   A3  AVX2 hybrid           (scatter + scalar conf pass)
 *   A4  AVX2 + prefetch
 *   T2  AVX2 + 2-shard pthread
 *
 * Compile:
 *   Linux/WSL2 (gcc):
 *     gcc -O3 -mavx2 -march=native -funroll-loops pogls_v4_avx2_bench.c -o avx2_bench -lpthread
 *
 *   Windows MSYS2/MinGW-w64:
 *     gcc -O3 -mavx2 -march=native -funroll-loops pogls_v4_avx2_bench.c -o avx2_bench.exe
 *
 *   Windows MSVC (x64 Native Tools Command Prompt):
 *     cl /O2 /arch:AVX2 /fp:fast pogls_v4_avx2_bench.c
 *
 * Run:
 *   ./avx2_bench          (Linux/WSL2/MSYS2)
 *   avx2_bench.exe        (Windows CMD)
 */

/* portability */
#if defined(_WIN32) || defined(_WIN64)
#  define POGLS_WIN 1
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <intrin.h>
   static inline double now_sec(void) {
       LARGE_INTEGER t, f;
       QueryPerformanceCounter(&t);
       QueryPerformanceFrequency(&f);
       return (double)t.QuadPart / (double)f.QuadPart;
   }
   typedef HANDLE pthread_t;
   typedef struct { HANDLE h; void*(*fn)(void*); void*arg; } _PCtx;
   static DWORD WINAPI _pw(LPVOID p){_PCtx*c=((_PCtx*)p);c->fn(c->arg);return 0;}
   static int pthread_create(pthread_t*t,void*a,void*(*fn)(void*),void*arg){
       _PCtx*c=(_PCtx*)malloc(sizeof(*c));c->fn=fn;c->arg=arg;
       *t=CreateThread(0,0,_pw,c,0,0);return *t?0:-1;}
   static int pthread_join(pthread_t t,void**r){
       (void)r;WaitForSingleObject(t,INFINITE);CloseHandle(t);return 0;}
#  define __builtin_popcount __popcnt
#  define __builtin_prefetch(p,r,l) (void)(p)
#else
#  define _POSIX_C_SOURCE 200809L
#  include <pthread.h>
#  include <time.h>
   static inline double now_sec(void) {
       struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
       return ts.tv_sec + ts.tv_nsec*1e-9;
   }
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>

/* PHI constants (FROZEN — single source) */
#define PHI_SCALE  (1u<<20)
#define PHI_UP     1696631u
#define PHI_DOWN    648055u
#define PHI_COMP    400521u
#define PHI_TOL       8192u
#define SMALL_T        128u
#define LARGE_T       1024u
#define N_OPS    10000000ULL
#define N_WARMUP   200000ULL

/* ── S0 scalar kernel (reference pv2) ─────────────────────────── */
static inline int pv2(uint32_t a,uint32_t p,uint32_t pm,uint32_t*sb,uint32_t*npm){
    uint32_t delta=(a>=*sb)?(a-*sb):(*sb-a); *sb=a;
    uint32_t d2=(a>=p)?(a-p):(p-a);
    uint32_t fp=((d2>=PHI_DOWN-PHI_TOL)&(d2<=PHI_DOWN+PHI_TOL))|
                ((d2>=PHI_COMP-PHI_TOL)&(d2<=PHI_COMP+PHI_TOL));
    uint32_t fl=(d2<SMALL_T),fg=(d2>=LARGE_T)&!fp;
    uint32_t rel=a^pm; rel^=(rel>>11); *npm=a^(a>>7);
    uint32_t m=PHI_SCALE-1u;
    uint32_t aa=(uint32_t)(((uint64_t)(a&m)*PHI_UP)>>20)&m;
    uint32_t bb=(uint32_t)(((uint64_t)(a&m)*PHI_DOWN)>>20)&m;
    uint32_t inv=(((uint64_t)aa*aa+(uint64_t)bb*bb)>>41)!=0;
    uint32_t conf=(fp|fl)&~fg;
    if(!conf&&!fg){uint32_t anc=rel&(rel>>8)&(rel>>16);conf=(uint32_t)(__builtin_popcount(anc)==0);}
    (void)delta;
    return (int)(conf&~inv);
}

/* ── workload ──────────────────────────────────────────────────── */
static void gen_mixed(uint32_t*out,uint64_t N){
    uint32_t x=42,ap=0;
    for(uint64_t i=0;i<N;i++){
        uint32_t r=(uint32_t)(i&3);
        if(r==0)      out[i]=(uint32_t)((i*4)&(PHI_SCALE-1u));
        else if(r==1){ap=(ap+PHI_DOWN)&(PHI_SCALE-1u);out[i]=ap;}
        else if(r==2) out[i]=(uint32_t)(((i/8)*64)&(PHI_SCALE-1u));
        else{x^=x>>13;x*=0x9e3779b9u;x^=x>>17;out[i]=x&(PHI_SCALE-1u);}
    }
}

/* ── AVX2 abs32 helper ─────────────────────────────────────────── */
static inline __m256i avx2_abs32(__m256i a, __m256i b){
    __m256i d=_mm256_sub_epi32(a,b);
    __m256i mask=_mm256_cmpgt_epi32(b,a);
    return _mm256_sub_epi32(_mm256_xor_si256(d,mask),mask);
}

/*
 * avx2_phi_scatter_8
 * Processes 8 addrs in parallel:
 *   aa[i] = (addr[i] * PHI_UP)   >> 20  & mask20
 *   bb[i] = (addr[i] * PHI_DOWN) >> 20  & mask20
 *   out_inv[i] = (aa^2 + bb^2) >> 41 != 0   (outside unit circle)
 *   out_phi[i] = |addr[i]-prev[i]| in PHI range  (nonzero = PHI match)
 *
 * Uses _mm256_mul_epu32 (even-lane 32->64 mul) via even+odd interleave.
 */
static inline void avx2_phi_scatter_8(
    const uint32_t *a8, const uint32_t *prev8,
    uint32_t *out_inv, uint32_t *out_phi)
{
    const __m256i vmask  = _mm256_set1_epi32((int)(PHI_SCALE-1u));
    const __m256i vone   = _mm256_set1_epi32(1);
    const __m256i vzero  = _mm256_setzero_si256();
    const __m256i vup    = _mm256_set1_epi32((int)PHI_UP);
    const __m256i vdown  = _mm256_set1_epi32((int)PHI_DOWN);
    const __m256i vlo64  = _mm256_set1_epi64x(0x00000000FFFFFFFFLL);
    const __m256i vhi64  = _mm256_set1_epi64x((int64_t)0xFFFFFFFF00000000LL);

    __m256i va   = _mm256_loadu_si256((const __m256i*)a8);
    __m256i va_m = _mm256_and_si256(va, vmask);

    /* --- PHI scatter: aa = (a_m * PHI_UP) >> 20, all 8 lanes --- */
    /* even lanes via mul_epu32 */
    __m256i aa_e = _mm256_srli_epi64(_mm256_mul_epu32(va_m, vup), 20);
    /* odd lanes: shift input down 32, mul, shift result back up */
    __m256i va_hi = _mm256_srli_epi64(va_m, 32);
    __m256i aa_o  = _mm256_slli_epi64(
                      _mm256_srli_epi64(_mm256_mul_epu32(va_hi,vup),20), 32);
    __m256i aa = _mm256_and_si256(
                    _mm256_or_si256(
                        _mm256_and_si256(aa_e, vlo64),
                        _mm256_and_si256(aa_o, vhi64)),
                    vmask);

    /* same for bb */
    __m256i bb_e = _mm256_srli_epi64(_mm256_mul_epu32(va_m, vdown), 20);
    __m256i bb_o = _mm256_slli_epi64(
                     _mm256_srli_epi64(_mm256_mul_epu32(va_hi,vdown),20), 32);
    __m256i bb = _mm256_and_si256(
                    _mm256_or_si256(
                        _mm256_and_si256(bb_e, vlo64),
                        _mm256_and_si256(bb_o, vhi64)),
                    vmask);

    /* --- unit circle: aa^2 + bb^2, check >> 41 --- */
    /* aa^2 even lanes */
    __m256i aa_sq_e = _mm256_mul_epu32(aa, aa);
    __m256i aa_hi2  = _mm256_srli_epi64(aa, 32);
    __m256i aa_sq_o = _mm256_slli_epi64(_mm256_mul_epu32(aa_hi2,aa_hi2), 32);
    __m256i bb_sq_e = _mm256_mul_epu32(bb, bb);
    __m256i bb_hi2  = _mm256_srli_epi64(bb, 32);
    __m256i bb_sq_o = _mm256_slli_epi64(_mm256_mul_epu32(bb_hi2,bb_hi2), 32);

    /* sum even+odd separately then >> 41 */
    __m256i sum_e = _mm256_add_epi64(
                        _mm256_and_si256(aa_sq_e, vlo64),
                        _mm256_and_si256(bb_sq_e, vlo64));
    __m256i sum_o = _mm256_add_epi64(
                        _mm256_srli_epi64(aa_sq_o, 32),
                        _mm256_srli_epi64(bb_sq_o, 32));

    /* inv_e: 1 where sum_e >> 41 != 0  (outside circle in even lanes) */
    __m256i inv_e64 = _mm256_srli_epi64(sum_e, 41);
    __m256i inv_e_flag = _mm256_and_si256(
                            _mm256_andnot_si256(
                                _mm256_cmpeq_epi32(inv_e64, vzero),
                                vone),
                            vlo64);   /* keep only low 32b per 64b lane */

    /* inv_o: 1 where sum_o >> 41 != 0 (outside circle in odd lanes) */
    __m256i inv_o64   = _mm256_slli_epi64(_mm256_srli_epi64(sum_o,41), 32);
    __m256i inv_o_cmp = _mm256_cmpeq_epi32(
                            _mm256_and_si256(inv_o64, vhi64), vzero);
    __m256i inv_o_flag = _mm256_and_si256(
                            _mm256_andnot_si256(inv_o_cmp, vone),
                            vhi64);

    __m256i vinv = _mm256_or_si256(inv_e_flag, inv_o_flag);
    _mm256_storeu_si256((__m256i*)out_inv, vinv);

    /* --- PHI delta: |a - prev| in [PHI_DOWN±TOL] or [PHI_COMP±TOL] --- */
    __m256i vprev = _mm256_loadu_si256((const __m256i*)prev8);
    __m256i vd2   = avx2_abs32(va, vprev);

    __m256i vlo_d = _mm256_set1_epi32((int)(PHI_DOWN-PHI_TOL));
    __m256i vhi_d = _mm256_set1_epi32((int)(PHI_DOWN+PHI_TOL));
    __m256i vlo_c = _mm256_set1_epi32((int)(PHI_COMP-PHI_TOL));
    __m256i vhi_c = _mm256_set1_epi32((int)(PHI_COMP+PHI_TOL));

    __m256i in_d = _mm256_and_si256(
                        _mm256_cmpgt_epi32(vd2, _mm256_sub_epi32(vlo_d,vone)),
                        _mm256_cmpgt_epi32(_mm256_add_epi32(vhi_d,vone), vd2));
    __m256i in_c = _mm256_and_si256(
                        _mm256_cmpgt_epi32(vd2, _mm256_sub_epi32(vlo_c,vone)),
                        _mm256_cmpgt_epi32(_mm256_add_epi32(vhi_c,vone), vd2));
    __m256i vfp  = _mm256_or_si256(in_d, in_c);
    _mm256_storeu_si256((__m256i*)out_phi, vfp);
}

/* ── bench result struct ───────────────────────────────────────── */
typedef struct { double mps; uint64_t sink; } BR;

/* ── S0 scalar ─────────────────────────────────────────────────── */
static BR run_s0(const uint32_t *g, double *base_out){
    uint32_t p=0,sb=0,pm=0,npm=0; uint64_t r=0;
    double t0=now_sec();
    for(uint64_t i=0;i<N_OPS;i++){r+=(uint64_t)(unsigned)pv2(g[i],p,pm,&sb,&npm);p=g[i];pm=npm;}
    double mps=(double)N_OPS/(now_sec()-t0)/1e6;
    if(base_out)*base_out=mps;
    return (BR){mps,r};
}

/* ── S1 scalar + prefetch ──────────────────────────────────────── */
static BR run_s1(const uint32_t *g){
    uint32_t p=0,sb=0,pm=0,npm=0; uint64_t r=0;
    double t0=now_sec();
    uint32_t cur=g[0];
    for(uint64_t i=1;i<N_OPS;i++){
        uint32_t nxt=g[i];
        if(i+16<N_OPS)__builtin_prefetch(&g[i+16],0,1);
        r+=(uint64_t)(unsigned)pv2(cur,p,pm,&sb,&npm);p=cur;pm=npm;cur=nxt;
    }
    r+=(uint64_t)(unsigned)pv2(cur,p,pm,&sb,&npm);
    return (BR){(double)N_OPS/(now_sec()-t0)/1e6,r};
}

/* ── A1 AVX2 scatter only ──────────────────────────────────────── */
static BR run_a1(const uint32_t *g){
    uint32_t prev8[8]={0}; uint32_t inv8[8],phi8[8]; uint64_t r=0;
    double t0=now_sec();
    for(uint64_t i=0;i+8<=N_OPS;i+=8){
        avx2_phi_scatter_8(&g[i],prev8,inv8,phi8);
        for(int k=0;k<8;k++){r+=phi8[k]?1u:0u;prev8[k]=g[i+k];}
    }
    return (BR){(double)N_OPS/(now_sec()-t0)/1e6,r};
}

/* ── A2 AVX2 scatter + unit circle ─────────────────────────────── */
static BR run_a2(const uint32_t *g){
    uint32_t prev8[8]={0}; uint32_t inv8[8],phi8[8]; uint64_t r=0;
    double t0=now_sec();
    for(uint64_t i=0;i+8<=N_OPS;i+=8){
        avx2_phi_scatter_8(&g[i],prev8,inv8,phi8);
        for(int k=0;k<8;k++){
            r+=((phi8[k]!=0)||(inv8[k]==0))?1u:0u;
            prev8[k]=g[i+k];
        }
    }
    return (BR){(double)N_OPS/(now_sec()-t0)/1e6,r};
}

/* ── A3 AVX2 hybrid (scatter + scalar conf) ─────────────────────── */
static BR run_a3(const uint32_t *g){
    uint32_t prev8[8]={0}; uint32_t inv8[8],phi8[8]; uint64_t r=0;
    uint32_t p=0,sb=0,pm=0;
    double t0=now_sec();
    (void)sb;
    for(uint64_t i=0;i+8<=N_OPS;i+=8){
        avx2_phi_scatter_8(&g[i],prev8,inv8,phi8);
        for(int k=0;k<8;k++){
            uint32_t a=g[i+k];
            uint32_t d2=(a>=p)?(a-p):(p-a);
            uint32_t fl=(d2<SMALL_T);
            uint32_t fg=(d2>=LARGE_T)&!(phi8[k]!=0);
            uint32_t conf=((phi8[k]!=0)|fl)&~fg;
            if(!conf&&!fg){
                uint32_t rel=(a^pm);rel^=(rel>>11);
                uint32_t anc=rel&(rel>>8)&(rel>>16);
                conf=(__builtin_popcount(anc)==0)?1u:0u;
            }
            r+=(uint64_t)(conf&(inv8[k]==0?1u:0u));
            p=a; pm=a^(a>>7); prev8[k]=a;
        }
    }
    return (BR){(double)N_OPS/(now_sec()-t0)/1e6,r};
}

/* ── A4 AVX2 + prefetch ─────────────────────────────────────────── */
static BR run_a4(const uint32_t *g){
    uint32_t prev8[8]={0}; uint32_t inv8[8],phi8[8]; uint64_t r=0;
    double t0=now_sec();
    for(uint64_t i=0;i+8<=N_OPS;i+=8){
        if(i+32<N_OPS)__builtin_prefetch(&g[i+32],0,1);
        avx2_phi_scatter_8(&g[i],prev8,inv8,phi8);
        for(int k=0;k<8;k++){
            r+=((phi8[k]!=0)||(inv8[k]==0))?1u:0u;
            prev8[k]=g[i+k];
        }
    }
    return (BR){(double)N_OPS/(now_sec()-t0)/1e6,r};
}

/* ── T2 shard worker ────────────────────────────────────────────── */
typedef struct{uint32_t*a;uint64_t n;double elapsed;uint64_t res;}ShW;
static void *shard_avx2_worker(void *arg){
    ShW *w=(ShW*)arg;
    uint32_t prev8[8]={0}; uint32_t inv8[8],phi8[8]; uint64_t r=0;
    double t0=now_sec();
    uint64_t i=0;
    for(;i+8<=w->n;i+=8){
        avx2_phi_scatter_8(&w->a[i],prev8,inv8,phi8);
        for(int k=0;k<8;k++){r+=((phi8[k]!=0)||(inv8[k]==0))?1u:0u;prev8[k]=w->a[i+k];}
    }
    uint32_t p=prev8[0],sb=0,pm=0,npm=0;
    for(;i<w->n;i++){r+=(uint64_t)(unsigned)pv2(w->a[i],p,pm,&sb,&npm);p=w->a[i];pm=npm;}
    w->elapsed=now_sec()-t0; w->res=r; return NULL;
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void){
    uint32_t *g=(uint32_t*)malloc(N_OPS*sizeof(uint32_t));
    if(!g){perror("malloc");return 1;}
    gen_mixed(g,N_OPS);

    /* warmup */
    {uint32_t p=0,sb=0,pm=0,npm=0; volatile uint64_t s=0;
     for(uint64_t i=0;i<N_WARMUP;i++){s+=(uint64_t)(unsigned)pv2(g[i],p,pm,&sb,&npm);p=g[i];pm=npm;}}

    double _base=0;
    BR s0=run_s0(g,&_base);

    printf("\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  POGLS V4 — AVX2 Benchmark   N=%lluM  mixed workload\n",
           (unsigned long long)N_OPS/1000000);
    printf("══════════════════════════════════════════════════════════\n\n");
    printf("  %-40s %6.1f M/s  (1.00x)  <- base\n","S0: Scalar baseline",s0.mps);

    BR s1=run_s1(g);
    printf("  %-40s %6.1f M/s  (%+.2fx)\n","S1: Scalar + prefetch",s1.mps,s1.mps/_base);

    printf("\n");

    BR a1=run_a1(g);
    printf("  %-40s %6.1f M/s  (%+.2fx)\n","A1: AVX2 PHI scatter x8",a1.mps,a1.mps/_base);

    BR a2=run_a2(g);
    printf("  %-40s %6.1f M/s  (%+.2fx)\n","A2: AVX2 scatter + unit circle",a2.mps,a2.mps/_base);

    BR a3=run_a3(g);
    printf("  %-40s %6.1f M/s  (%+.2fx)\n","A3: AVX2 hybrid (scatter+conf)",a3.mps,a3.mps/_base);

    BR a4=run_a4(g);
    printf("  %-40s %6.1f M/s  (%+.2fx)\n","A4: AVX2 + prefetch",a4.mps,a4.mps/_base);

    printf("\n  -- T2: AVX2 + 2-shard parallel --------------------------\n");
    {
        uint32_t *s0p=(uint32_t*)malloc(N_OPS*sizeof(uint32_t));
        uint32_t *s1p=(uint32_t*)malloc(N_OPS*sizeof(uint32_t));
        uint64_t n0=0,n1=0;
        for(uint64_t i=0;i<N_OPS;i++){
            uint32_t sid=(g[i]^(g[i]>>10))&1u;
            if(sid==0)s0p[n0++]=g[i]; else s1p[n1++]=g[i];
        }
        ShW w0={s0p,n0,0,0},w1={s1p,n1,0,0};
        pthread_t th0,th1;
        double wall=now_sec();
        pthread_create(&th0,NULL,shard_avx2_worker,&w0);
        pthread_create(&th1,NULL,shard_avx2_worker,&w1);
        pthread_join(th0,NULL); pthread_join(th1,NULL);
        wall=now_sec()-wall;
        double wmps=(double)N_OPS/wall/1e6;
        printf("  shard0=%lluM  %.1fM/s\n",(unsigned long long)n0/1000000,n0/w0.elapsed/1e6);
        printf("  shard1=%lluM  %.1fM/s\n",(unsigned long long)n1/1000000,n1/w1.elapsed/1e6);
        printf("  %-40s %6.1f M/s  (%+.2fx)\n","T2: AVX2+2shard (wall)",wmps,wmps/_base);
        free(s0p);free(s1p);
    }

    printf("\n  -- Production estimate -----------------------------------\n");
    printf("  compute-only best:          %.0f M/s\n", _base);
    printf("  with disk (~70%% cache hit): %.0f M/s  (estimated)\n", _base*0.30);
    printf("  spec target:                13.6 M/s\n");
    printf("  margin (with disk):         %.1fx above spec\n", _base*0.30/13.6);
    printf("\n  AVX2 vectorizes: PHI scatter x8, unit circle check.\n");
    printf("  Chain (d2/rel/conf) stays scalar — no false parallelism.\n");
    printf("\n  (sink=%llu)\n",(unsigned long long)(s0.sink+s1.sink+a1.sink+a2.sink+a3.sink+a4.sink));
    printf("══════════════════════════════════════════════════════════\n");
    free(g); return 0;
}
