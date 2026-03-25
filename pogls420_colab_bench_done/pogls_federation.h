/*
 * pogls_federation.h — POGLS V4 + POGLS38 Federation Layer  v1.2
 * ══════════════════════════════════════════════════════════════════
 *
 * V38-aligned: uses core_c types directly (Delta_Context from core_c/pogls_delta.h)
 * DELTA_MAGIC = 0x504C4400 ("PLD\0"), MAX_PAYLOAD = 224B
 *
 * Architecture:
 *   GPU (POGLS38)
 *     ↓ fed_write(packed, angular_addr, value)
 *   Pre-Gate  (iso + lane audit + ghost_mature)
 *     ↓ GATE_PASS only
 *   Backpressure  (queue > HWM → GHOST)
 *     ↓
 *   EarlyMerkle  (tile-level XOR hash per lane)
 *     ↓
 *   ShadowSnapshot  (double-buffer Delta_ContextAB)
 *     ↓ fed_commit() at TC_EVENT_CYCLE_END
 *   Disk (World A lanes 0-3 + World B lanes 4-7)
 *
 * Rules (FROZEN):
 *   - PHI from pogls_platform.h only
 *   - DiamondBlock / Delta_Context from core_c/ only — never copy
 *   - GPU never touches commit path
 *   - Pre-gate MUST run BEFORE fed_write
 *   - commit_pending cleared BEFORE fed_commit() (re-entry guard)
 */
#ifndef POGLS_FEDERATION_H
#define POGLS_FEDERATION_H

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#include "pogls_platform.h"                  /* PHI constants */
#include "core_c/pogls_delta.h"              /* Delta_Context (V3.5 real types) */
#include "core_c/pogls_delta_world_b.h"      /* Delta_ContextAB, World B */

/* ── magic ────────────────────────────────────────────────────── */
#define FED_MAGIC         0x46454401u   /* "FED\1" — v1.2 */
#define FED_GATE_MAGIC    0x47415445u   /* "GATE" */

/* ── backpressure thresholds ──────────────────────────────────── */
#define FED_QUEUE_HWM     4096u
#define FED_QUEUE_LWM      512u
#define FED_QUEUE_MAX     8192u

/* ── tile-level early merkle ──────────────────────────────────── */
#define FED_TILE_COUNT      54u
#define FED_TILE_HASH_SZ    32u

/* ── ghost warm-up window ─────────────────────────────────────── */
#define POGLS_GHOST_STREAK_MAX  8u

/* ══════════════════════════════════════════════════════════════════
 * 1. PRE-COMMIT GATE
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    GATE_PASS  = 0,
    GATE_GHOST = 1,
    GATE_DROP  = 2,
} GateResult;

typedef struct {
    uint64_t  passed;
    uint64_t  ghosted;
    uint64_t  dropped;
    uint32_t  magic;
} GateStats;

/*
 * packed bit layout (from GPU kernel):
 *   bits[19:0]  = hilbert index
 *   bits[25:20] = lane id (0-53)
 *   bit[26]     = iso flag (1 = outside unit circle → DROP)
 *
 * Gate checks (order matters):
 *   1. iso=1           → DROP
 *   2. hil%54 != lane  → DROP  (lane audit invariant)
 *   3. op_count < 8    → GHOST (warm-up)
 *   4. else            → PASS
 */
static inline GateResult fed_gate(uint32_t packed,
                                   uint64_t op_count,
                                   GateStats *gs)
{
    uint32_t hil  = packed & 0xFFFFFu;
    uint32_t lane = (packed >> 20) & 0x3Fu;
    uint32_t iso  = (packed >> 26) & 1u;

    if (iso) {
        if (gs) gs->dropped++;
        return GATE_DROP;
    }
    if ((hil % 54u) != lane) {
        if (gs) gs->dropped++;
        return GATE_DROP;
    }
    if (op_count < (uint64_t)POGLS_GHOST_STREAK_MAX) {
        if (gs) gs->ghosted++;
        return GATE_GHOST;
    }
    if (gs) gs->passed++;
    return GATE_PASS;
}

