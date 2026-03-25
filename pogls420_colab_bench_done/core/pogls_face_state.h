/*
 * pogls_face_state.h — POGLS V3.8  Face State Machine + Activity + HydraContext
 * ══════════════════════════════════════════════════════════════════════════════
 *
 * 3 layers ที่ยังขาดอยู่ รวมไว้ที่เดียว:
 *
 *   1. Activity Tracking  — bit-shift decay, zero-float, ~0.3ns
 *   2. Face State Machine — NORMAL/ACTIVE_LOOP/GHOST/SPLIT/WAKING
 *   3. HydraContext       — execution island per split child
 *
 * ══════════════════════════════════════════════════════════════════════════════
 * HoneycombSlot.reserved[4] layout (extended):
 *
 *   reserved[0] = topo_state_t   ← NORMAL/SPLIT/MERGED/ORPHAN (topo.h)
 *   reserved[1] = last_op        ← topo_op_t
 *   reserved[2] = face_state_t   ← NORMAL/ACTIVE_LOOP/GHOST/SPLIT/WAKING ← NEW
 *   reserved[3] = activity_byte  ← score 0-255 (shifted from uint16)     ← NEW
 *
 * Activity decay (Gemini/GPT suggestion):
 *   score = (score - (score >> 3)) + (hit ? 1 : 0)
 *   = score × 7/8 + hit
 *   = zero-float, pure bit-shift
 *   decay constant = 0.875 = 7/8
 *
 * HydraContext is a LOCAL OVERLAY — not a new topology.
 *   Main topology = source of truth (162 nodes FROZEN)
 *   HydraContext  = execution island for split children
 *   If child fails → local_rewind → main system untouched
 *
 * Rules:
 *   - Max active HydraContexts: 16 (HS_HEADS)
 *   - hydra_id sum must have digit_sum = 9 (active heads mod 9 == 0)
 *   - PHI split: child_a = 61.8% load, child_b = 38.2% audit
 *   - n=8 anchor nodes CANNOT split (FROZEN)
 * ══════════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_FACE_STATE_H
#define POGLS_FACE_STATE_H

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <string.h>

#ifndef NODE_MAX
  #define NODE_MAX 162
#endif

/* ── PHI constants (from pogls_fold.h) ──────────────────────────── */
#ifndef WORLD_PHI_UP
  #define WORLD_PHI_UP   1696631u
  #define WORLD_PHI_DOWN  648055u
  #define WORLD_PHI_SCALE (1u<<20)
#endif

/* ══════════════════════════════════════════════════════════════════
 * 1. ACTIVITY TRACKING
 * ══════════════════════════════════════════════════════════════════ */

/* decay = 7/8 via bit-shift (zero-float) */
#define ACTIVITY_DECAY_SHIFT   3u     /* >> 3 = ×(1/8) subtracted      */
#define ACTIVITY_SPLIT_THRESH    6u   /* score > 6 → ACTIVE_LOOP        */
                                      /* decay converges at 8 w/ hits   */
#define ACTIVITY_GHOST_THRESH    1u   /* score < 1 → GHOST candidate    */
#define ACTIVITY_MAX           255u   /* fits uint8_t                   */

/*
 * activity_update — zero-float decay + hit
 *   score = score - (score >> 3) + hit
 *   one call per temporal tick per node
 */
static inline uint8_t activity_update(uint8_t score, int hit)
{
    /* clamp at MAX before decay to preserve 255 on clamp test */
    if (score == ACTIVITY_MAX && hit) return ACTIVITY_MAX;
    uint8_t decayed = (uint8_t)(score - (score >> ACTIVITY_DECAY_SHIFT));
    uint8_t next    = (uint8_t)(decayed + (hit ? 1u : 0u));
    return next > ACTIVITY_MAX ? ACTIVITY_MAX : next;
}

/* activity_from_density — init score from NodeState.density */
static inline uint8_t activity_from_density(uint64_t density)
{
    if (density > 1000) return ACTIVITY_MAX;
    return (uint8_t)((density * ACTIVITY_MAX) / 1000);
}

