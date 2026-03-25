/*
 * pogls_canonical.cu — POGLS V4.x GPU Canonical Engine
 * ══════════════════════════════════════════════════════════════════════
 *
 * CORE SHIFT: verify-after → correct-by-construction
 *
 * GPU = canonical engine (source of truth)
 * CPU = scheduler + logic (uses v_clean directly)
 *
 * Pipeline:
 *   v_raw → gpu_canonical(v) → v_clean
 *   v_clean = valid by construction (no verify needed)
 *
 * Canonical Projection:
 *   a = f(v) % 144 → snap to 12-grid  (World A, 12×12=144)
 *   b = g(v) % 144 → snap to 9-grid   (World B, 9×16=144)
 *   x = a²-b²,  y = 2ab,  z = a²+b²  (Pythagorean triple)
 *   output = hash3(x,y,z)             (pack → single value)
 *
 * Sacred numbers respected:
 *   144 = Fib(12) = spatial anchor
 *   12  = World A grid step (12×12=144)
 *   9   = World B grid step (9×16=144)
 *   720 = temporal closure (used by scheduler, step 3)
 *
 * Rules:
 *   - GPU never touches commit path (FROZEN)
 *   - integer only, no float
 *   - PHI constants from pogls_platform.h
 *   - persistent kernel (no relaunch)
 *   - fail → snap, never reject
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_CANONICAL_CU
#define POGLS_CANONICAL_CU

#include <stdint.h>
#include <cuda_runtime.h>

/* ── PHI + anchor constants ───────────────────────────────────────── */
#define CAN_PHI_UP      1696631u
#define CAN_PHI_DOWN    648055u
#define CAN_PHI_SCALE   (1u << 20)

#define CAN_ANCHOR      144u    /* Fib(12) = spatial lock              */
#define CAN_GRID_A      12u     /* World A snap step (12×12=144)       */
#define CAN_GRID_B      9u      /* World B snap step (9×16=144)        */
#define CAN_CYCLE       720u    /* temporal closure                    */

/* compile-time checks */
typedef char _can_a[(CAN_GRID_A * CAN_GRID_A == CAN_ANCHOR)  ? 1 : -1];
typedef char _can_b[(CAN_GRID_B * 16u        == CAN_ANCHOR)  ? 1 : -1];
typedef char _can_c[(CAN_CYCLE  % CAN_ANCHOR == 0)           ? 1 : -1];

/* ── Queue sizes (same as persistent fusion) ──────────────────────── */
#define CAN_Q_SIZE      (1u << 20)
#define CAN_Q_MASK      (CAN_Q_SIZE - 1u)
#define CAN_BP_LIMIT    (CAN_Q_SIZE * 3u / 4u)

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — CANONICAL PROJECTION (DEVICE)
 *
 * f(v) = PHI_UP scatter   → World A projection
 * g(v) = PHI_DOWN scatter → World B projection
 * snap a to 12-grid, b to 9-grid
 * embed in Pythagorean triple → always valid
 * ══════════════════════════════════════════════════════════════════════ */

__device__ __forceinline__
uint32_t can_f(uint32_t v)
{
    /* World A: PHI_UP scatter → [0..143] */
    uint64_t t = (uint64_t)v * CAN_PHI_UP;
    return (uint32_t)((t >> 20) % CAN_ANCHOR);
}

__device__ __forceinline__
uint32_t can_g(uint32_t v)
{
    /* World B: PHI_DOWN scatter → [0..143] */
    uint64_t t = (uint64_t)v * CAN_PHI_DOWN;
    return (uint32_t)((t >> 20) % CAN_ANCHOR);
}

__device__ __forceinline__
uint32_t can_hash3(uint32_t x, uint32_t y, uint32_t z)
{
    /* mix3: pack (x,y,z) into single 32-bit canonical value
     * PHI_DOWN final fold — aligned with POGLS constants           */
    uint64_t h = (uint64_t)x * 2654435761u
               ^ (uint64_t)y * 2246822519u
               ^ (uint64_t)z * 3266489917u;
    h ^= h >> 17;
    h *= (uint64_t)CAN_PHI_DOWN;
    h ^= h >> 31;
    return (uint32_t)(h & 0xFFFFFFFFu);
}

