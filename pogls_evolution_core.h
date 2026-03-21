/*
 * pogls_evolution_core.h — POGLS V3.95  Evolution Core
 * ══════════════════════════════════════════════════════════════════════
 *
 * Implements the 3 missing pieces GPT identified:
 *
 *   1. Fibonacci Additive Recurrence (ไม่ใช่ linear scale)
 *      f0 + f1 rolling 2 registers → zero-mul, pure add
 *      = growth + memory + fractal boundary
 *
 *   2. Mandelbrot Feedback Loop → Scaffold
 *      stable  → val >>= 1  (collapse)  = End of Cycle
 *      chaotic → val <<= 1  (expand)    = Multiplier Effect
 *      balance → val unchanged           = Sustainable Evolution
 *
 *   3. Time Memory (temporal dimension)
 *      state_hash = (spatial_index ^ global_tick) & 0xFFFFFFFF
 *      = pattern recurrence + temporal compression
 *      wires into rewind.h epoch system
 *
 * Pipeline binding:
 *   evo_process() → uint8_t lane (0..53)
 *   → plug into pipeline_wire_process() as pre-routing step
 *
 * Sacred Numbers:
 *   FP_SHIFT = 12   (fixed-point balance)
 *   FIB_SEED = 162  (NODE_MAX = 2×3⁴)
 *   LANES    = 54   (2×3³ = nexus)
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_EVOLUTION_CORE_H
#define POGLS_EVOLUTION_CORE_H
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

/* ── constants ───────────────────────────────────────────────────── */
#define EVO_FP_SHIFT      12u     /* fixed-point: all ops use >> 12   */
#define EVO_FIB_SEED      162u    /* NODE_MAX — Fibonacci anchor       */
#define EVO_LANES         54u     /* Rubik lanes                       */
#define EVO_MANDEL_ITER   32u     /* enough for boundary detection     */
#define EVO_ESCAPE_SQ     (4u << EVO_FP_SHIFT)  /* |z|² > 4 in fp12  */
#define EVO_MAGIC    0x45564F43u  /* "EVOC"                            */

/* ══════════════════════════════════════════════════════════════════
 * PART 1: Fibonacci Additive Recurrence
 *
 * NOT: val * 1655801 >> shift  (linear — no fractal memory)
 * YES: f0 + f1  (rolling 2 registers — zero-mul, pure add)
 *
 * Property:
 *   growth    = f grows with golden ratio spacing
 *   memory    = each value depends on 2 previous
 *   boundary  = natural thresholds at fib numbers
 *   zero-mul  = no multiply, fast on any CPU
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    int32_t f0;   /* previous value                                    */
    int32_t f1;   /* current value                                     */
    uint32_t steps; /* how many steps taken                            */
} FibState;

static inline void fib_init(FibState *f, int32_t seed)
{
    if (!f) return;
    /* clamp seed to [0, EVO_FIB_SEED] to prevent oscillation */
    int32_t s = seed < 0 ? -seed : seed;
    if (s > (int32_t)EVO_FIB_SEED) s = s % (int32_t)EVO_FIB_SEED + 1;
    f->f0    = s;
    f->f1    = s >> 1;
    f->steps = 0;
}

/*
 * fib_next — one additive step
 * Returns next Fibonacci value
 * Handles overflow by halving both registers (keeps in bounds)
 */
static inline int32_t fib_next(FibState *f)
{
    if (!f) return 0;
    int32_t next = f->f0 + f->f1;

    /* overflow guard: if sign flips, halve both */
    if ((f->f0 > 0 && f->f1 > 0 && next < 0) ||
        (f->f0 < 0 && f->f1 < 0 && next > 0)) {
        f->f0 >>= 1;
        f->f1 >>= 1;
        next = f->f0 + f->f1;
    }

    f->f0    = f->f1;
    f->f1    = next;
    f->steps++;
    return next;
}

/*
 * fib_boundary — returns which Fibonacci region val falls in (0..8)
 * Boundary numbers: 1,2,3,5,8,13,21,34,55,89,144,233
 * Scaled by seed (162 = NODE_MAX)
 */
