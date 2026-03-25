/*
 * pogls_diamond_hc.h — POGLS V4  Diamond + Honeycomb + Shadow
 * ══════════════════════════════════════════════════════════════════════
 *
 * Three-layer observer (async, outside hot path):
 *
 *   Diamond  = binary index (2^n)  — fast scaffold, deterministic lookup
 *   Honeycomb= ternary state (3^n) — cell/state/neighbor, repair-ready
 *   Shadow   = offset only         — d_a, d_b, pattern/global view
 *
 * 2:3 ratio materialises here:
 *   diamond_id uses binary bit-shift  (2^n addressing)
 *   cell_id    uses ternary quantize  (3^n addressing)
 *
 * Flow (called from DetachLane drain callback = Tail summon point):
 *
 *   DetachEntry (a,b,phase,reason)
 *     → diamond_map(a,b)         → diamond_id   (binary, fast)
 *     → hc_alloc_or_get()        → HoneycombCell (ternary, stateful)
 *     → shadow_update()          → ShadowOffset  (d_a, d_b)
 *     → HoneycombCell.create()   = Tail auto-summon (replaces Entangle)
 *
 * HCDiamondBlock field mapping (64B, all slots now used):
 *   raw      = core value
 *   inv      = tail lineage ref (parent cell_id + slice)
 *   q[0]     = diamond_id
 *   q[1]     = phase18 | phase288<<8 | phase306<<24
 *   q[2]     = shadow d_a (int32) | d_b (int32)
 *   q[3]     = reserved (Rubik recovery, future)
 *   hc[0]    = cell_id | neighbor_mask<<16
 *   hc[1]    = state_hash
 *
 * Rules (FROZEN):
 *   1. Diamond = index only, no heavy state
 *   2. Honeycomb = state + topology (repair source)
 *   3. All ops async + batch, never in hot path
 *   4. TTL per cell — stale cells reclaimed
 *   5. ghost core (lane+27)%54 never touched
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_DIAMOND_HC_H
#define POGLS_DIAMOND_HC_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "pogls_platform.h"
#include "pogls_detach_lane.h"
#include "pogls_engine_slice.h"

/* ── dimensions ──────────────────────────────────────────────────── */
#define DHC_DIAMOND_BITS    6u          /* 2^6 = 64 diamond buckets     */
#define DHC_DIAMOND_COUNT   (1u << DHC_DIAMOND_BITS)   /* 64            */
#define DHC_DIAMOND_MASK    (DHC_DIAMOND_COUNT - 1u)

#define DHC_HC_TERNARY      3u          /* ternary base                  */
#define DHC_HC_CELLS_MAX    (DHC_HC_TERNARY * DHC_HC_TERNARY * \
                             DHC_HC_TERNARY)   /* 27 = 3³ cells per diamond */
#define DHC_HC_POOL_SIZE    (DHC_DIAMOND_COUNT * DHC_HC_CELLS_MAX)  /* 64×27=1728 */

#define DHC_TTL_DEFAULT     54u         /* cell lives 54 ingest cycles   */
#define DHC_SHADOW_HISTORY  18u         /* last 18 offsets per diamond   */
#define DHC_MAGIC       0x44484300u     /* "DHC\0"                       */

/* ══════════════════════════════════════════════════════════════════
 * diamond_map — binary quantize (a,b) → diamond_id
 *
 * 2^n addressing: divide PHI space into 8×8 = 64 grid cells.
 * Adjacent points → adjacent ids. O(1), pure bit-shift.
 *
 * a ∈ [0, PHI_SCALE)  → 3 high bits → 0..7
 * b ∈ [0, PHI_SCALE)  → 3 high bits → 0..7
 * diamond_id = (a_hi << 3) | b_hi   → 0..63
 * ══════════════════════════════════════════════════════════════════ */
static inline uint8_t diamond_map(uint32_t a, uint32_t b)
{
    /* top 3 bits of each 20-bit PHI coordinate */
    uint8_t a_hi = (uint8_t)((a >> 17) & 0x7u);   /* bits 19-17 */
    uint8_t b_hi = (uint8_t)((b >> 17) & 0x7u);
    return (uint8_t)((a_hi << 3) | b_hi);          /* 6-bit id   */
}

