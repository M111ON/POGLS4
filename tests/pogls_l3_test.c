/* pogls_l3_test.c — L3 Hybrid Field Router Tests */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "pogls_l3_intersection.h"

static int _p=0,_f=0;
#define CHECK(cond,msg) do{ if(cond){printf("  PASS  %s\n",msg);_p++;}else{printf("  FAIL  %s (line %d)\n",msg,__LINE__);_f++;} }while(0)

void test_structured(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 1: Structured (grid)                    ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    L3Engine eng; l3_init(&eng);
    int mc=0;
    for(int i=0;i<800;i++){
        uint64_t v=((uint64_t)(1000+(i%4)*10)<<16)|(1000+(i/4%4)*10);
        if(l3_process(&eng,v)==ROUTE_MAIN) mc++;
    }
    l3_print_stats(&eng);
    printf("MAIN=%d/800\n",mc);
    CHECK(mc>400,"Structured → MAIN majority");
}

void test_chaotic(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 2: Chaotic (random coords)              ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    L3Engine eng; l3_init(&eng);
    srand(42); int nonmain=0;
    for(int i=0;i<1000;i++){
        uint64_t v=((uint64_t)(rand()&0xFFFF)<<16)|(rand()&0xFFFF);
        if(l3_process(&eng,v)!=ROUTE_MAIN) nonmain++;
    }
    l3_print_stats(&eng);
    CHECK(nonmain>250,"Chaotic → GHOST/SHADOW > 25%");
}

void test_wolfram(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 3: Wolfram Rule 30                      ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    static const uint8_t ex[8]={0,1,1,1,1,0,0,0};
    int ok=1;
    for(int i=0;i<8;i++){
        uint8_t l=(i>>2)&1,m=(i>>1)&1,r=i&1,o=wolfram_rule30(l,m,r);
        printf("  %d%d%d→%d(expect %d) %s\n",l,m,r,o,ex[i],o==ex[i]?"✓":"✗");
        if(o!=ex[i]) ok=0;
    }
    CHECK(ok,"Wolfram Rule 30 correct");
}

void test_scatter(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 4: Scatter + GeoGate                    ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    uint32_t a,b;
    l3_scatter(0, &a, &b); CHECK(a==0&&b==0,"scatter(0)=(0,0)");
    l3_scatter(648055, &a, &b);
    uint64_t sq=(uint64_t)a*a+(uint64_t)b*b;
    CHECK((sq>>L3_CIRCLE_SHIFT)==0,"scatter result inside circle");
    CHECK(L3_PHI_UP==1696631u,"L3_PHI_UP frozen");
    CHECK(L3_PHI_DOWN==648055u,"L3_PHI_DOWN frozen");
    CHECK(L3_PROBE_A==312320u,"L3_PROBE_A = fib(610)×2⁹");
    CHECK(L3_PROBE_B==729088u,"L3_PROBE_B = fib(89)×2¹³");
}

void test_consistency(void) {
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Test 5: Consistency + PHI-spread             ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    /* same value fresh engine → same route */
    L3Engine e1,e2; l3_init(&e1); l3_init(&e2);
    RouteTarget r1=l3_process(&e1,0xDEADBEEF);
    RouteTarget r2=l3_process(&e2,0xDEADBEEF);
    CHECK(r1==r2,"Deterministic: same value → same route");
    /* PHI-spread should have high MAIN */
    L3Engine eng; l3_init(&eng);
    int mc=0; uint32_t PHI_UP=1696631u,MASK=(1<<20)-1;
    for(int i=0;i<200;i++){
        uint64_t v=(uint64_t)((i*PHI_UP)&MASK);
        if(l3_process(&eng,v)==ROUTE_MAIN) mc++;
    }
    printf("  PHI-spread MAIN=%d/200  s_flip=%u r_flip=%u\n",
           mc,eng.structured_flip,eng.random_flip);
    CHECK(mc>100,"PHI-spread → MAIN > 50% (phase flip boost)");
    CHECK(eng.structured_flip>0,"structured_flip detected");
    l3_print_stats(&eng);
}

static void test_prev_drift(void);  /* forward decl */

int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  L3 Hybrid Field Router Tests V2              ║\n");
    printf("║  GeoGate + L3score + PhaseFlip + StreakGuard  ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    test_structured(); test_chaotic(); test_wolfram();
    test_scatter();    test_consistency();
    test_prev_drift();
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Tests Complete                               ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    printf("  %d/%d PASS%s\n\n",_p,_p+_f,_f==0?" ✓ ALL PASS":"");
    return _f?1:0;
}

/* ══════════════════════════════════════════════════════════════════
 * T13: prev-drift test — seq addr stream with corrupted prev
 *
 * Scenario: addr moves sequentially (0,4,8,12...) but every 10th
 * step prev is replaced with a random value (simulates desync).
 * System must still classify majority as MAIN (structured pattern
 * survives despite occasional prev noise — self-mix gives tolerance).
 * ══════════════════════════════════════════════════════════════════ */
static void test_prev_drift(void)
{
    printf("\n── T13: prev-drift tolerance ──────────────────────────\n");
    L3Engine eng; l3_init(&eng);

    int main_count = 0, total = 200;
    uint32_t rng = 0xDEADC0DE;

    for (int i = 0; i < total; i++) {
        /* sequential addr */
        uint32_t addr = (uint32_t)(i * 4) & 0xFFFFFu;

        /* every 10 steps: corrupt prev_addr with random value */
        if (i % 10 == 0) {
            rng ^= rng >> 13; rng *= 0x9e3779b9u; rng ^= rng >> 17;
            eng.prev_addr  = rng & 0xFFFFFu;   /* inject noise */
            eng.prev2_addr = (rng >> 3) & 0xFFFFFu;
        }

        RouteTarget r = l3_process(&eng, (uint64_t)addr);
        if (r == ROUTE_MAIN) main_count++;
    }

    float pct = 100.f * main_count / total;
    printf("  seq + 10%% prev-drift: MAIN=%.0f%% (%d/%d)\n",
           pct, main_count, total);
    /* expect >= 60% MAIN — self-mix absorbs noise, structured signal survives */
    CHECK(main_count >= total * 60 / 100,
          "prev-drift: majority still MAIN despite 10% noise");
}
