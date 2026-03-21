/*
 * pogls_evo_v2.h — POGLS V3.95  Evolution Core V2 (GPT Spec)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Implements GPT's production spec:
 *
 *   1. Bitwise Anchor + Intersection (แทน float overlap)
 *      anchor = uint32_t bitmask per axis
 *      intersection = F0 & F1 & F2 & F3
 *      score = popcount ≥ INTERSECTION_MIN → FAST PATH
 *
 *   2. Ghost Store + Epoch Aging (ดีกว่า v1)
 *      epoch uint16 → age = clock - epoch
 *      replace if empty OR age > 1024
 *      rotl32 hash → better entropy
 *
 *   3. evo_process() — unified single function
 *      state_hash = rotl32(spatial ^ tick, 13)
 *      ghost L0 → fib → mandel → damped feedback → lane
 *
 * Config (ตั้งค่าครั้งเดียวจบ):
 *   FP_SHIFT=12, DRIFT_LIMIT=3, INTERSECTION_MIN=6
 *   TEMPORAL_DRIFT_LIMIT=2, GHOST_CAP=4096
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_EVO_V2_H
#define POGLS_EVO_V2_H
/* ═══════════════════════════════════════════════════════════════
 * EVO V1/V2 DEPRECATED — Superseded by pogls_evo_v3.h
 * Use evo3_process() for all new code.
 * ═══════════════════════════════════════════════════════════════ */
#if defined(USE_EVO_V1) || defined(USE_EVO_V2)
#  error "evo_v1/v2 deprecated — use pogls_evo_v3.h"
#endif
/* Compatibility shim: evo_process → evo3_process
 * NOTE: Include pogls_evo_v3.h BEFORE this header in external callers.
 * Do NOT auto-include here — causes type conflicts with native evo_v1/v2 APIs.
 * Usage in new code: #include "pogls_evo_v3.h"  (directly) */
#ifndef EVO_COMPAT_SHIM
#define EVO_COMPAT_SHIM
#  define evo_process_v3(ctx, addr) evo3_process(ctx, addr)
#endif


#include <stdint.h>
#include <string.h>

/* ── Config (single source of truth) ────────────────────────────── */
#define EVO2_FP_SHIFT             12u
#define EVO2_FP_ONE               (1 << EVO2_FP_SHIFT)
#define EVO2_DRIFT_LIMIT           3u
#define EVO2_TEMPORAL_DRIFT_LIMIT  2u
#define EVO2_INTERSECTION_MIN      6u
#define EVO2_GHOST_CAP          4096u   /* must be power of 2 */
#define EVO2_GHOST_AGE_MAX      1024u
#define EVO2_MANDEL_ITER          32u
#define EVO2_ESCAPE_SQ     (4 << EVO2_FP_SHIFT)
#define EVO2_FIB_CAP             128u   /* 2^7 → bitmask mod */
#define EVO2_LANES                54u
#define EVO2_MAGIC          0x45563200u /* "EV2\0" */

/* ══════════════════════════════════════════════════════════════════
 * PART 1: Bitwise Anchor + Intersection
 *
 * Each axis (F0..F3) produces a uint32_t anchor bitmask
 * representing the "view" from that direction.
 *
 * intersection = F0 & F1 & F2 & F3
 * score = __builtin_popcount(intersection)
 *
 * score >= INTERSECTION_MIN (6) → FAST PATH (all 4 axes agree)
 * score < INTERSECTION_MIN    → SHADOW PATH (uncertainty)
 * ══════════════════════════════════════════════════════════════════ */
typedef uint32_t AnchorMask;

/* build anchor from value via bit scatter */
static inline AnchorMask anchor_build(uint64_t value, int axis_offset)
{
    uint32_t v = (uint32_t)(value >> axis_offset);
    /* scatter bits for better popcount distribution */
    v ^= v >> 16;
    v *= 0x45d9f3bu;
    v ^= v >> 16;
    return (AnchorMask)v;
}

/* smooth anchor update (bitwise average) */
static inline AnchorMask anchor_update(AnchorMask prev, AnchorMask curr)
{
    return (prev & curr) | ((prev ^ curr) >> 1);
}