/* ══════════════════════════════════════════════════════════════════
 * cell_ternary — ternary quantize (a,b) → local cell within diamond
 *
 * 3^n addressing: within a diamond bucket, further divide into
 * 3×3×3 = 27 ternary sub-cells using phase as third axis.
 *
 * a_t = (a >> 15) % 3   (bits 16-15 mod 3)
 * b_t = (b >> 15) % 3
 * p_t = phase18 % 3
 * cell = a_t * 9 + b_t * 3 + p_t   → 0..26
 * ══════════════════════════════════════════════════════════════════ */
static inline uint8_t cell_ternary(uint32_t a, uint32_t b, uint8_t phase18)
{
    uint8_t a_t = (uint8_t)((a >> 15) % DHC_HC_TERNARY);
    uint8_t b_t = (uint8_t)((b >> 15) % DHC_HC_TERNARY);
    uint8_t p_t = (uint8_t)(phase18   % DHC_HC_TERNARY);
    return (uint8_t)(a_t * 9u + b_t * 3u + p_t);
}

/* ══════════════════════════════════════════════════════════════════
 * HoneycombCell — ternary state unit (32B)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint16_t  cell_id;          /* 0..26 within diamond               */
    uint8_t   diamond_id;       /* which diamond owns this cell       */
    uint8_t   slice_id;         /* origin EngineSlice                 */
    uint32_t  parent_cluster;   /* Voronoi cluster_id (from Mesh)     */
    uint64_t  state_hash;       /* compact state fingerprint          */
    uint16_t  neighbor_mask;    /* Delaunay edges (local, 27-bit max) */
    uint16_t  ttl;              /* decrements on each ingest pass     */
    uint32_t  next_cell;        /* chain: next cell in same diamond   */
    uint32_t  event_count;      /* total events routed here           */
} HoneycombCell;                /* 32B                                */