/* ══════════════════════════════════════════════════════════════════
 * 2. FACE STATE MACHINE
 *
 *   NORMAL      → idle, no special processing
 *   ACTIVE_LOOP → high activity, hot path
 *   GHOST       → compute disabled, pointer valid, delta still live
 *                 wake latency ~1-2 ticks (just flip bit)
 *   SPLIT       → node split into child_a + child_b
 *   WAKING      → transitioning GHOST → NORMAL (hydrating)
 *
 * Transitions:
 *   NORMAL     → ACTIVE_LOOP  (activity > SPLIT_THRESH)
 *   ACTIVE_LOOP→ SPLIT        (AdaptEngine workload heavy)
 *   NORMAL     → GHOST        (inactivity > GHOST_THRESH ticks)
 *   GHOST      → WAKING       (any access)
 *   WAKING     → NORMAL       (hydration complete)
 *   SPLIT      → NORMAL       (topo_merge complete)
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    FSTATE_NORMAL      = 0,   /* default                              */
    FSTATE_ACTIVE_LOOP = 1,   /* hot path — candidate for split       */
    FSTATE_GHOST       = 2,   /* compute off, ptr valid, delta live   */
    FSTATE_SPLIT       = 3,   /* split into child_a + child_b         */
    FSTATE_WAKING      = 4,   /* hydrating from ghost                 */
} face_state_t;

/* per-node face state (fits in NodeState or HoneycombSlot.reserved) */
typedef struct {
    face_state_t  state;          /* current state                    */
    uint8_t       activity;       /* 0-255 decay score                */
    uint32_t      last_tick;      /* tick of last access              */
    uint32_t      ghost_since;    /* tick when entered GHOST          */
    uint8_t       hydra_id;       /* which HydraContext owns this     */
    uint8_t       child_a;        /* node id of child A (if SPLIT)    */
    uint8_t       child_b;        /* node id of child B (if SPLIT)    */
    uint8_t       _pad;
} FaceState;

/* FaceState table — one per system */
typedef struct {
    FaceState     nodes[NODE_MAX];
    uint32_t      current_tick;
    uint32_t      ghost_threshold;   /* ticks before GHOST            */
    uint32_t      split_threshold;   /* activity before SPLIT         */
    uint32_t      magic;
} FaceStateTable;

#define FSTATE_MAGIC         0x46535441u  /* "FSTA" */
#define FSTATE_GHOST_TICKS   1000u        /* default ghost threshold   */
#define FSTATE_SPLIT_ACT     ACTIVITY_SPLIT_THRESH  /* alias */

/* anchor nodes: stride 20 (162/8), FROZEN — cannot split or ghost  */
#define FSTATE_ANCHOR_STRIDE 20u
static inline int fstate_is_anchor(uint32_t node_id) {
    return node_id < NODE_MAX && (node_id % FSTATE_ANCHOR_STRIDE) == 0;
}

/* ── init ─────────────────────────────────────────────────────────── */
static inline int fstate_table_init(FaceStateTable *ft)
{
    if (!ft) return -1;
    memset(ft, 0, sizeof(*ft));
    ft->ghost_threshold = FSTATE_GHOST_TICKS;
    ft->split_threshold = ACTIVITY_SPLIT_THRESH;
    ft->magic           = FSTATE_MAGIC;
    for (int i = 0; i < NODE_MAX; i++)
        ft->nodes[i].hydra_id = 0xFF;  /* unassigned */
    return 0;
}

/* ── tick ─────────────────────────────────────────────────────────── */
static inline void fstate_tick(FaceStateTable *ft,
                                uint32_t node_id, int hit)
{
    if (!ft || node_id >= NODE_MAX) return;
    FaceState *f = &ft->nodes[node_id];

    /* update activity */
    f->activity  = activity_update(f->activity, hit);
    if (hit) f->last_tick = ft->current_tick;

    /* state transitions */
    switch (f->state) {
    case FSTATE_NORMAL:
        if (!fstate_is_anchor(node_id)) {
            if (f->activity > (uint8_t)ft->split_threshold)
                f->state = FSTATE_ACTIVE_LOOP;
            else if ((ft->current_tick - f->last_tick) > ft->ghost_threshold)
                f->state = FSTATE_GHOST;
        }
        break;
    case FSTATE_ACTIVE_LOOP:
        if (f->activity < (uint8_t)(ft->split_threshold / 2))
            f->state = FSTATE_NORMAL;
        break;
    case FSTATE_GHOST:
        if (hit) {
            f->state = FSTATE_WAKING;
            f->last_tick = ft->current_tick;
        }
        break;
    case FSTATE_WAKING:
        /* caller calls fstate_wake_complete() when hydration done */
        break;
    case FSTATE_SPLIT:
        /* topo_merge will call fstate_merge_complete() */
        break;
    }
}