/* drift detection via popcount of XOR */
static inline int anchor_drift_ok(AnchorMask curr, AnchorMask prev)
{
    return __builtin_popcount(curr ^ prev) <= (int)EVO2_DRIFT_LIMIT;
}

typedef struct {
    AnchorMask f[4];      /* 4 axis anchors (East/West/North/South)    */
    AnchorMask prev[4];   /* previous anchors for drift detection      */
    int        fast_disabled; /* 1 = drift guard triggered             */
    uint32_t   drift_events;
} AnchorState;

static inline void anchor_state_init(AnchorState *a)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

static inline int anchor_process(AnchorState *a,
                                  uint64_t value,
                                  int *score_out)
{
    if (!a) return 0;

    /* build 4 axis views (East/West/North/South offsets) */
    AnchorMask nf[4];
    nf[0] = anchor_build(value,  0);   /* East  */
    nf[1] = anchor_build(value, 16);   /* West  */
    nf[2] = anchor_build(value, 32);   /* North */
    nf[3] = anchor_build(value, 48);   /* South */

    /* drift check */
    int drift_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (a->prev[i] != 0 && !anchor_drift_ok(nf[i], a->prev[i])) {
            drift_ok = 0;
            a->drift_events++;
        }
    }

    if (!drift_ok) {
        a->fast_disabled = 1;
    } else if (a->fast_disabled) {
        a->fast_disabled = 0; /* auto-recover */
    }

    /* update anchors */
    for (int i = 0; i < 4; i++) {
        a->prev[i] = a->f[i];
        a->f[i]    = anchor_update(a->f[i], nf[i]);
    }

    /* intersection = where all 4 agree */
    AnchorMask inter = a->f[0] & a->f[1] & a->f[2] & a->f[3];
    int score = __builtin_popcount(inter);
    if (score_out) *score_out = score;

    if (a->fast_disabled) return 0;
    return score >= (int)EVO2_INTERSECTION_MIN;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2: Ghost Store V2 (epoch aging + rotl32)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t  key;      /* state_hash (0 = empty)                     */
    uint32_t  value;    /* packed: lane(8) | type(8) | score(8) | _   */
    uint16_t  epoch;    /* age tracking (wrap ok)                     */
    uint16_t  pad;
} GhostEntryV2;         /* 16B — one per cache line quarter           */

typedef struct {
    GhostEntryV2  pool[EVO2_GHOST_CAP];
    uint32_t      clock;      /* global epoch counter                  */
    uint64_t      stores;
    uint64_t      hits;
    uint64_t      evictions;
} GhostStoreV2;

/* rotl32 — better entropy than simple XOR */
static inline uint32_t _rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

