/*
 * pogls_face_sleep.h — POGLS V3.8  Face Hibernation
 * ══════════════════════════════════════════════════════════════════════
 *
 * Idle face → sleep → คืน RAM ให้ face ที่ทำงานหนัก
 * Wake เมื่อถูกเรียกใช้ (page fault หรือ explicit wake)
 *
 * ══════════════════════════════════════════════════════════════════════
 * กลไก:
 *
 *   face_sleep(node_id)
 *     → madvise(page, MADV_FREE)   ← บอก OS ว่า "คืน RAM ได้"
 *     → set sleep_mask bit          ← track state
 *     → physical RAM ว่าง ทันที
 *     → virtual address ยังอยู่ (ไม่แตะ topology)
 *
 *   face_wake(node_id)
 *     → clear sleep_mask bit
 *     → touch page → OS โหลดกลับ (zero-filled หรือ swap in)
 *     → พร้อมใช้งาน
 *
 * Rules:
 *   - ห้าม sleep node ที่ dirty (delta pending)
 *   - ต้อง wake ก่อน split (topo.h rule)
 *   - n=8 anchor nodes ห้าม sleep (FROZEN)
 *   - sleep_mask เป็น NodeMask (256-bit ครอบ 162 nodes)
 *
 * Integration:
 *   AdaptEngine idle_heads_pct สูง + node density ต่ำ
 *   → face_sleep_idle_nodes() อัตโนมัติ
 *
 * ══════════════════════════════════════════════════════════════════════
 * Memory saving per node:
 *   attention[162]  = 162×8 = 1296B
 *   density[162]    = 162×8 = 1296B
 *   timestamp[162]  = 162×8 = 1296B
 *   total NodeState ≈ 21KB
 *   1 sleeping node ≈ 21KB/162 ≈ 130B saved from OS perspective
 *   162 sleeping nodes = ~21KB freed to hot nodes
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_FACE_SLEEP_H
#define POGLS_FACE_SLEEP_H

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#if defined(__linux__) || defined(__APPLE__)
  #include <sys/mman.h>
  /* use MADV_DONTNEED — supported everywhere, frees physical pages */
  #ifndef MADV_DONTNEED
    #define MADV_DONTNEED 4
  #endif
  #ifndef MADV_WILLNEED
    #define MADV_WILLNEED 3
  #endif
  #define FACE_SLEEP_MADV_FREE  MADV_DONTNEED
  #define FACE_WAKE_MADV        MADV_WILLNEED
  #define FACE_HAS_MADVISE      1
#else
  #define FACE_HAS_MADVISE      0
#endif

/* ── dependencies (forward decl for standalone compile) ──────────── */
#ifndef NODE_MAX
  #define NODE_MAX 162
#endif
#ifndef NODE_MASK_WORDS
  #define NODE_MASK_WORDS 4
#endif

typedef struct { uint64_t w[NODE_MASK_WORDS]; } FaceSleepMask;

/* ── error codes ─────────────────────────────────────────────────── */
#define FACE_OK             0
#define FACE_ERR_NULL      -1
#define FACE_ERR_RANGE     -2    /* node_id >= NODE_MAX */
#define FACE_ERR_DIRTY     -3    /* node has pending delta */
#define FACE_ERR_ANCHOR    -4    /* anchor node (FROZEN) */
#define FACE_ERR_ALREADY   -5    /* already sleeping / already awake */
#define FACE_ERR_MADVISE   -6    /* madvise() failed */

/* ── anchor nodes (n=8, FROZEN — must never sleep) ───────────────── */
#define FACE_ANCHOR_STRIDE  (NODE_MAX / 8)   /* every 20th node       */

static inline int face_is_anchor(uint32_t node_id)
{
    /* anchor nodes: 0, 20, 40, 60, 80, 100, 120, 140 (8 anchors) */
    return node_id < NODE_MAX && (node_id % FACE_ANCHOR_STRIDE) == 0;
}

