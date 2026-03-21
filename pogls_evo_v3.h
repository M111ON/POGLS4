/*
 * pogls_evo_v3.h — POGLS V3.95  Evolution Core V3
 * ══════════════════════════════════════════════════════════════════════
 * Extends V2 with 12 refinements (GPT production spec):
 *
 *  1. 2-way ghost set      (silent collision prevention)
 *  2. Dynamic threshold    (adaptive intersection)
 *  3. 2-step drift         (hysteresis)
 *  4. Bit mixing lane      (skew ~1.2x)
 *  5. Phase-shift time     (cycle-aware)
 *  6. Auto-tuning          (self-tuning threshold)
 *  7. Ghost predictor      (hint byte)
 *  8. Loop detection       (End of Cycle)
 *  9. Energy budget        (global stabilizer)
 * 10. Lane pressure        (self-balancing)
 * 11. Phase state          (cyclic behavior)
 * 12. Combined evo3_process (single unified function)
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_EVO_V3_H
#define POGLS_EVO_V3_H

#include <stdint.h>
#include <string.h>

/* ── Config ──────────────────────────────────────────────────────── */
#define EV3_FP_SHIFT          12u
#define EV3_MANDEL_ITER       32u
#define EV3_ESCAPE_SQ         (4 << EV3_FP_SHIFT)
#define EV3_FIB_CAP          128u
#define EV3_LANES             54u
#define EV3_GHOST_CAP       2048u   /* power of 2 */
#define EV3_GHOST_WAYS         2u   /* 2-way set   */
#define EV3_GHOST_AGE_MAX   1024u
#define EV3_DRIFT_LIMIT        3u
#define EV3_ENERGY_MAX    (1u<<20)
#define EV3_AVG_LOAD         200u
#define EV3_MAGIC       0x45563300u /* "EV3\0" */

/* ══════════════════════════════════════════════════════════════════
 * 1. Ghost Store V3 — 2-way set (no silent collision)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t key;
    uint32_t val;
    uint8_t  hint;    /* predicted next lane (point 7) */
    uint16_t epoch;
    uint8_t  _pad;
} GhostEntryV3;   /* 16B */

typedef struct {
    GhostEntryV3 ways[EV3_GHOST_CAP][EV3_GHOST_WAYS];
    uint32_t     clock;
    uint64_t     hits, stores, evictions, hint_hits;
} GhostV3;

static inline uint32_t _ev3_hash(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
    return (uint32_t)k;
}
static inline uint32_t _ev3_rotl(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static inline void ghost_v3_init(GhostV3 *g) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
}

static inline int ghost_v3_lookup(GhostV3 *g, uint64_t key,
                                   uint32_t *val, uint8_t expected_lane)
{
    if (!g || !val) return 0;
    uint32_t i = _ev3_hash(key) & (EV3_GHOST_CAP - 1);
    for (int w = 0; w < (int)EV3_GHOST_WAYS; w++) {
        GhostEntryV3 *e = &g->ways[i][w];
        if (e->key == key && e->key != 0) {
            *val = e->val;
            e->epoch = (uint16_t)g->clock;
            g->hits++;
            /* hint check (point 7) */
            if (e->hint == expected_lane) g->hint_hits++;
            return 1;
        }
    }
    return 0;
}

static inline void ghost_v3_store(GhostV3 *g, uint64_t key,
                                   uint32_t val, uint8_t lane)
{
    if (!g) return;
    uint32_t i = _ev3_hash(key) & (EV3_GHOST_CAP - 1);
    /* find victim: empty or oldest */
    int victim = 0;
    uint16_t age0 = (uint16_t)(g->clock - g->ways[i][0].epoch);
    uint16_t age1 = (uint16_t)(g->clock - g->ways[i][1].epoch);
    if (g->ways[i][0].key == 0) victim = 0;
    else if (g->ways[i][1].key == 0) victim = 1;
    else { victim = (age0 > age1) ? 0 : 1; g->evictions++; }

    g->ways[i][victim] = (GhostEntryV3){key, val, lane,
                                         (uint16_t)g->clock, 0};
    g->stores++;
    g->clock++;
}

