/*
 * pogls_diamond_layer.h — POGLS V4  Diamond Layer  v1.2
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Changes from v1.1:
 *   [FIX-1] bias clamp: hard cap ±32 (prevent runaway, stabilize)
 *   [FIX-2] heat → route impact: hot cluster gets +1 boost
 *   [FIX-3] confidence filter: count < 8 → return 0 (cold start noise)
 *   [FIX-4] last_type pattern memory: same type streak → +1 reinforce
 *
 * DiamondCell final form:
 *   spatial  (Morton + PHI skew → id)
 *   memory   (bias, clamped ±32)
 *   activity (heat, 15/16 decay)
 *   stability(count, confidence gate)
 *   pattern  (last_type, sequence reinforce)
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_DIAMOND_LAYER_H
#define POGLS_DIAMOND_LAYER_H

#include <stdint.h>
#include <string.h>
#include "pogls_mesh_entry.h"

/* ── constants ───────────────────────────────────────────────────── */
#define DIAMOND_COUNT        64u
#define DIAMOND_MASK         (DIAMOND_COUNT - 1u)
#define DIAMOND_MAGIC        0x44494D44u

/* [FIX-1] tighter clamp: ±32 (was ±60) */
#define DIAMOND_BIAS_MAX      32
#define DIAMOND_BIAS_MIN     (-32)

#define DIAMOND_HEAT_DECAY_SHIFT  4u   /* 15/16 per update */
#define DIAMOND_BIAS_DECAY_SHIFT  3u   /* 7/8  per update  */

/* [FIX-2] heat threshold for route boost — heat converges at ~15 with active zone */
#define DIAMOND_HEAT_BOOST_THRESH  15u

/* [FIX-3] confidence gate */
#define DIAMOND_COLD_START_THRESH  8u

/* decision thresholds (strict < / >)
 * GHOST pure streak converges at bias=-1 (decay cancels -2 delta at small values)
 * Pattern reinforce (FIX-4) can push further negative with streak
 * Demote: -1 means "any negative signal" — conservative and correct */
#define DIAMOND_BOOST_THRESHOLD    8   /* strict > : need strong positive  */
#define DIAMOND_DEMOTE_THRESHOLD  (-1) /* strict < : any negative demotes  */

/* ══════════════════════════════════════════════════════════════════
 * diamond_id — Morton + PHI skew + delta → [0..63]  (unchanged)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t _diamond_morton4(uint32_t x, uint32_t y)
{
    x &= 0xFu; y &= 0xFu;
    x = (x | (x << 2)) & 0x33u;
    x = (x | (x << 1)) & 0x55u;
    y = (y | (y << 2)) & 0x33u;
    y = (y | (y << 1)) & 0x55u;
    return x | (y << 1);
}

static inline uint32_t diamond_id(int32_t a, int32_t b, int16_t delta)
{
    int32_t ax = a + (a >> 1);   /* PHI skew ×1.5 */
    int32_t by = b;

    uint32_t ax_mid = (uint32_t)((ax >> 13) & 0xFu);
    uint32_t by_mid = (uint32_t)((by >> 13) & 0xFu);
    uint32_t id = _diamond_morton4(ax_mid, by_mid);

    id ^= (id >> 4);
    id ^= (uint32_t)((uint16_t)delta & 0x3Fu);
    return id & DIAMOND_MASK;
}

/* ══════════════════════════════════════════════════════════════════
 * DiamondCell v1.2 — 6B (was 4B, adds last_type + _pad)
 *
 * [FIX-4] last_type: 1B pattern memory — tracks previous event type
 *         0xFF = unset (never seen an event)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    int8_t   bias;       /* routing memory (clamped ±32)             */
    uint8_t  heat;       /* activity level (15/16 decay)             */
    uint16_t count;      /* total events (confidence gate)           */
    uint8_t  last_type;  /* [FIX-4] previous MeshEntry type          */
    uint8_t  _pad;       /* align to 6B                              */
} DiamondCell;           /* 6B                                       */