/* hash function (GPT spec) */
static inline uint32_t ghost_hash_v2(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static inline void ghost_store_v2_init(GhostStoreV2 *gs)
{
    if (!gs) return;
    memset(gs, 0, sizeof(*gs));
}

static inline int ghost_lookup_v2(GhostStoreV2 *gs,
                                    uint64_t key, uint32_t *out)
{
    if (!gs || !out) return 0;
    uint32_t idx = ghost_hash_v2(key) & (EVO2_GHOST_CAP - 1);
    GhostEntryV2 *e = &gs->pool[idx];
    if (e->key == key && e->key != 0) {
        *out = e->value;
        e->epoch = (uint16_t)gs->clock;   /* refresh epoch */
        gs->hits++;
        return 1;
    }
    return 0;
}

static inline void ghost_store_v2(GhostStoreV2 *gs,
                                    uint64_t key, uint32_t val)
{
    if (!gs) return;
    uint32_t idx = ghost_hash_v2(key) & (EVO2_GHOST_CAP - 1);
    GhostEntryV2 *e = &gs->pool[idx];

    uint16_t age = (uint16_t)(gs->clock - e->epoch);

    /* replace if: empty, key match (update), or old */
    if (e->key == 0 || e->key == key || age > (uint16_t)EVO2_GHOST_AGE_MAX) {
        if (e->key != 0 && e->key != key) gs->evictions++;
        e->key   = key;
        e->value = val;
        e->epoch = (uint16_t)gs->clock;
        gs->stores++;
    }
    gs->clock++;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 3: Mandelbrot fp12 + map_to_mandel
 * ══════════════════════════════════════════════════════════════════ */
static inline void map_to_mandel_v2(uint32_t addr,
                                     int32_t *cx, int32_t *cy)
{
    /* GPT spec: addr bits → [-2048,2047] × 1.5 = [-3072,3071]
     * covers Mandelbrot boundary richly                           */
    int32_t x = (int32_t)(addr & 0xFFF) - 2048;
    int32_t y = (int32_t)((addr >> 12) & 0xFFF) - 2048;
    *cx = (x * 3) >> 1;   /* × 1.5 */
    *cy = (y * 3) >> 1;
}

static inline int mandel_v2(int32_t cx, int32_t cy)
{
    int32_t x=0, y=0, x2=0, y2=0;
    for (int i = 0; i < (int)EVO2_MANDEL_ITER; i++) {
        if ((uint32_t)(x2+y2) > (uint32_t)EVO2_ESCAPE_SQ) return 0; /* chaotic */
        y  = ((x * y) >> (EVO2_FP_SHIFT-1)) + cy;
        x  = x2 - y2 + cx;
        x2 = (x*x) >> EVO2_FP_SHIFT;
        y2 = (y*y) >> EVO2_FP_SHIFT;
    }
    return 1; /* stable */
}

/* ══════════════════════════════════════════════════════════════════
 * PART 4: Fibonacci V2 (bitmask mod, no division)
 * ══════════════════════════════════════════════════════════════════ */
static inline void fib_init_v2(uint32_t seed, int32_t *f0, int32_t *f1)
{
    uint32_t s = seed & (EVO2_FIB_CAP - 1);  /* bitmask mod, no division */
    if (s == 0) s = 1;
    *f0 = (int32_t)s;
    *f1 = (int32_t)(s >> 1);
    if (*f1 == 0) *f1 = 1;
}

static inline int32_t fib_step_v2(int32_t *f0, int32_t *f1)
{
    int32_t next = *f0 + *f1;
    /* overflow guard */
    if ((*f0 > 0 && *f1 > 0 && next < 0) ||
        (*f0 < 0 && *f1 < 0 && next > 0)) {
        *f0 >>= 1; *f1 >>= 1; next = *f0 + *f1;
    }
    *f0 = *f1;
    *f1 = next;
    return next;
}

/* ══════════════════════════════════════════════════════════════════
 * EvoV2Context — full system
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    AnchorState   anchor;
    GhostStoreV2  ghost;
    uint32_t      tick;       /* global tick (temporal dimension)      */
    uint32_t      prev_hash;  /* for temporal drift check              */

    /* stats */
    uint64_t  total;
    uint64_t  ghost_hits;
    uint64_t  fast_path;
    uint64_t  shadow_path;
    uint64_t  temporal_ok;

    uint32_t  magic;
} EvoV2Context;

static inline int evo2_init(EvoV2Context *ec)
{
    if (!ec) return -1;
    memset(ec, 0, sizeof(*ec));
    anchor_state_init(&ec->anchor);
    ghost_store_v2_init(&ec->ghost);
    ec->magic = EVO2_MAGIC;
    return 0;
}

/*
 * evo2_process — unified single function (GPT spec)
 *
 * Input:  addr (angular address)
 * Output: packed uint32_t = (lane << 8) | type
 *         lane 0..53, type 0=stable 1=chaotic
 *
 * Pipeline:
 *   state_hash = rotl32(spatial ^ tick, 13)
 *   → ghost L0 lookup
 *   → fib init/step
 *   → mandel classify
 *   → damped feedback (chaotic: +50%, stable: -25%)
 *   → dna bitmask
 *   → bitwise anchor intersection
 *   → temporal drift check
 *   → lane compute
 *   → ghost store
 */
static inline uint32_t evo2_process(EvoV2Context *ec, uint64_t addr)
{
    if (!ec) return 0;
    ec->total++;
    ec->tick++;

    /* ── Time Memory: state_hash ────────────────────────────────── */
    uint32_t spatial = (uint32_t)(addr ^ (addr >> 32));
    /* state_hash keys on spatial (not tick) so same addr = same ghost slot
     * tick used separately for temporal tracking                       */
    uint32_t state_hash = _rotl32(spatial, 13);
    uint32_t time_hash  = state_hash ^ ec->tick;  /* for temporal only  */

    /* ── Ghost L0: fast return ──────────────────────────────────── */
    uint32_t cached;
    if (ghost_lookup_v2(&ec->ghost, state_hash, &cached)) {
        ec->ghost_hits++;
        ec->fast_path++;
        return cached;
    }

    /* ── Fibonacci (bitmask mod, zero-mul) ──────────────────────── */
    int32_t f0, f1;
    fib_init_v2((uint32_t)addr, &f0, &f1);
    int32_t fib = fib_step_v2(&f0, &f1);

    /* ── Mandelbrot classify ─────────────────────────────────────── */
    int32_t cx, cy;
    map_to_mandel_v2((uint32_t)addr, &cx, &cy);
    int type = mandel_v2(cx, cy);   /* 1=stable, 0=chaotic */

    /* ── Damped feedback (GPT spec: asymmetric) ─────────────────── */
    int32_t val = fib;
    if (type == 0) {
        val = val + (val >> 1);   /* chaotic: expand +50% */
    } else {
        val = val - (val >> 2);   /* stable:  collapse -25% */
    }

    /* ── DNA bitmask (not modulo) ───────────────────────────────── */
    uint8_t dna = (uint8_t)(state_hash & 0x3);

    /* ── Bitwise Anchor Intersection ────────────────────────────── */
    int anchor_score = 0;
    int anchor_fast  = anchor_process(&ec->anchor,
                                       (uint64_t)val ^ addr,
                                       &anchor_score);

    /* ── Temporal drift check ────────────────────────────────────── */
    int temporal_ok = 1;
    if (ec->prev_hash != 0) {
        /* drift check on time_hash (varies with tick) */
        temporal_ok = (__builtin_popcount(time_hash ^ ec->prev_hash)
                       <= (int)EVO2_DRIFT_LIMIT);  /* use DRIFT_LIMIT=3 not 2 */
    }
    ec->prev_hash = time_hash;
    if (temporal_ok) ec->temporal_ok++;

    /* ── Route decision ──────────────────────────────────────────── */
    if (anchor_fast && temporal_ok) {
        ec->fast_path++;
    } else {
        ec->shadow_path++;
    }

    /* ── Lane compute (branchless mod via subtract) ─────────────── */
    /* lane spread across all 54 using addr + fib + type */
    uint32_t lane_mix = (uint32_t)(addr % EVO2_LANES)
                      + (uint32_t)(dna * 13u)
                      + (uint32_t)((val < 0 ? -val : val) % EVO2_LANES)
                      + (uint32_t)(type * 7u);
    uint8_t lane = (uint8_t)(lane_mix % EVO2_LANES);

    /* ── Pack result ─────────────────────────────────────────────── */
    uint32_t packed = ((uint32_t)lane << 8) | (uint32_t)(type & 0xFF);
    (void)time_hash; /* used for temporal tracking above */

    /* ── Ghost store ─────────────────────────────────────────────── */
    ghost_store_v2(&ec->ghost, state_hash, packed);

    return packed;
}

/* convenience: extract lane and type from packed result */
static inline uint8_t evo2_lane(uint32_t packed) { return (uint8_t)(packed >> 8); }
static inline uint8_t evo2_type(uint32_t packed) { return (uint8_t)(packed & 0xFF); }

/* stats */
static inline void evo2_stats(const EvoV2Context *ec)
{
    if (!ec) return;
    uint64_t t = ec->total ? ec->total : 1;
    printf("[EvoV2] total=%llu ghost_hit=%llu(%llu%%) "
           "fast=%llu(%llu%%) shadow=%llu(%llu%%)\n"
           "        temporal_ok=%llu drift_events=%u "
           "ghost_evictions=%llu\n",
           (unsigned long long)ec->total,
           (unsigned long long)ec->ghost_hits,
           (unsigned long long)(ec->ghost_hits*100/t),
           (unsigned long long)ec->fast_path,
           (unsigned long long)(ec->fast_path*100/t),
           (unsigned long long)ec->shadow_path,
           (unsigned long long)(ec->shadow_path*100/t),
           (unsigned long long)ec->temporal_ok,
           ec->anchor.drift_events,
           (unsigned long long)ec->ghost.evictions);
}

#endif /* POGLS_EVO_V2_H */