/* ══════════════════════════════════════════════════════════════════
 * FaceSleepCtx — sleep controller (one per system)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    FaceSleepMask  sleeping;       /* nodes currently sleeping         */
    FaceSleepMask  dirty;          /* nodes with pending delta         */

    uint32_t       sleep_count;    /* nodes currently sleeping         */
    uint32_t       wake_count;     /* total wake() calls               */
    uint32_t       total_sleeps;   /* total sleep() calls ever         */
    uint32_t       total_wakes;    /* total wake() calls ever          */
    uint64_t       bytes_freed;    /* estimated bytes returned to OS   */

    uint32_t       magic;
} FaceSleepCtx;

#define FACE_SLEEP_MAGIC  0x534C5045u  /* "SLPE" */

/* bytes per node (estimate for stats) */
#define FACE_BYTES_PER_NODE  130u

/* ── mask helpers ────────────────────────────────────────────────── */
static inline void _fsm_set(FaceSleepMask *m, uint32_t n)
{ m->w[n>>6] |= (1ULL<<(n&63)); }

static inline void _fsm_clr(FaceSleepMask *m, uint32_t n)
{ m->w[n>>6] &= ~(1ULL<<(n&63)); }

static inline int _fsm_get(const FaceSleepMask *m, uint32_t n)
{ return (int)((m->w[n>>6] >> (n&63)) & 1); }

static inline int _fsm_empty(const FaceSleepMask *m)
{ return !(m->w[0]|m->w[1]|m->w[2]|m->w[3]); }

/* ══════════════════════════════════════════════════════════════════
 * face_sleep_init
 * ══════════════════════════════════════════════════════════════════ */