/* ══════════════════════════════════════════════════════════════════
 * 2. BACKPRESSURE CONTROLLER
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    volatile uint32_t  queue_depth;
    uint32_t           hwm;
    uint32_t           lwm;
    uint32_t           hard_cap;
    uint64_t           throttle_count;
    uint64_t           drop_count;
} BackpressureCtx;

static inline void bp_init(BackpressureCtx *bp)
{
    if (!bp) return;
    memset(bp, 0, sizeof(*bp));
    bp->hwm      = FED_QUEUE_HWM;
    bp->lwm      = FED_QUEUE_LWM;
    bp->hard_cap = FED_QUEUE_MAX;
}

static inline int bp_check(BackpressureCtx *bp)
{
    if (!bp) return 0;
    return (bp->queue_depth >= bp->hwm) ? 1 : 0;
}

static inline void bp_push(BackpressureCtx *bp)
{
    if (!bp) return;
    if (bp->queue_depth < bp->hard_cap) bp->queue_depth++;
    else bp->drop_count++;
}

static inline void bp_pop(BackpressureCtx *bp)
{
    if (!bp || bp->queue_depth == 0) return;
    bp->queue_depth--;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. EARLY MERKLE (tile-level, per lane 0-53)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  tile_hash[FED_TILE_COUNT][FED_TILE_HASH_SZ];
    uint32_t tile_count[FED_TILE_COUNT];
    uint8_t  root[FED_TILE_HASH_SZ];
    uint8_t  root_valid;
} EarlyMerkle;

static inline void em_init(EarlyMerkle *em)
{ if (em) memset(em, 0, sizeof(*em)); }

static inline void em_update(EarlyMerkle *em, uint8_t lane,
                              const void *data, uint32_t size)
{
    if (!em || lane >= FED_TILE_COUNT || !data) return;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < size; i++)
        em->tile_hash[lane][i % FED_TILE_HASH_SZ] ^= p[i];
    em->tile_count[lane]++;
    em->root_valid = 0;
}

static inline void em_reduce(EarlyMerkle *em)
{
    if (!em) return;
    memset(em->root, 0, FED_TILE_HASH_SZ);
    for (uint32_t t = 0; t < FED_TILE_COUNT; t++)
        for (uint32_t b = 0; b < FED_TILE_HASH_SZ; b++)
            em->root[b] ^= em->tile_hash[t][b];
    em->root_valid = 1;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. SHADOW SNAPSHOT (double-buffer)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    Delta_ContextAB  snap[2];   /* double buffer */
    uint32_t         active;    /* 0 or 1 */
    uint64_t         epoch;
} ShadowSnapshot;

static inline int ss_init(ShadowSnapshot *ss, const char *vault)
{
    if (!ss || !vault) return -1;
    memset(ss, 0, sizeof(*ss));
    int r = delta_ab_open(&ss->snap[0], vault);
    if (r != 0) return r;
    /* pre-open shadow slot so second commit doesn't fail */
    delta_ab_open(&ss->snap[1], vault);
    ss->active = 0;
    return 0;
}

static inline int ss_commit(ShadowSnapshot *ss)
{
    if (!ss) return -1;
    int r = delta_ab_commit(&ss->snap[ss->active]);
    if (r != 0) return -1;
    ss->active ^= 1;
    ss->epoch++;
    return 0;
}

static inline void ss_close(ShadowSnapshot *ss)
{
    if (!ss) return;
    delta_ab_close(&ss->snap[0]);
    delta_ab_close(&ss->snap[1]);
}

/* ══════════════════════════════════════════════════════════════════
 * 5. FEDERATION CONTEXT
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    ShadowSnapshot   ss;
    BackpressureCtx  bp;
    GateStats        gate;
    EarlyMerkle      em;
    uint64_t         op_count;
    uint32_t         magic;
    uint32_t         commit_pending;  /* flag: 1 = ring drained → commit */
} FederationCtx;

static inline int fed_init(FederationCtx *f, const char *vault)
{
    if (!f || !vault) return -1;
    memset(f, 0, sizeof(*f));
    f->magic = FED_MAGIC;
    bp_init(&f->bp);
    em_init(&f->em);
    return ss_init(&f->ss, vault);
}

/*
 * fed_write — entry point from POGLS38 GPU output
 *
 * packed       : PACK(hilbert, lane, iso) from GPU kernel
 * angular_addr : PHI scatter address
 * value        : raw data value
 */
static inline GateResult fed_write(FederationCtx  *f,
                                    uint32_t        packed,
                                    uint64_t        angular_addr,
                                    uint64_t        value)
{
    if (!f) return GATE_DROP;

    GateResult gr = fed_gate(packed, f->op_count, &f->gate);
    if (gr != GATE_PASS) return gr;

    if (bp_check(&f->bp)) return GATE_GHOST;

    uint8_t lane = (uint8_t)((packed >> 20) & 0x3Fu);
    uint8_t buf[8]; memcpy(buf, &value, 8);
    em_update(&f->em, lane, buf, 8);

    /* Write to active snapshot World A */
    Delta_ContextAB *cur = &f->ss.snap[f->ss.active];
    delta_append_v3(&cur->a, lane % 4u, angular_addr,
                 &value, sizeof(value));

    bp_push(&f->bp);
    f->op_count++;
    return GATE_PASS;
}

static inline void fed_drain(FederationCtx *f, uint32_t n)
{
    if (!f) return;
    for (uint32_t i = 0; i < n && f->bp.queue_depth > 0; i++)
        bp_pop(&f->bp);
}

static inline int fed_commit(FederationCtx *f)
{
    if (!f) return -1;
    em_reduce(&f->em);
    int r = ss_commit(&f->ss);
    em_init(&f->em);
    return r;
}

static inline Delta_DualRecovery fed_recover(const char *vault)
{
    return delta_ab_recover(vault);
}

static inline void fed_close(FederationCtx *f)
{
    if (!f) return;
    ss_close(&f->ss);
}

static inline void fed_stats(const FederationCtx *f)
{
    if (!f) return;
    printf("FED: op=%llu passed=%llu ghosted=%llu dropped=%llu epoch=%llu\n",
           (unsigned long long)f->op_count,
           (unsigned long long)f->gate.passed,
           (unsigned long long)f->gate.ghosted,
           (unsigned long long)f->gate.dropped,
           (unsigned long long)f->ss.epoch);
}

#endif /* POGLS_FEDERATION_H */