typedef char _hc_cell_sz[(sizeof(HoneycombCell) == 32u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * DiamondAnchor — binary index entry (16B)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t   diamond_id;
    uint8_t   active_cells;     /* number of live cells in this anchor */
    uint16_t  head_cell;        /* index into cell pool (0 = empty)   */
    uint16_t  cell_count;       /* total cells ever allocated         */
    uint16_t  event_count;      /* total events on this diamond       */
    uint64_t  last_event_ns;    /* timestamp of last event            */
} DiamondAnchor;                /* 16B                                */

typedef char _diamond_sz[(sizeof(DiamondAnchor) == 16u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * ShadowOffset — spatial delta (16B)
 * Stores PHI-space offset between consecutive events on same diamond.
 * Galaxy reads this for pattern/global view.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    int32_t   d_a;              /* delta in PHI-a axis               */
    int32_t   d_b;              /* delta in PHI-b axis               */
    uint16_t  weight;           /* confidence (event count clipped)  */
    uint8_t   phase18;          /* gate phase of this offset         */
    uint8_t   diamond_id;       /* which diamond produced this       */
} ShadowOffset;                 /* 12B + pad to 16B if needed        */

/* ══════════════════════════════════════════════════════════════════
 * DHCContext — the complete Diamond/Honeycomb/Shadow system
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* Diamond layer: 64 anchors */
    DiamondAnchor  anchors[DHC_DIAMOND_COUNT];

    /* Honeycomb layer: flat cell pool 64×27=1728 cells */
    HoneycombCell  cells[DHC_HC_POOL_SIZE];
    uint32_t       cell_alloc_head;   /* next free cell index          */

    /* Shadow layer: ring of last 18 offsets per diamond */
    ShadowOffset   shadows[DHC_DIAMOND_COUNT][DHC_SHADOW_HISTORY];
    uint8_t        shadow_head[DHC_DIAMOND_COUNT];  /* ring write idx  */

    /* Aggregates */
    uint64_t  total_ingested;
    uint64_t  cells_created;      /* Tail summon count                 */
    uint64_t  cells_expired;      /* TTL=0, reclaimed                  */
    uint64_t  shadow_updates;

    uint32_t  magic;
} DHCContext;

/* ── init ────────────────────────────────────────────────────────── */
static inline void dhc_init(DHCContext *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    for (uint32_t i = 0; i < DHC_DIAMOND_COUNT; i++)
        ctx->anchors[i].diamond_id = (uint8_t)i;
    /* cell 0 = sentinel (unused) */
    ctx->cell_alloc_head = 1;
    ctx->magic = DHC_MAGIC;
}

/* ── alloc cell from pool ────────────────────────────────────────── */
static inline uint32_t _dhc_cell_alloc(DHCContext *ctx)
{
    if (!ctx || ctx->cell_alloc_head >= DHC_HC_POOL_SIZE) return 0;
    return ctx->cell_alloc_head++;
}

/* ── reclaim stale cells (TTL=0) ─────────────────────────────────── */
static inline void dhc_tick_ttl(DHCContext *ctx)
{
    if (!ctx) return;
    for (uint32_t i = 1; i < ctx->cell_alloc_head; i++) {
        HoneycombCell *c = &ctx->cells[i];
        if (c->ttl == 0) continue;
        c->ttl--;
        if (c->ttl == 0) {
            /* mark dead: anchor will skip on next lookup */
            c->event_count = 0;
            ctx->cells_expired++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * dhc_ingest — process one DetachEntry
 *
 * This is the Tail summon point.
 * Called from DetachLane drain callback (async, off hot path).
 *
 * Steps:
 *   1. PHI scatter → (a, b)
 *   2. diamond_map(a,b) → diamond_id    [binary 2^n]
 *   3. cell_ternary(a,b,phase) → cell_local [ternary 3^n]
 *   4. find or alloc HoneycombCell       [Tail summon if new]
 *   5. update state_hash, neighbor_mask
 *   6. update ShadowOffset (d_a, d_b)
 * ══════════════════════════════════════════════════════════════════ */
static inline void dhc_ingest(DHCContext *ctx, const DetachEntry *e)
{
    if (!ctx || !e) return;

    /* 1. PHI scatter */
    uint32_t mask = POGLS_PHI_SCALE - 1u;
    uint32_t addr = (uint32_t)(e->angular_addr & mask);
    uint32_t a    = (uint32_t)(((uint64_t)addr * POGLS_PHI_UP)   >> 20) & mask;
    uint32_t b    = (uint32_t)(((uint64_t)addr * POGLS_PHI_DOWN) >> 20) & mask;

    /* 2. Diamond id (binary) */
    uint8_t  did   = diamond_map(a, b);

    /* 3. Cell local index (ternary) */
    uint8_t  clocal = cell_ternary(a, b, e->phase18);

    /* 4. Find or alloc cell */
    DiamondAnchor *anc = &ctx->anchors[did];
    HoneycombCell *found = NULL;
    uint32_t pool_idx = anc->head_cell;

    while (pool_idx != 0) {
        HoneycombCell *c = &ctx->cells[pool_idx];
        if (c->ttl > 0 && c->cell_id == clocal) {
            found = c;
            break;
        }
        pool_idx = c->next_cell;
    }

    if (!found) {
        /* Tail summon: create new cell */
        uint32_t new_idx = _dhc_cell_alloc(ctx);
        if (new_idx == 0) goto shadow_update;  /* pool full — skip */

        HoneycombCell *nc = &ctx->cells[new_idx];
        memset(nc, 0, sizeof(*nc));
        nc->cell_id        = clocal;
        nc->diamond_id     = did;
        nc->slice_id       = (uint8_t)(e->angular_addr % 54u / 18u);
        nc->ttl            = DHC_TTL_DEFAULT;
        nc->next_cell      = anc->head_cell;
        anc->head_cell     = new_idx;
        anc->active_cells++;
        anc->cell_count++;
        ctx->cells_created++;
        found = nc;
    }

    /* 5. Update cell state */
    found->ttl = DHC_TTL_DEFAULT;   /* refresh TTL on access */
    found->event_count++;
    /* state_hash: rolling XOR with value + phase context */
    found->state_hash = (found->state_hash << 7) ^
                        (found->state_hash >> 57) ^
                        e->value ^ ((uint64_t)e->phase18 << 32);
    /* neighbor_mask: mark ternary neighbors active */
    {
        uint8_t a_t = clocal / 9u;
        uint8_t b_t = (clocal % 9u) / 3u;
        /* adjacent cells in ternary grid */
        if (a_t > 0) found->neighbor_mask |= (uint16_t)(1u << (clocal - 9u));
        if (a_t < 2) found->neighbor_mask |= (uint16_t)(1u << (clocal + 9u));
        if (b_t > 0) found->neighbor_mask |= (uint16_t)(1u << (clocal - 3u));
        if (b_t < 2) found->neighbor_mask |= (uint16_t)(1u << (clocal + 3u));
    }

shadow_update:
    /* 6. Shadow offset: d_a, d_b from previous event on this diamond */
    {
        uint8_t sh = ctx->shadow_head[did];
        ShadowOffset *prev = NULL;
        if (sh > 0 || ctx->shadows[did][0].weight > 0) {
            uint8_t prev_idx = (uint8_t)((sh + DHC_SHADOW_HISTORY - 1u)
                                         % DHC_SHADOW_HISTORY);
            prev = &ctx->shadows[did][prev_idx];
        }
        ShadowOffset *cur = &ctx->shadows[did][sh];
        if (prev && prev->weight > 0) {
            /* compute spatial delta from previous event */
            uint32_t prev_a = (uint32_t)(prev->d_a < 0 ? 0 : prev->d_a);
            uint32_t prev_b = (uint32_t)(prev->d_b < 0 ? 0 : prev->d_b);
            cur->d_a      = (int32_t)a - (int32_t)prev_a;
            cur->d_b      = (int32_t)b - (int32_t)prev_b;
        } else {
            cur->d_a = (int32_t)a;
            cur->d_b = (int32_t)b;
        }
        cur->weight     = (uint16_t)(anc->event_count < 65535u
                                     ? anc->event_count + 1u : 65535u);
        cur->phase18    = e->phase18;
        cur->diamond_id = did;
        ctx->shadow_head[did] = (uint8_t)((sh + 1u) % DHC_SHADOW_HISTORY);
        ctx->shadow_updates++;
    }

    anc->last_event_ns = e->timestamp_ns;
    anc->event_count = (uint16_t)(anc->event_count < 65535u
                                   ? anc->event_count + 1u : 65535u);
    ctx->total_ingested++;
}

/* ── batch ingest ────────────────────────────────────────────────── */
static inline void dhc_ingest_batch(DHCContext *ctx,
                                     const DetachEntry *entries,
                                     uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        dhc_ingest(ctx, &entries[i]);
}

/* ── repair hint: find best neighbor cell for recovery ──────────── */
static inline HoneycombCell *dhc_repair_hint(DHCContext *ctx,
                                               uint8_t diamond_id,
                                               uint8_t cell_local)
{
    if (!ctx || diamond_id >= DHC_DIAMOND_COUNT) return NULL;
    DiamondAnchor *anc = &ctx->anchors[diamond_id];
    HoneycombCell *best = NULL;
    uint32_t       best_score = 0;
    uint32_t       pool_idx   = anc->head_cell;

    while (pool_idx != 0) {
        HoneycombCell *c = &ctx->cells[pool_idx];
        if (c->ttl > 0 && c->cell_id != cell_local) {
            /* score: neighbor + event_count */
            uint32_t is_neighbor = (c->neighbor_mask >> cell_local) & 1u;
            uint32_t score = c->event_count + is_neighbor * 1000u;
            if (score > best_score) { best_score = score; best = c; }
        }
        pool_idx = c->next_cell;
    }
    return best;
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void dhc_stats(const DHCContext *ctx)
{
    if (!ctx) return;
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  Diamond/Honeycomb/Shadow Stats                      ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Total ingested:  %10llu                          ║\n",
           (unsigned long long)ctx->total_ingested);
    printf("║ Cells created:   %10llu  (Tail summons)          ║\n",
           (unsigned long long)ctx->cells_created);
    printf("║ Cells expired:   %10llu  (TTL=0)                 ║\n",
           (unsigned long long)ctx->cells_expired);
    printf("║ Shadow updates:  %10llu                          ║\n",
           (unsigned long long)ctx->shadow_updates);
    printf("║ Pool used:       %10u / %-6u                  ║\n",
           ctx->cell_alloc_head - 1u, DHC_HC_POOL_SIZE);
    /* active diamonds */
    uint32_t active = 0;
    for (uint32_t i = 0; i < DHC_DIAMOND_COUNT; i++)
        if (ctx->anchors[i].active_cells > 0) active++;
    printf("║ Active diamonds: %10u / 64                      ║\n", active);
    printf("║ 2:3 structure:   Diamond=2^6 grid, HC=3^3 cells     ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_DIAMOND_HC_H */