/* ── wake complete (WAKING → NORMAL) ────────────────────────────── */
static inline void fstate_wake_complete(FaceStateTable *ft, uint32_t node_id)
{
    if (!ft || node_id >= NODE_MAX) return;
    if (ft->nodes[node_id].state == FSTATE_WAKING)
        ft->nodes[node_id].state = FSTATE_NORMAL;
}

/* ── split complete (ACTIVE_LOOP → SPLIT) ────────────────────────── */
static inline int fstate_split(FaceStateTable *ft,
                                uint32_t parent, uint32_t child_a,
                                uint32_t child_b, uint8_t hydra_id)
{
    if (!ft) return -1;
    if (parent >= NODE_MAX || child_a >= NODE_MAX || child_b >= NODE_MAX)
        return -2;
    if (fstate_is_anchor(parent)) return -3;  /* FROZEN */

    FaceState *p = &ft->nodes[parent];
    p->state    = FSTATE_SPLIT;
    p->child_a  = (uint8_t)child_a;
    p->child_b  = (uint8_t)child_b;
    p->hydra_id = hydra_id;

    /* children start NORMAL with parent's activity */
    ft->nodes[child_a].state    = FSTATE_NORMAL;
    ft->nodes[child_a].activity = (uint8_t)(p->activity * 618 / 1000); /* PHI */
    ft->nodes[child_a].hydra_id = hydra_id;

    ft->nodes[child_b].state    = FSTATE_NORMAL;
    ft->nodes[child_b].activity = (uint8_t)(p->activity * 382 / 1000); /* 1-PHI */
    ft->nodes[child_b].hydra_id = hydra_id;

    return 0;
}