static inline int face_sleep_init(FaceSleepCtx *ctx)
{
    if (!ctx) return FACE_ERR_NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = FACE_SLEEP_MAGIC;
    return FACE_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * face_mark_dirty / face_mark_clean
 *   call from delta_append path
 * ══════════════════════════════════════════════════════════════════ */
static inline void face_mark_dirty(FaceSleepCtx *ctx, uint32_t node_id)
{
    if (!ctx || node_id >= NODE_MAX) return;
    _fsm_set(&ctx->dirty, node_id);
}

static inline void face_mark_clean(FaceSleepCtx *ctx, uint32_t node_id)
{
    if (!ctx || node_id >= NODE_MAX) return;
    _fsm_clr(&ctx->dirty, node_id);
}

/* ══════════════════════════════════════════════════════════════════
 * face_sleep — hibernate one node
 *
 *   node_base : pointer to start of NodeState data for this node
 *               (e.g. &ns->attention[node_id] — the hot field)
 *   node_bytes: size of data to release (typically FACE_BYTES_PER_NODE
 *               or full stride across SoA arrays)
 * ══════════════════════════════════════════════════════════════════ */
static inline int face_sleep(FaceSleepCtx *ctx,
                              uint32_t      node_id,
                              void         *node_base,
                              size_t        node_bytes)
{
    if (!ctx)               return FACE_ERR_NULL;
    if (node_id >= NODE_MAX) return FACE_ERR_RANGE;
    if (face_is_anchor(node_id)) return FACE_ERR_ANCHOR;
    if (_fsm_get(&ctx->dirty,   node_id)) return FACE_ERR_DIRTY;
    if (_fsm_get(&ctx->sleeping, node_id)) return FACE_ERR_ALREADY;

    /* tell OS: this memory can be reclaimed */
#if FACE_HAS_MADVISE
    if (node_base && node_bytes > 0) {
        if (madvise(node_base, node_bytes, FACE_SLEEP_MADV_FREE) != 0)
            return FACE_ERR_MADVISE;
    }
#endif

    _fsm_set(&ctx->sleeping, node_id);
    ctx->sleep_count++;
    ctx->total_sleeps++;
    ctx->bytes_freed += FACE_BYTES_PER_NODE;
    return FACE_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * face_wake — wake one node
 *
 *   touches the page → OS loads it back (zero or swap)
 *   MUST call before split, detach, or any write to node
 * ══════════════════════════════════════════════════════════════════ */
static inline int face_wake(FaceSleepCtx *ctx,
                             uint32_t      node_id,
                             void         *node_base,
                             size_t        node_bytes)
{
    if (!ctx)                return FACE_ERR_NULL;
    if (node_id >= NODE_MAX) return FACE_ERR_RANGE;
    if (!_fsm_get(&ctx->sleeping, node_id)) return FACE_ERR_ALREADY;

#if FACE_HAS_MADVISE
    /* MADV_WILLNEED = prefetch back into RAM */
    if (node_base && node_bytes > 0)
        madvise(node_base, node_bytes, FACE_WAKE_MADV);
#else
    /* fallback: touch page to trigger load */
    if (node_base && node_bytes > 0) {
        volatile uint8_t *p = (volatile uint8_t *)node_base;
        volatile uint8_t touch = *p; (void)touch;
    }
#endif

    _fsm_clr(&ctx->sleeping, node_id);
    if (ctx->sleep_count > 0) ctx->sleep_count--;
    ctx->total_wakes++;
    ctx->wake_count++;
    if (ctx->bytes_freed >= FACE_BYTES_PER_NODE)
        ctx->bytes_freed -= FACE_BYTES_PER_NODE;
    return FACE_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * face_is_sleeping — query sleep state
 * ══════════════════════════════════════════════════════════════════ */
static inline int face_is_sleeping(const FaceSleepCtx *ctx,
                                    uint32_t node_id)
{
    if (!ctx || node_id >= NODE_MAX) return 0;
    return _fsm_get(&ctx->sleeping, node_id);
}

/* ══════════════════════════════════════════════════════════════════
 * face_sleep_idle_nodes — auto-sleep nodes with low density
 *
 *   density[]   : NodeState.density array (162 entries)
 *   threshold   : density <= threshold → candidate for sleep
 *   base_ptr    : pointer to NodeState base (for madvise)
 *   stride      : bytes per node in SoA layout
 *
 *   Returns: number of nodes put to sleep
 *
 *   Called by AdaptEngine when idle_heads_pct > 70%
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t face_sleep_idle_nodes(FaceSleepCtx  *ctx,
                                              const uint64_t *density,
                                              uint64_t        threshold,
                                              uint8_t        *base_ptr,
                                              size_t          stride)
{
    if (!ctx || !density) return 0;
    uint32_t slept = 0;
    for (uint32_t i = 0; i < NODE_MAX; i++) {
        if (face_is_anchor(i))           continue;
        if (_fsm_get(&ctx->sleeping, i)) continue;
        if (_fsm_get(&ctx->dirty,    i)) continue;
        if (density[i] > threshold)      continue;
        void *node_ptr = base_ptr ? (base_ptr + i * stride) : NULL;
        if (face_sleep(ctx, i, node_ptr, stride) == FACE_OK)
            slept++;
    }
    return slept;
}

/* ══════════════════════════════════════════════════════════════════
 * face_wake_all — wake all sleeping nodes (e.g. before shutdown)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t face_wake_all(FaceSleepCtx *ctx,
                                      uint8_t      *base_ptr,
                                      size_t        stride)
{
    if (!ctx) return 0;
    uint32_t woken = 0;
    for (uint32_t i = 0; i < NODE_MAX; i++) {
        if (!_fsm_get(&ctx->sleeping, i)) continue;
        void *node_ptr = base_ptr ? (base_ptr + i * stride) : NULL;
        if (face_wake(ctx, i, node_ptr, stride) == FACE_OK)
            woken++;
    }
    return woken;
}

/* ══════════════════════════════════════════════════════════════════
 * face_sleep_stats — print summary
 * ══════════════════════════════════════════════════════════════════ */
static inline void face_sleep_stats(const FaceSleepCtx *ctx)
{
    if (!ctx) return;
    printf("[FaceSleep] sleeping=%u  total_sleeps=%u  total_wakes=%u"
           "  bytes_freed≈%lluB\n",
           ctx->sleep_count, ctx->total_sleeps, ctx->total_wakes,
           (unsigned long long)ctx->bytes_freed);
}

/* ══════════════════════════════════════════════════════════════════
 * Integration hook for topo_split (topo.h)
 *
 * Call at top of topo_split_node() before any topology change:
 *   FACE_WAKE_BEFORE_SPLIT(face_ctx, node_id, base, stride)
 * ══════════════════════════════════════════════════════════════════ */
#define FACE_WAKE_BEFORE_SPLIT(ctx, node_id, base, stride) \
    do { \
        if ((ctx) && face_is_sleeping((ctx), (node_id))) \
            face_wake((ctx), (node_id), (base), (stride)); \
    } while(0)

#endif /* POGLS_FACE_SLEEP_H */