/*
 * gpu_canonicalize — core function
 *
 * Input:  v_raw (any uint32_t)
 * Output: v_clean (valid by construction)
 *
 * Properties:
 *   - a snapped to 12-grid → a ∈ {0,12,24,...,132}
 *   - b snapped to 9-grid  → b ∈ {0,9,18,...,135}
 *   - x²+y² = z² always   (Pythagorean triple, by construction)
 *   - deterministic: same v → same output
 *   - no verify needed
 */
__device__ __forceinline__
uint32_t gpu_canonicalize(uint32_t v)
{
    uint32_t a = can_f(v);
    uint32_t b = can_g(v);

    /* snap to grids */
    a = (a / CAN_GRID_A) * CAN_GRID_A;   /* 0,12,24,...,132 */
    b = (b / CAN_GRID_B) * CAN_GRID_B;   /* 0,9,18,...,135  */

    /* Pythagorean embed: always valid by construction */
    uint32_t x = a * a - b * b;           /* a²-b² */
    uint32_t y = 2u * a * b;              /* 2ab   */
    uint32_t z = a * a + b * b;           /* a²+b² = z (c in triple) */

    /* pack → single canonical value */
    return can_hash3(x, y, z);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — CANONICAL QUERY HELPERS (DEVICE)
 * ══════════════════════════════════════════════════════════════════════ */

/* extract snap components (for debugging / audit) */
__device__ __forceinline__
void gpu_canonical_components(uint32_t v,
                               uint32_t *a_out,
                               uint32_t *b_out,
                               uint32_t *x_out,
                               uint32_t *y_out,
                               uint32_t *z_out)
{
    uint32_t a = (can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (can_g(v) / CAN_GRID_B) * CAN_GRID_B;
    *a_out = a;
    *b_out = b;
    *x_out = a*a - b*b;
    *y_out = 2u*a*b;
    *z_out = a*a + b*b;
}

/* verify: is v already canonical? (x²+y²=z² via hash3) */
__device__ __forceinline__
uint8_t gpu_is_canonical(uint32_t v)
{
    return (gpu_canonicalize(v) == v) ? 1u : 0u;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — PERSISTENT CANONICAL KERNEL
 *
 * launch 1 ครั้ง — runs until d_shutdown = 1
 * Input:  q_in  → raw values
 * Output: q_out → canonical values (always, not fail-only)
 *         (เพราะ output = clean value, not fail flag)
 *
 * warp-cooperative: lane 0 does atomics
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t v_raw;
    uint64_t seq;
} CanTask;           /* 12B (pad to 16) */

typedef struct {
    uint32_t v_clean;   /* canonical output                             */
    uint32_t v_raw;     /* original (for tracing)                       */
    uint64_t seq;
} CanResult;         /* 16B */

typedef struct {
    uint32_t  head;
    uint32_t  tail;
    uint32_t  _pad[14];
    CanTask   buf[CAN_Q_SIZE];
} CanQueueIn;

typedef struct {
    uint32_t  head;
    uint32_t  tail;
    uint32_t  _pad[14];
    CanResult buf[CAN_Q_SIZE];
} CanQueueOut;

__device__ CanQueueIn  *d_can_q_in;
__device__ CanQueueOut *d_can_q_out;
__device__ int          d_can_shutdown;
__device__ uint64_t     d_can_total;   /* monotonic op counter */

#define CAN_WATCHDOG_MAX (1u << 24)

__global__
__launch_bounds__(256, 2)
void pogls_canonical_persistent(void)
{
    const uint32_t lane = threadIdx.x & 31u;
    uint32_t watchdog = 0;

    while (!d_can_shutdown) {

        /* ── fetch (warp-cooperative) ────────────────────────────── */
        uint32_t idx = 0;
        if (lane == 0) {
            uint32_t tail = d_can_q_in->tail;
            uint32_t head = d_can_q_in->head;
            idx = (tail < head)
                ? atomicAdd(&d_can_q_in->tail, 1u)
                : 0xFFFFFFFFu;
        }
        idx = __shfl_sync(0xFFFFFFFFu, idx, 0);

        if (idx == 0xFFFFFFFFu || idx >= d_can_q_in->head) {
            if (++watchdog > CAN_WATCHDOG_MAX) break;
            continue;
        }
        watchdog = 0;

        /* ── read task ───────────────────────────────────────────── */
        uint32_t v_raw = d_can_q_in->buf[idx & CAN_Q_MASK].v_raw;
        uint64_t seq   = d_can_q_in->buf[idx & CAN_Q_MASK].seq;

        /* ── CANONICAL PROJECTION (full fusion inline) ───────────── */
        uint32_t v_clean = gpu_canonicalize(v_raw);

        /* ── push result (all outputs, not fail-only) ────────────── */
        uint32_t out_idx = 0;
        if (lane == 0) {
            out_idx = atomicAdd(&d_can_q_out->head, 1u);
            atomicAdd((unsigned long long*)&d_can_total, 1ULL);
        }
        out_idx = __shfl_sync(0xFFFFFFFFu, out_idx, 0);

        if (lane == 0) {
            CanResult *r = &d_can_q_out->buf[out_idx & CAN_Q_MASK];
            r->v_clean = v_clean;
            r->v_raw   = v_raw;
            r->seq     = seq;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — HOST API
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    CanQueueIn  *h_q_in;
    CanQueueOut *h_q_out;
    CanQueueIn  *d_q_in_ptr;
    CanQueueOut *d_q_out_ptr;
    uint64_t     push_seq;
    int          running;
} CanCtx;

static inline int can_ctx_init(CanCtx *ctx)
{
    if (!ctx) return -1;
    ctx->push_seq = 0;
    ctx->running  = 0;

    cudaError_t e;
    e = cudaHostAlloc(&ctx->h_q_in,  sizeof(CanQueueIn),
                      cudaHostAllocMapped);
    if (e != cudaSuccess) return -1;
    e = cudaHostAlloc(&ctx->h_q_out, sizeof(CanQueueOut),
                      cudaHostAllocMapped);
    if (e != cudaSuccess) { cudaFreeHost(ctx->h_q_in); return -1; }

    memset(ctx->h_q_in,  0, sizeof(CanQueueIn));
    memset(ctx->h_q_out, 0, sizeof(CanQueueOut));

    cudaHostGetDevicePointer(&ctx->d_q_in_ptr,  ctx->h_q_in,  0);
    cudaHostGetDevicePointer(&ctx->d_q_out_ptr, ctx->h_q_out, 0);

    cudaMemcpyToSymbol(d_can_q_in,  &ctx->d_q_in_ptr,  sizeof(void*));
    cudaMemcpyToSymbol(d_can_q_out, &ctx->d_q_out_ptr, sizeof(void*));
    int zero = 0; uint64_t zero64 = 0;
    cudaMemcpyToSymbol(d_can_shutdown, &zero,   sizeof(int));
    cudaMemcpyToSymbol(d_can_total,    &zero64, sizeof(uint64_t));

    int n_sm = 0;
    cudaDeviceGetAttribute(&n_sm, cudaDevAttrMultiProcessorCount, 0);
    pogls_canonical_persistent<<<n_sm * 2, 256>>>();

    ctx->running = 1;
    return 0;
}

/* push v_raw → GPU canonical queue */
static inline int can_push(CanCtx *ctx, uint32_t v_raw)
{
    if (!ctx || !ctx->running) return -1;
    uint32_t depth = ctx->h_q_in->head - ctx->h_q_in->tail;
    if (depth >= CAN_BP_LIMIT) return 1;  /* backpressure */

    uint32_t slot = ctx->h_q_in->head & CAN_Q_MASK;
    ctx->h_q_in->buf[slot].v_raw = v_raw;
    ctx->h_q_in->buf[slot].seq   = ctx->push_seq++;
    __sync_synchronize();
    ctx->h_q_in->head++;
    return 0;
}

/* pop v_clean (non-blocking) */
static inline int can_pop(CanCtx *ctx, CanResult *out)
{
    if (!ctx || !out) return 0;
    if (ctx->h_q_out->tail >= ctx->h_q_out->head) return 0;
    uint32_t slot = ctx->h_q_out->tail & CAN_Q_MASK;
    *out = ctx->h_q_out->buf[slot];
    __sync_synchronize();
    ctx->h_q_out->tail++;
    return 1;
}

/* drain: wait until all pushed values have been canonicalized */
static inline void can_drain(CanCtx *ctx)
{
    if (!ctx) return;
    while (ctx->h_q_in->tail < ctx->h_q_in->head) { /* spin */ }
}

static inline void can_shutdown(CanCtx *ctx)
{
    if (!ctx || !ctx->running) return;
    int one = 1;
    cudaMemcpyToSymbol(d_can_shutdown, &one, sizeof(int));
    cudaDeviceSynchronize();
    cudaFreeHost(ctx->h_q_in);
    cudaFreeHost(ctx->h_q_out);
    ctx->running = 0;
}

#ifdef __cplusplus
}
#endif

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — CPU MIRROR (for testing without GPU)
 *
 * Identical logic to GPU kernel — used in:
 *   - unit tests (no CUDA needed)
 *   - CPU fallback path
 *   - temporal core step 3 (validate snap)
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t cpu_can_f(uint32_t v)
{
    return (uint32_t)(((uint64_t)v * CAN_PHI_UP >> 20) % CAN_ANCHOR);
}

static inline uint32_t cpu_can_g(uint32_t v)
{
    return (uint32_t)(((uint64_t)v * CAN_PHI_DOWN >> 20) % CAN_ANCHOR);
}

static inline uint32_t cpu_hash3(uint32_t x, uint32_t y, uint32_t z)
{
    uint64_t h = (uint64_t)x * 2654435761u
               ^ (uint64_t)y * 2246822519u
               ^ (uint64_t)z * 3266489917u;
    h ^= h >> 17;
    h *= (uint64_t)CAN_PHI_DOWN;
    h ^= h >> 31;
    return (uint32_t)(h & 0xFFFFFFFFu);
}

static inline uint32_t cpu_canonicalize(uint32_t v)
{
    uint32_t a = (cpu_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (cpu_can_g(v) / CAN_GRID_B) * CAN_GRID_B;
    uint32_t x = a*a - b*b;
    uint32_t y = 2u*a*b;
    uint32_t z = a*a + b*b;
    return cpu_hash3(x, y, z);
}

/* verify canonical properties (for testing) */
static inline int cpu_canonical_verify(uint32_t v,
                                        uint32_t *a_out,
                                        uint32_t *b_out)
{
    uint32_t a = (cpu_can_f(v) / CAN_GRID_A) * CAN_GRID_A;
    uint32_t b = (cpu_can_g(v) / CAN_GRID_B) * CAN_GRID_B;
    if (a_out) *a_out = a;
    if (b_out) *b_out = b;

    /* a on 12-grid */
    if (a % CAN_GRID_A != 0) return 0;
    /* b on 9-grid */
    if (b % CAN_GRID_B != 0) return 0;
    /* Pythagorean: (a²-b²)² + (2ab)² = (a²+b²)² */
    uint64_t x = (uint64_t)a*a - (uint64_t)b*b;
    uint64_t y = 2ULL*a*b;
    uint64_t z = (uint64_t)a*a + (uint64_t)b*b;
    if (x*x + y*y != z*z) return 0;
    return 1;
}

#endif /* POGLS_CANONICAL_CU */