/* ══════════════════════════════════════════════════════════════════
 * 2+6. Auto-tuning threshold (adaptive intersection, point 2+6)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct { uint16_t hit, miss; } TuneStat;

static inline uint32_t dynamic_thresh(TuneStat *s, uint32_t state_hash)
{
    uint32_t total = (uint32_t)s->hit + s->miss + 1;
    uint32_t ratio = ((uint32_t)s->hit << 8) / total;  /* 0..255 */
    /* base 4..7 from state_hash, +0..3 from ratio */
    return (4u + (state_hash & 0x3u)) + (ratio >> 6);
}

/* ══════════════════════════════════════════════════════════════════
 * 3. 2-step drift (hysteresis, point 3)
 * ══════════════════════════════════════════════════════════════════ */
static inline int drift_ok_2step(uint32_t curr, uint32_t prev,
                                  uint32_t prev2)
{
    uint32_t d1 = (uint32_t)__builtin_popcount(curr ^ prev);
    uint32_t d2 = (uint32_t)__builtin_popcount(prev ^ prev2);
    return (d1 + d2) <= (EV3_DRIFT_LIMIT << 1);
}

/* ══════════════════════════════════════════════════════════════════
 * Main context
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    GhostV3    ghost;
    TuneStat   tune;

    /* drift memory (point 3) */
    uint32_t   prev_hash, prev2_hash;

    /* loop detection (point 8) */
    uint32_t   prev_sig, prev2_sig;
    uint32_t   loop_count;

    /* energy budget (point 9) */
    uint32_t   energy_pool;

    /* lane pressure (point 10) */
    uint16_t   lane_load[EV3_LANES];

    /* tick / phase */
    uint32_t   tick;

    /* stats */
    uint64_t   total, fast_path, shadow_path, loop_detected;

    uint32_t   magic;
} EvoV3;