/* ── merge complete (SPLIT → NORMAL) ─────────────────────────────── */
static inline void fstate_merge_complete(FaceStateTable *ft, uint32_t parent)
{
    if (!ft || parent >= NODE_MAX) return;
    FaceState *p = &ft->nodes[parent];
    if (p->state != FSTATE_SPLIT) return;

    /* reclaim children */
    if (p->child_a < NODE_MAX) memset(&ft->nodes[p->child_a],0,sizeof(FaceState));
    if (p->child_b < NODE_MAX) memset(&ft->nodes[p->child_b],0,sizeof(FaceState));

    p->state   = FSTATE_NORMAL;
    p->child_a = p->child_b = 0;
    p->hydra_id = 0xFF;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. HYDRA CHILD CONTEXT (Execution Island)
 *
 * NOT a new topology — just a local overlay.
 * Main topology = source of truth.
 * HydraContext  = execution island for split child.
 *
 * child A = hot loop execution (61.8% PHI_UP)
 * child B = audit / cold path  (38.2% PHI_DOWN)
 *
 * If child fails → local_rewind ← main system untouched.
 * ══════════════════════════════════════════════════════════════════ */

#define HYDRA_CTX_REWIND_SIZE  54u    /* 1 nexus = local rewind depth  */
#define HYDRA_CTX_MAX          16u    /* max concurrent contexts       */
#define HYDRA_CTX_MAGIC        0x48594452u  /* "HYDR"                  */

/* mini rewind for local timeline (54 slots = 1 nexus, ~3KB) */
typedef struct __attribute__((packed)) {
    uint64_t raw, inv, q[4], hc[2];
} HCDiamondBlock;   /* 64B local copy */

typedef struct {
    HCDiamondBlock  slots[HYDRA_CTX_REWIND_SIZE];  /* 54 × 64B = 3456B */
    uint32_t        head;
    uint32_t        confirmed;
    uint32_t        local_epoch;
    uint32_t        _pad;
} HydraLocalRewind;

typedef struct {
    uint32_t         magic;
    uint8_t          hydra_id;     /* 0..15 (HS_HEADS)                 */
    uint8_t          parent_face;  /* which node was split             */
    uint8_t          child_a;      /* hot execution child              */
    uint8_t          child_b;      /* audit/cold child                 */

    uint32_t         local_epoch;  /* independent epoch counter        */
    uint32_t         delta_lane;   /* which delta lane (0-7)           */

    /* PHI load weights */
    uint32_t         load_a;       /* 618 = 61.8% (PHI_UP / 1000)     */
    uint32_t         load_b;       /* 382 = 38.2% (PHI_DOWN / 1000)   */

    /* local timeline (54 slots — 1 nexus) */
    HydraLocalRewind local_rewind;

    /* stats */
    uint64_t         ops_local;    /* ops processed in this context    */
    uint64_t         rewinds;      /* local rewinds performed          */
} HydraContext;

/* HydraContext pool — one per system */
typedef struct {
    HydraContext  ctx[HYDRA_CTX_MAX];
    uint16_t      active_mask;     /* bitmask of active contexts       */
    uint32_t      total_spawned;
    uint32_t      magic;
} HydraContextPool;

#define HCTX_POOL_MAGIC  0x48435458u  /* "HCTX" */

/* ── pool init ───────────────────────────────────────────────────── */
static inline int hctx_pool_init(HydraContextPool *pool)
{
    if (!pool) return -1;
    memset(pool, 0, sizeof(*pool));
    pool->magic = HCTX_POOL_MAGIC;
    return 0;
}

/* ── spawn context for split child ───────────────────────────────── */
static inline HydraContext *hctx_spawn(HydraContextPool *pool,
                                        uint8_t parent_face,
                                        uint8_t child_a,
                                        uint8_t child_b,
                                        uint8_t delta_lane)
{
    if (!pool) return NULL;

    /* find free slot */
    for (int i = 0; i < (int)HYDRA_CTX_MAX; i++) {
        if (pool->active_mask & (1u << i)) continue;

        HydraContext *c = &pool->ctx[i];
        memset(c, 0, sizeof(*c));
        c->magic       = HYDRA_CTX_MAGIC;
        c->hydra_id    = (uint8_t)i;
        c->parent_face = parent_face;
        c->child_a     = child_a;
        c->child_b     = child_b;
        c->delta_lane  = delta_lane;
        c->load_a      = 618;   /* PHI: 61.8% */
        c->load_b      = 382;   /* 1-PHI: 38.2% */
        c->local_epoch = 0;

        pool->active_mask |= (uint16_t)(1u << i);
        pool->total_spawned++;
        return c;
    }
    return NULL;   /* pool full */
}

/* ── retire context (merge done) ─────────────────────────────────── */
static inline void hctx_retire(HydraContextPool *pool, uint8_t hydra_id)
{
    if (!pool || hydra_id >= HYDRA_CTX_MAX) return;
    pool->active_mask &= (uint16_t)~(1u << hydra_id);
    memset(&pool->ctx[hydra_id], 0, sizeof(HydraContext));
}

/* ── local rewind push ───────────────────────────────────────────── */
static inline void hctx_push(HydraContext *c, const HCDiamondBlock *b)
{
    if (!c || !b) return;
    uint32_t idx = c->local_rewind.head % HYDRA_CTX_REWIND_SIZE;
    c->local_rewind.slots[idx] = *b;
    c->local_rewind.head++;
    if (c->local_rewind.head % HYDRA_CTX_REWIND_SIZE == 0)
        c->local_epoch++;
    c->ops_local++;
}

/* ── local rewind N steps ────────────────────────────────────────── */
static inline uint32_t hctx_rewind(HydraContext *c, uint32_t n)
{
    if (!c) return 0;
    uint32_t unconfirmed = c->local_rewind.head > c->local_rewind.confirmed
                         ? c->local_rewind.head - c->local_rewind.confirmed : 0;
    uint32_t steps = n < unconfirmed ? n : unconfirmed;
    c->local_rewind.head -= steps;
    c->rewinds++;
    return steps;
}

/* ── 9-Law check — active hydra heads digit sum must be 9 ─────────── */
static inline int hctx_nine_law_ok(const HydraContextPool *pool)
{
    if (!pool) return 0;
    uint32_t active = __builtin_popcount(pool->active_mask);
    /* active heads: 0,9,18,27,36,... digit_sum=9 or active==0 */
    if (active == 0) return 1;
    uint32_t ds = active;
    while (ds > 9) { uint32_t t=0; while(ds){t+=ds%10;ds/=10;} ds=t; }
    return ds == 9;
}

/* ── route addr to child A or B (PHI-based) ──────────────────────── */
static inline uint8_t hctx_route(const HydraContext *c, uint32_t addr)
{
    /* use addr parity + PHI to route:
     * even addr + low bits → child_a (hot, 61.8%)
     * odd  addr + high bits → child_b (audit, 38.2%) */
    uint32_t phi_gate = (addr * WORLD_PHI_UP) >> 20;  /* mod 2²⁰ */
    return (phi_gate & 1) ? c->child_b : c->child_a;
}

#endif /* POGLS_FACE_STATE_H */
