/*
 * pogls_hydra_weight.h — POGLS V3.9  Hydra Weighted Routing
 * ══════════════════════════════════════════════════════════════════════
 *
 * เพิ่ม 2 อย่างที่ DeepSeek เสนอและยังไม่มีใน codebase:
 *
 *   1. iter_history[4] — ring buffer เก็บ fractal iteration history
 *      ต่อ face → avg_iter → split depth decision
 *
 *   2. HydraWeight — activity-based weight per head
 *      weighted routing: distance + weight → best head
 *
 * ══════════════════════════════════════════════════════════════════════
 * Integration:
 *   Step 1c (FaceState tick) → iter_history_push() → avg_iter
 *   Step 8  (AdaptTopo)     → hydra_weight_update() → route
 *   hs_route_addr() replaced by hs_route_weighted()
 *
 * DNA alignment:
 *   history depth = 4 = 2² (binary root)
 *   weight decay  = 7/8 (same as activity, bit-shift)
 *   PHI routing   = (addr × PHI_UP) >> 20 (same as angular addr)
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_HYDRA_WEIGHT_H
#define POGLS_HYDRA_WEIGHT_H

#include <stdint.h>
#include <string.h>

#ifndef HS_HEADS
  #define HS_HEADS 16
#endif
#ifndef NODE_MAX
  #define NODE_MAX 162
#endif
#ifndef FRACTAL_PHI_SCALE
  #define FRACTAL_PHI_SCALE (1u<<20)
#endif

/* ══════════════════════════════════════════════════════════════════
 * PART 1: IterHistory — fractal iteration ring buffer per face
 *
 * Stores last 4 fractal_gate_check() results per node.
 * avg_iter = (sum of 4) >> 2  ← zero-division, zero-float
 *
 * Use: feed into fractal_split_depth() for smoother split decisions
 *      prevents single-spike split/merge oscillation
 * ══════════════════════════════════════════════════════════════════ */

#define ITER_HISTORY_DEPTH  4u    /* 2² — binary root                  */
#define ITER_HISTORY_MASK   3u    /* mod 4 via bitmask                  */

typedef struct {
    uint32_t  buf[ITER_HISTORY_DEPTH];  /* ring buffer                  */
    uint8_t   idx;                       /* next write index (0..3)      */
    uint8_t   full;                      /* 1 when all 4 slots filled    */
    uint16_t  _pad;
} IterHistory;

/* push new iteration count */
static inline void iter_history_push(IterHistory *h, uint32_t iter)
{
    if (!h) return;
    h->buf[h->idx] = iter;
    h->idx = (uint8_t)((h->idx + 1u) & ITER_HISTORY_MASK);
    if (!h->full && h->idx == 0) h->full = 1;
}

/* get average — right-shift by 2 (zero-float) */
static inline uint32_t iter_history_avg(const IterHistory *h)
{
    if (!h) return 0;
    return (h->buf[0] + h->buf[1] + h->buf[2] + h->buf[3]) >> 2;
}

/* get max — for conservative split decisions */
static inline uint32_t iter_history_max(const IterHistory *h)
{
    if (!h) return 0;
    uint32_t m = h->buf[0];
    for (int i = 1; i < (int)ITER_HISTORY_DEPTH; i++)
        if (h->buf[i] > m) m = h->buf[i];
    return m;
}

/* is history stable? (max - min < threshold) */
static inline int iter_history_stable(const IterHistory *h, uint32_t thresh)
{
    if (!h || !h->full) return 0;
    uint32_t mn = h->buf[0], mx = h->buf[0];
    for (int i = 1; i < (int)ITER_HISTORY_DEPTH; i++) {
        if (h->buf[i] < mn) mn = h->buf[i];
        if (h->buf[i] > mx) mx = h->buf[i];
    }
    return (mx - mn) <= thresh;
}

/* IterHistory table — one per node */
typedef struct {
    IterHistory nodes[NODE_MAX];
    uint32_t    magic;
} IterHistoryTable;

#define ITER_HISTORY_MAGIC  0x49544852u  /* "ITHR" */