static inline int evo3_init(EvoV3 *ec) {
    if (!ec) return -1;
    memset(ec, 0, sizeof(*ec));
    ghost_v3_init(&ec->ghost);
    ec->magic = EV3_MAGIC;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * Helpers (Mandelbrot + Fibonacci, same as V2)
 * ══════════════════════════════════════════════════════════════════ */
static inline int _ev3_mandel(int32_t cx, int32_t cy) {
    int32_t x=0,y=0,x2=0,y2=0;
    for (int i=0;i<(int)EV3_MANDEL_ITER;i++){
        if ((uint32_t)(x2+y2)>(uint32_t)EV3_ESCAPE_SQ) return 0;
        y=((x*y)>>((int)EV3_FP_SHIFT-1))+cy;
        x=x2-y2+cx; x2=(x*x)>>EV3_FP_SHIFT; y2=(y*y)>>EV3_FP_SHIFT;
    }
    return 1;
}

static inline void _ev3_map(uint32_t addr, int32_t *cx, int32_t *cy) {
    *cx = ((int32_t)(addr & 0xFFF) - 2048) * 3 / 2;
    *cy = ((int32_t)((addr>>12)&0xFFF) - 2048) * 3 / 2;
}

static inline int32_t _ev3_fib(uint32_t seed) {
    uint32_t s = seed & (EV3_FIB_CAP-1); if(!s)s=1;
    int32_t f0=(int32_t)s, f1=(int32_t)(s>>1); if(!f1)f1=1;
    int32_t n=f0+f1;
    if((f0>0&&f1>0&&n<0)||(f0<0&&f1<0&&n>0)){f0>>=1;f1>>=1;n=f0+f1;}
    return n;
}

/* ══════════════════════════════════════════════════════════════════
 * 12. evo3_process — unified function (all 12 points)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t evo3_process(EvoV3 *ec, uint64_t addr)
{
    if (!ec) return 0;
    ec->total++;
    ec->tick++;

    /* ── Hashes ─────────────────────────────────────────────────── */
    uint32_t spatial    = (uint32_t)(addr ^ (addr >> 32));
    uint32_t state_hash = _ev3_rotl(spatial, 13);

    /* point 5: phase-shift time_hash (cycle-aware) */
    uint8_t  phase_shift = (uint8_t)(ec->tick & 0xFF);
    uint32_t time_hash   = _ev3_rotl(state_hash, phase_shift & 31)
                           ^ ec->tick;

    /* ── Ghost lookup (point 1: 2-way, point 7: hint) ───────────── */
    uint32_t cached;
    uint8_t  predicted_lane = (uint8_t)(addr % EV3_LANES);
    if (ghost_v3_lookup(&ec->ghost, state_hash, &cached, predicted_lane)) {
        ec->fast_path++;
        return cached;
    }

    /* ── Fibonacci ──────────────────────────────────────────────── */
    int32_t val = _ev3_fib((uint32_t)addr);

    /* ── Mandelbrot ─────────────────────────────────────────────── */
    int32_t cx, cy; _ev3_map((uint32_t)addr, &cx, &cy);
    int type = _ev3_mandel(cx, cy);  /* 1=stable 0=chaotic */

    /* damped feedback */
    val = type ? val - (val>>2) : val + (val>>1);

    /* point 9: energy budget */
    int32_t abs_val = val < 0 ? -val : val;
    ec->energy_pool += (uint32_t)abs_val;
    if (ec->energy_pool > EV3_ENERGY_MAX) {
        val >>= 2;
        ec->energy_pool >>= 1;
    }

    /* point 11: phase state (cyclic behavior) */
    uint8_t phase = (uint8_t)((ec->tick >> 8) & 0x3);
    switch (phase) {
    case 0: val += val >> 2; break;          /* expansion   */
    case 1: val -= val >> 2; break;          /* contraction */
    case 2: break;                           /* stable      */
    case 3: val ^= (val >> 1); break;        /* mutation    */
    }

    /* ── Loop detection (point 8: End of Cycle) ─────────────────── */
    uint32_t sig = state_hash ^ (ec->tick >> 4);
    if (sig == ec->prev_sig) {
        ec->loop_count++;
        ec->loop_detected++;
        val >>= 1;   /* force collapse */
    }
    ec->prev2_sig = ec->prev_sig;
    ec->prev_sig  = sig;

    /* ── 2-step drift check (point 3: hysteresis) ───────────────── */
    int drift_good = drift_ok_2step(time_hash,
                                     ec->prev_hash,
                                     ec->prev2_hash);
    ec->prev2_hash = ec->prev_hash;
    ec->prev_hash  = time_hash;

    /* ── Dynamic threshold (point 2+6: adaptive) ────────────────── */
    uint32_t anchor = state_hash;
    uint32_t inter  = anchor & _ev3_rotl(anchor, 7)
                             & _ev3_rotl(anchor, 13)
                             & _ev3_rotl(anchor, 19);
    uint32_t pc = (uint32_t)__builtin_popcount(inter);
    uint32_t th = dynamic_thresh(&ec->tune, state_hash);

    if (pc >= th && drift_good) {
        ec->tune.hit++;
        ec->fast_path++;
    } else {
        ec->tune.miss++;
        ec->shadow_path++;
    }

    /* ── Lane compute (point 4: bit mixing) ─────────────────────── */
    uint32_t mix = state_hash ^ (state_hash >> 7);
    mix ^= (mix << 11);
    uint32_t lane_raw = (mix ^ ((uint32_t)abs_val << 2) ^ (uint32_t)type)
                        & 0x3Fu;
    uint8_t lane = (uint8_t)(lane_raw - (lane_raw >= EV3_LANES)
                                       * EV3_LANES);

    /* point 10: lane pressure self-balancing */
    ec->lane_load[lane]++;
    if (ec->lane_load[lane] > EV3_AVG_LOAD) {
        lane = (uint8_t)((lane + 1) % EV3_LANES);
        ec->lane_load[lane]++;
    }

    /* ── Pack + store ────────────────────────────────────────────── */
    uint32_t packed = ((uint32_t)lane << 8) | (uint32_t)(type & 0xFF);
    ghost_v3_store(&ec->ghost, state_hash, packed, lane);
    return packed;
}

static inline uint8_t evo3_lane(uint32_t p) { return (uint8_t)(p >> 8); }
static inline uint8_t evo3_type(uint32_t p) { return (uint8_t)(p & 0xFF); }

static inline void evo3_stats(const EvoV3 *ec) {
    if (!ec) return;
    uint64_t t = ec->total ? ec->total : 1;
    printf("[EvoV3] total=%llu fast=%llu(%llu%%) shadow=%llu(%llu%%)\n"
           "        loops=%llu ghost_hits=%llu hint_hits=%llu energy=%u\n",
           (unsigned long long)ec->total,
           (unsigned long long)ec->fast_path,
           (unsigned long long)(ec->fast_path*100/t),
           (unsigned long long)ec->shadow_path,
           (unsigned long long)(ec->shadow_path*100/t),
           (unsigned long long)ec->loop_detected,
           (unsigned long long)ec->ghost.hits,
           (unsigned long long)ec->ghost.hint_hits,
           ec->energy_pool);
}

#endif /* POGLS_EVO_V3_H */