static inline uint8_t fib_boundary(int32_t val, int32_t seed)
{
    static const uint16_t FIB[9] = {1,2,3,5,8,13,21,34,55};
    int32_t abs_val = val < 0 ? -val : val;
    int32_t abs_seed = seed < 0 ? -seed : seed;
    if (abs_seed == 0) abs_seed = 1;

    for (int i = 8; i >= 0; i--) {
        if (abs_val >= (int32_t)FIB[i] * abs_seed / 10)
            return (uint8_t)i;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2: Mandelbrot Feedback → Scaffold
 *
 * Fixed-point Mandelbrot with FP_SHIFT=12 (uniform scaling)
 *   x² + y² use >> 12 consistently (no mismatch)
 *
 * Feedback:
 *   stable  (iter == MAX)      → collapse: val >>= 1
 *   chaotic (iter < THRESH)    → expand:   val <<= 1
 *   boundary (else)            → unchanged
 *
 * = The Multiplier Effect + End of Cycle + Sustainable Evolution
 * ══════════════════════════════════════════════════════════════════ */

typedef enum {
    EVO_STABLE   = 0,   /* inside Mandelbrot set  → collapse          */
    EVO_BOUNDARY = 1,   /* edge of set            → sustain           */
    EVO_CHAOTIC  = 2,   /* outside set            → expand            */
} EvoState;

/*
 * mandel_fp12 — fixed-point Mandelbrot with uniform FP_SHIFT=12
 * Returns iteration count (0..EVO_MANDEL_ITER)
 */
static inline uint32_t mandel_fp12(int32_t cx, int32_t cy)
{
    int32_t x = 0, y = 0;
    int32_t x2 = 0, y2 = 0;

    for (uint32_t i = 0; i < EVO_MANDEL_ITER; i++) {
        /* escape check: x² + y² > 4 in fp12 */
        if ((uint32_t)(x2 + y2) > EVO_ESCAPE_SQ) return i;

        /* z = z² + c (all ops >> 12 uniformly) */
        y  = ((x * y) >> (EVO_FP_SHIFT - 1)) + cy;  /* 2xy >> 11 + cy */
        x  = x2 - y2 + cx;
        x2 = (x * x) >> EVO_FP_SHIFT;
        y2 = (y * y) >> EVO_FP_SHIFT;
    }
    return EVO_MANDEL_ITER;
}

/*
 * mandel_classify — classify point into EvoState
 */
static inline EvoState mandel_classify(int32_t cx, int32_t cy)
{
    uint32_t iter = mandel_fp12(cx, cy);
    if (iter == EVO_MANDEL_ITER) return EVO_STABLE;
    if (iter < 4)                 return EVO_CHAOTIC;
    return EVO_BOUNDARY;
}

/*
 * evo_feedback — apply Mandelbrot result back to scaffold value
 *
 * GPT's feedback loop:
 *   stable  → val >>= 1  (collapse — End of Cycle)
 *   chaotic → val <<= 1  (expand   — Multiplier Effect)
 *   boundary → unchanged (sustain  — Sustainable Evolution)
 */
static inline int32_t evo_feedback(int32_t val, EvoState state)
{
    switch (state) {
    case EVO_STABLE:   return val >> 1;   /* collapse */
    case EVO_CHAOTIC:  return val << 1;   /* expand   */
    case EVO_BOUNDARY: return val;        /* sustain  */
    }
    return val;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 3: Time Memory
 *
 * state_hash = (spatial_index ^ global_tick) & 0xFFFFFFFF
 *
 * Properties:
 *   pattern recurrence  — same spatial + same time → same hash
 *   temporal compression — tick wraps → patterns repeat periodically
 *   predictive behavior — hash predicts next access pattern
 *
 * Wires into rewind.h epoch:
 *   global_tick = epoch × REWIND_MAX + slot
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t global_tick;   /* increments every process() call        */
    uint32_t epoch;         /* rewind epoch (from rewind.h)           */
    uint32_t last_hash;     /* previous state_hash (for recurrence)   */
    uint32_t recurrence;    /* count: how many times hash repeated    */
} TimeMemory;

static inline void tmem_init(TimeMemory *tm) {
    if (!tm) return;
    memset(tm, 0, sizeof(*tm));
}

static inline uint32_t tmem_hash(TimeMemory *tm, uint64_t spatial_index)
{
    if (!tm) return 0;
    tm->global_tick++;

    uint32_t h = (uint32_t)(spatial_index ^ (uint64_t)tm->global_tick);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;

    /* track recurrence */
    if (h == tm->last_hash) tm->recurrence++;
    else tm->recurrence = 0;
    tm->last_hash = h;

    return h & 0xFFFFFFFFu;
}

/* set epoch from rewind.h */
static inline void tmem_set_epoch(TimeMemory *tm, uint32_t epoch,
                                   uint32_t slot)
{
    if (!tm) return;
    tm->epoch      = epoch;
    tm->global_tick = epoch * 972u + slot;  /* REWIND_MAX = 972 */
}

/* ══════════════════════════════════════════════════════════════════
 * GhostSlab — minimal ghost store (16B per entry, no malloc)
 *
 * Replaces "simplified — full impl would use slab allocator"
 * Fixed-size pool: 1024 entries × 16B = 16KB (L1 friendly)
 * Hash-addressed: sig & (GHOST_SLAB_SIZE-1)
 * ══════════════════════════════════════════════════════════════════ */
#define GHOST_SLAB_SIZE   1024u   /* power of 2 for fast mask          */
#define GHOST_SLAB_MASK   (GHOST_SLAB_SIZE - 1)

typedef struct __attribute__((aligned(16))) {
    uint64_t  sig;     /* signature                                    */
    uint64_t  val;     /* stored value                                 */
} GhostSlot;  /* 16B = 1 cache entry */

typedef struct {
    GhostSlot slots[GHOST_SLAB_SIZE];  /* 16KB L1-resident             */
    uint64_t  hits;
    uint64_t  stores;
    uint64_t  evictions;
} GhostSlab;

static inline void ghost_slab_init(GhostSlab *gs) {
    if (!gs) return;
    memset(gs, 0, sizeof(*gs));
}

static inline void ghost_slab_store(GhostSlab *gs,
                                     uint64_t sig, uint64_t val)
{
    if (!gs) return;
    uint32_t idx = (uint32_t)(sig & GHOST_SLAB_MASK);
    if (gs->slots[idx].sig != 0 && gs->slots[idx].sig != sig)
        gs->evictions++;
    gs->slots[idx].sig = sig;
    gs->slots[idx].val = val;
    gs->stores++;
}

static inline int ghost_slab_lookup(GhostSlab *gs,
                                     uint64_t sig, uint64_t *out)
{
    if (!gs || !out) return 0;
    uint32_t idx = (uint32_t)(sig & GHOST_SLAB_MASK);
    if (gs->slots[idx].sig == sig && sig != 0) {
        *out = gs->slots[idx].val;
        gs->hits++;
        return 1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * Evolution Core — combines all 3
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    FibState    fib;
    TimeMemory  tmem;
    GhostSlab   ghost;   /* 16KB slab — real ghost store              */

    /* stats */
    uint64_t  stable_count;
    uint64_t  chaotic_count;
    uint64_t  boundary_count;
    uint64_t  total;

    uint32_t  magic;
} EvoCore;

static inline int evo_init(EvoCore *ec, int32_t seed)
{
    if (!ec) return -1;
    memset(ec, 0, sizeof(*ec));
    fib_init(&ec->fib, seed ? seed : (int32_t)EVO_FIB_SEED);
    tmem_init(&ec->tmem);
    ghost_slab_init(&ec->ghost);
    ec->magic = EVO_MAGIC;
    return 0;
}

/*
 * evo_process — THE main function
 *
 * Input:  angular_addr (uint64_t)
 * Output: lane (0..53) for delta write
 *
 * Pipeline:
 *   1. Fibonacci recurrence → scaffold val
 *   2. Time Memory → state_hash
 *   3. Map addr → cx, cy (fp12)
 *   4. Mandelbrot classify → EvoState
 *   5. Feedback → val adjusted
 *   6. DNA mask → bitmask (not modulo!)
 *   7. lane = (dna ^ fib_boundary) % 54
 */
static inline uint8_t evo_process(EvoCore *ec, uint64_t angular_addr)
{
    if (!ec) return 0;
    ec->total++;

    /* Step 0: Ghost slab check (fast path ~16ns) */
    uint64_t gsig = angular_addr ^ (angular_addr >> 32);
    uint64_t gcached = 0;
    if (ghost_slab_lookup(&ec->ghost, gsig, &gcached)) {
        ec->total++;
        return (uint8_t)(gcached % EVO_LANES);
    }

    /* Step 1: Fibonacci recurrence (zero-mul) */
    int32_t val = fib_next(&ec->fib);

    /* Step 2: Time Memory */
    uint32_t t_hash = tmem_hash(&ec->tmem, angular_addr);

    /* Step 3: Map addr → cx, cy in fp12 fixed-point
     * Full Mandelbrot window [-2,2]×[-2,2] in fp12 = [-8192,8192]
     * Use 12-bit slices for full range coverage             */
    /* Scale to full Mandelbrot window [-2,2] in fp12
     * fp12 unit = 1/4096, so 2.0 = 8192
     * addr 12-bit [0,4095] × 4 - 8192 → [-8192, 8192] = [-2,2] */
    int32_t cx = (int32_t)((angular_addr & 0xFFF) * 4) - 8192;
    int32_t cy = (int32_t)(((angular_addr >> 12) & 0xFFF) * 4) - 8192;

    /* Step 4: Mandelbrot classify */
    EvoState state = mandel_classify(cx, cy);

    switch (state) {
    case EVO_STABLE:   ec->stable_count++;   break;
    case EVO_CHAOTIC:  ec->chaotic_count++;  break;
    case EVO_BOUNDARY: ec->boundary_count++; break;
    }

    /* Step 5: Feedback → val adjusted */
    val = evo_feedback(val, state);

    /* Step 6: DNA mask — bitmask not modulo (GPT fix) */
    uint8_t dna = (uint8_t)((t_hash ^ (uint32_t)val) & 0x3);  /* & 0x3 not % 4 */

    /* Step 7: fib_boundary for lane distribution */
    uint8_t boundary = fib_boundary(val, (int32_t)EVO_FIB_SEED);

    /* lane = deterministic, 0..53 */
    uint8_t lane = (uint8_t)((dna * 13u + boundary * 7u +
                              (uint8_t)(angular_addr % EVO_LANES))
                              % EVO_LANES);
    /* Store in ghost slab for next hit */
    ghost_slab_store(&ec->ghost, gsig, (uint64_t)lane);

    return lane;
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void evo_stats(const EvoCore *ec)
{
    if (!ec) return;
    uint64_t t = ec->total ? ec->total : 1;
    printf("[EvoCore] total=%llu ghost_hit=%llu(%llu%%) "
           "stable=%llu chaotic=%llu boundary=%llu\n",
           (unsigned long long)ec->total,
           (unsigned long long)ec->ghost.hits,
           (unsigned long long)(ec->ghost.hits*100/(t+1)),
           (unsigned long long)ec->stable_count,
           (unsigned long long)ec->chaotic_count,
           (unsigned long long)ec->boundary_count);
    (void)t;
    // original stats
    printf("[EvoCore-detail] total=%llu stable=%llu(%llu%%) "
           "chaotic=%llu(%llu%%) boundary=%llu(%llu%%)\n",
           (unsigned long long)ec->total,
           (unsigned long long)ec->stable_count,
           (unsigned long long)(ec->stable_count*100/t),
           (unsigned long long)ec->chaotic_count,
           (unsigned long long)(ec->chaotic_count*100/t),
           (unsigned long long)ec->boundary_count,
           (unsigned long long)(ec->boundary_count*100/t));
}

#endif /* POGLS_EVOLUTION_CORE_H */