typedef char _diamond_cell_sz[(sizeof(DiamondCell) == 6u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * DiamondLayer
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    DiamondCell cells[DIAMOND_COUNT];   /* 64 × 6B = 384B            */

    uint64_t total_updates;
    uint64_t seq_boosts;
    uint64_t ghost_demotes;
    uint64_t burst_updates;
    uint64_t pattern_reinforces;   /* [FIX-4] streak events          */
    uint64_t cold_filtered;        /* [FIX-3] count < 8 filtered out */

    uint32_t magic;
} DiamondLayer;

static inline void diamond_init(DiamondLayer *dl)
{
    if (!dl) return;
    memset(dl, 0, sizeof(*dl));
    dl->magic = DIAMOND_MAGIC;
    /* mark all cells as uninitialized */
    for (uint32_t i = 0; i < DIAMOND_COUNT; i++)
        dl->cells[i].last_type = 0xFFu;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_update v1.2
 *
 * Order of operations:
 *   1. count++ (before confidence check — count always grows)
 *   2. heat++
 *   3. [FIX-4] pattern reinforce: same type streak → +1 extra
 *   4. base bias delta by type
 *   5. decay bias (7/8)
 *   6. [FIX-1] clamp bias to ±32
 *   7. decay heat (15/16)
 *   8. update last_type
 * ══════════════════════════════════════════════════════════════════ */
static inline void diamond_update(DiamondLayer   *dl,
                                   uint32_t        id,
                                   const MeshEntry *m)
{
    if (!dl || !m || id >= DIAMOND_COUNT) return;
    DiamondCell *d = &dl->cells[id];

    /* 1. count */
    if (d->count < 0xFFFFu) d->count++;

    /* 2. heat */
    if (d->heat < 255u) d->heat++;

    /* 3. [FIX-4] pattern reinforce: same type as last event */
    int pattern_bonus = 0;
    if (d->last_type != 0xFFu && d->last_type == m->type) {
        pattern_bonus = 1;
        dl->pattern_reinforces++;
    }

    /* 4. base bias delta */
    int bias_delta = 0;
    switch ((mesh_entry_type_t)m->type) {
    case MESH_TYPE_SEQ:
        bias_delta = +2;
        dl->seq_boosts++;
        break;
    case MESH_TYPE_BURST:
        bias_delta = +1;
        dl->burst_updates++;
        break;
    case MESH_TYPE_GHOST:
        bias_delta = -2;
        dl->ghost_demotes++;
        break;
    case MESH_TYPE_ANOMALY:
        bias_delta = -1;
        break;
    default:
        bias_delta = -1;
        break;
    }

    int new_bias = (int)d->bias + bias_delta + pattern_bonus;

    /* 5. decay 7/8 — applied AFTER adding delta
     * Note: decay on small values rounds toward zero quickly.
     * For GHOST (-2 delta): -2 - (-2>>3=0) = -2, then next: -2+(-2)=-4...
     * Decay only kicks in once bias has magnitude > 7 (>> 3 > 0).
     * This is intentional: small signals persist until overridden.       */
    new_bias = new_bias - (new_bias >> DIAMOND_BIAS_DECAY_SHIFT);

    /* 6. [FIX-1] hard clamp ±32 */
    if (new_bias > DIAMOND_BIAS_MAX) new_bias = DIAMOND_BIAS_MAX;
    if (new_bias < DIAMOND_BIAS_MIN) new_bias = DIAMOND_BIAS_MIN;
    d->bias = (int8_t)new_bias;

    /* 7. heat decay 15/16 */
    d->heat = (uint8_t)((int)d->heat - ((int)d->heat >> DIAMOND_HEAT_DECAY_SHIFT));

    /* 8. [FIX-4] update pattern memory */
    d->last_type = m->type;

    dl->total_updates++;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_score — [FIX-3] confidence-gated bias
 *
 * Returns 0 if count < COLD_START_THRESH (cold start filter)
 * Returns bias otherwise
 *
 * Use this for routing decisions instead of raw diamond_bias()
 * ══════════════════════════════════════════════════════════════════ */
static inline int8_t diamond_score(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    const DiamondCell *d = &dl->cells[id];
    if (d->count < DIAMOND_COLD_START_THRESH) return 0;   /* cold start */
    return d->bias;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_heat_boost — [FIX-2] hot cluster route contribution
 *
 * Returns +1 if heat >= HEAT_BOOST_THRESH AND bias >= 0
 * Quality gate: hot GHOST zones must NOT get a boost.
 * Activity alone does not equal quality.
 * ══════════════════════════════════════════════════════════════════ */
static inline int diamond_heat_boost(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    const DiamondCell *c = &dl->cells[id];
    /* heat boost only for neutral/positive zones (quality gate) */
    return (c->heat >= DIAMOND_HEAT_BOOST_THRESH && c->bias >= 0) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_route_signal — combined signal for V4 route_final
 *
 * Single call returns everything V4 needs:
 *   score  = diamond_score()    (confidence-gated bias)
 *   boost  = diamond_heat_boost() (+1 if hot)
 *   total  = score + boost
 *
 * V4 usage:
 *   int sig = diamond_route_signal(&dl, did, NULL, NULL);
 *   route_score += sig;
 * ══════════════════════════════════════════════════════════════════ */
static inline int diamond_route_signal(const DiamondLayer *dl,
                                        uint32_t            id,
                                        int8_t             *out_score,
                                        int                *out_boost)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    int8_t score = diamond_score(dl, id);
    int    boost = diamond_heat_boost(dl, id);
    if (out_score) *out_score = score;
    if (out_boost) *out_boost = boost;
    return (int)score + boost;
}

/* ══════════════════════════════════════════════════════════════════
 * raw accessors (for stats/debug only — use diamond_score for routing)
 * ══════════════════════════════════════════════════════════════════ */
static inline int8_t   diamond_bias(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    return dl->cells[id].bias;
}
static inline uint8_t  diamond_heat(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    return dl->cells[id].heat;
}
static inline uint16_t diamond_count(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    return dl->cells[id].count;
}
static inline uint8_t  diamond_last_type(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0xFFu;
    return dl->cells[id].last_type;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_process — scatter + update in one call
 * ══════════════════════════════════════════════════════════════════ */
#ifndef POGLS_PHI_UP
#  define POGLS_PHI_UP   1696631u
#  define POGLS_PHI_DOWN  648055u
#endif

static inline uint32_t diamond_process(DiamondLayer    *dl,
                                        const MeshEntry *m)
{
    if (!dl || !m) return 0;
    uint32_t mask = (1u << 20) - 1u;
    uint32_t addr = (uint32_t)(m->addr & mask);
    int32_t a = (int32_t)(((uint64_t)addr * POGLS_PHI_UP)   >> 20) & (int32_t)mask;
    int32_t b = (int32_t)(((uint64_t)addr * POGLS_PHI_DOWN)  >> 20) & (int32_t)mask;
    uint32_t id = diamond_id(a, b, m->delta);
    diamond_update(dl, id, m);
    return id;
}

/* ══════════════════════════════════════════════════════════════════
 * decision helpers
 * ══════════════════════════════════════════════════════════════════ */


static inline int diamond_should_demote(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    /* <= -1: any negative warm signal triggers demotion
     * cold start gate (count<8) in diamond_score ensures no false demotion */
    return (int)diamond_score(dl, id) <= DIAMOND_DEMOTE_THRESHOLD;
}

static inline int diamond_should_boost(const DiamondLayer *dl, uint32_t id)
{
    if (!dl || id >= DIAMOND_COUNT) return 0;
    return (int)diamond_score(dl, id) > DIAMOND_BOOST_THRESHOLD;
}

static inline uint32_t diamond_coverage(const DiamondLayer *dl)
{
    if (!dl) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < DIAMOND_COUNT; i++)
        if (dl->cells[i].count > 0) n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════════
 * diamond_stats
 * ══════════════════════════════════════════════════════════════════ */
static inline void diamond_stats(const DiamondLayer *dl)
{
    if (!dl) return;
    uint32_t pos=0,neg=0,neu=0,cold=0,hot=0;
    int8_t mx=-128,mn=127;
    for (uint32_t i=0;i<DIAMOND_COUNT;i++) {
        const DiamondCell *c=&dl->cells[i];
        if (c->count == 0) continue;
        if (c->count < DIAMOND_COLD_START_THRESH) { cold++; continue; }
        if (c->bias>0) pos++;
        else if (c->bias<0) neg++;
        else neu++;
        if (c->bias>mx) mx=c->bias;
        if (c->bias<mn) mn=c->bias;
        if (c->heat>DIAMOND_HEAT_BOOST_THRESH) hot++;
    }
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Diamond Layer v1.2 Stats                       ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ updates:   %llu  pattern: %llu  cold_skip: %llu\n",
           (unsigned long long)dl->total_updates,
           (unsigned long long)dl->pattern_reinforces,
           (unsigned long long)dl->cold_filtered);
    printf("║ SEQ+:%llu  GHOST-:%llu  BURST:%llu\n",
           (unsigned long long)dl->seq_boosts,
           (unsigned long long)dl->ghost_demotes,
           (unsigned long long)dl->burst_updates);
    printf("║ coverage:  %u/64  cold:%u  hot:%u\n",
           diamond_coverage(dl), cold, hot);
    printf("║ bias dist: +%u neu:%u -%u  range[%d..%d]\n",
           pos,neu,neg,(int)mn,(int)mx);
    printf("╚══════════════════════════════════════════════════╝\n");
}

#endif /* POGLS_DIAMOND_LAYER_H */