static inline int iter_table_init(IterHistoryTable *t)
{
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    t->magic = ITER_HISTORY_MAGIC;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2: HydraWeight — activity weight per head
 *
 * weight decays 7/8 per tick (same as FaceState activity)
 * higher weight = head is busier = less preferred for new work
 *
 * hs_route_weighted() replaces simple hs_route_addr():
 *   score = phi_distance(addr, head) + weight[head]
 *   best  = argmin(score)
 * ══════════════════════════════════════════════════════════════════ */

#define HYDRA_WEIGHT_DECAY_SHIFT  3u    /* >> 3 = 7/8 decay             */
#define HYDRA_WEIGHT_MAX          255u  /* fits uint8_t                  */
#define HYDRA_WEIGHT_MAGIC        0x48574754u  /* "HWGT"                */

typedef struct {
    uint8_t   weight[HS_HEADS];     /* 0=idle, 255=saturated            */
    uint8_t   last_routed[HS_HEADS];/* last addr routed to each head    */
    uint32_t  total_routes;
    uint32_t  magic;
} HydraWeightTable;

/* ── init ─────────────────────────────────────────────────────────── */
static inline int hydra_weight_init(HydraWeightTable *wt)
{
    if (!wt) return -1;
    memset(wt, 0, sizeof(*wt));
    wt->magic = HYDRA_WEIGHT_MAGIC;
    return 0;
}

/* ── update weight on write (called from Step 1c) ────────────────── */
static inline void hydra_weight_hit(HydraWeightTable *wt, int head_id)
{
    if (!wt || head_id < 0 || head_id >= HS_HEADS) return;
    uint8_t w = wt->weight[head_id];
    /* add load: +8 per hit (capped at MAX) */
    uint16_t next = (uint16_t)w + 8u;
    wt->weight[head_id] = (uint8_t)(next > HYDRA_WEIGHT_MAX
                                    ? HYDRA_WEIGHT_MAX : next);
}

/* ── decay all weights (called from Step 8 hab_tick) ─────────────── */
static inline void hydra_weight_decay(HydraWeightTable *wt)
{
    if (!wt) return;
    for (int i = 0; i < HS_HEADS; i++) {
        uint8_t w = wt->weight[i];
        wt->weight[i] = (uint8_t)(w - (w >> HYDRA_WEIGHT_DECAY_SHIFT));
    }
}

/* ── PHI-based distance between addr and head ────────────────────── */
static inline uint32_t _phi_distance(uint32_t addr, int head_id)
{
    /* angular distance: (addr × PHI_UP >> 20) mod HS_HEADS vs head_id */
    uint32_t phi_slot = (uint32_t)(((uint64_t)addr * 1696631u) >> 20)
                        & (HS_HEADS - 1);
    int diff = (int)phi_slot - head_id;
    if (diff < 0) diff = -diff;
    if (diff > HS_HEADS / 2) diff = HS_HEADS - diff;
    return (uint32_t)diff;
}

/*
 * hs_route_weighted — find best head for addr
 *
 * score = phi_distance × 16 + weight
 * lower score = better choice
 * distance scaled ×16 so weight (0-255) can influence but not dominate
 *
 * Returns: head_id (0..HS_HEADS-1)
 */
static inline int hs_route_weighted(HydraWeightTable *wt, uint32_t addr)
{
    if (!wt) return (int)(addr & (HS_HEADS - 1));  /* fallback */

    int     best_head  = 0;
    uint32_t best_score = UINT32_MAX;

    for (int h = 0; h < HS_HEADS; h++) {
        uint32_t dist  = _phi_distance(addr, h);
        uint32_t score = dist * 16u + wt->weight[h];
        if (score < best_score) {
            best_score = score;
            best_head  = h;
        }
    }

    wt->last_routed[best_head] = (uint8_t)(addr & 0xFF);
    wt->total_routes++;
    return best_head;
}

/* ── 9-Law check: active (weight>0) heads digit_sum = 9 ─────────── */
static inline int hydra_weight_nine_law(const HydraWeightTable *wt)
{
    if (!wt) return 0;
    uint32_t active = 0;
    for (int i = 0; i < HS_HEADS; i++)
        if (wt->weight[i] > 0) active++;
    if (active == 0) return 1;
    uint32_t ds = active;
    while (ds > 9) { uint32_t t=0; while(ds){t+=ds%10;ds/=10;} ds=t; }
    return ds == 9;
}

/* ── Combined context ─────────────────────────────────────────────── */
typedef struct {
    IterHistoryTable  iter_table;   /* per-node fractal history         */
    HydraWeightTable  weight_table; /* per-head activity weight         */
    uint32_t          magic;
} HydraRoutingCtx;

#define HYDRA_ROUTING_MAGIC  0x48525458u  /* "HRTX" */

static inline int hydra_routing_init(HydraRoutingCtx *ctx)
{
    if (!ctx) return -1;
    iter_table_init(&ctx->iter_table);
    hydra_weight_init(&ctx->weight_table);
    ctx->magic = HYDRA_ROUTING_MAGIC;
    return 0;
}

/*
 * hydra_routing_update — call from Step 1c after fractal_gate_check()
 *   node_id : current node
 *   iter    : result of fractal_gate_check(addr)
 *   head_id : head handling this write
 */
static inline void hydra_routing_update(HydraRoutingCtx *ctx,
                                         uint32_t node_id,
                                         uint32_t iter,
                                         int      head_id)
{
    if (!ctx) return;
    if (node_id < NODE_MAX)
        iter_history_push(&ctx->iter_table.nodes[node_id], iter);
    hydra_weight_hit(&ctx->weight_table, head_id);
}

#endif /* POGLS_HYDRA_WEIGHT_H */
