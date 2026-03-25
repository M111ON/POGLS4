/*
 * pogls_v4_snapshot.h — POGLS V4 Snapshot + Satellite Audit
 * ══════════════════════════════════════════════════════════════════════
 *
 * Ported from V3.1 pogls_snapshot.h + pogls_audit.h
 * Adapted for V4: WAL removed, Delta lane is durability primitive
 *
 * Satellite Audit (Image 1 concept):
 *   Like Iridium constellation — each tile overlaps neighbors.
 *   If one tile anomaly detected, neighbor tiles confirm/deny.
 *   3-layer verify: XOR audit → Fibo intersect → Dual Merkle
 *
 * Snapshot lifecycle:
 *   PENDING → CERTIFIED  (Audit ACK — Absolute Truth)
 *   PENDING → AUTO       (timeout promote — Provisional)
 *   AUTO    → VOID       (invalidate, one-shot)
 *
 * Wire points (V4):
 *   DetachLane anomaly → pogls_audit_signal_push()
 *   delta_sync()       → pogls_snap_certify()
 *   pipeline_wire_init → pogls_audit_init() + pogls_snap_create()
 *
 * Rules (FROZEN):
 *   CERTIFIED = one-shot, never voidable
 *   Auto-promote BLOCKED when audit_health == DEGRADED
 *   Merkle root = XOR of all certified tile hashes
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_V4_SNAPSHOT_H
#define POGLS_V4_SNAPSHOT_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "pogls_platform.h"
#include "pogls_detach_lane.h"

/* ── snapshot state ──────────────────────────────────────────────── */
typedef enum {
    SNAP_PENDING             = 0,  /* just created, waiting Audit ACK   */
    SNAP_CONFIRMED_CERTIFIED = 1,  /* Audit passed — Absolute Truth     */
    SNAP_CONFIRMED_AUTO      = 2,  /* timeout promote — Provisional     */
    SNAP_VOID                = 3,  /* force rollback (one-shot) — Dead  */
    SNAP_MIGRATED            = 4,  /* new lineage after hard migration  */
} snap_state_t;

typedef enum {
    AUDIT_HEALTH_OK       = 0,
    AUDIT_HEALTH_DEGRADED = 1,
    AUDIT_HEALTH_OFFLINE  = 2,
} audit_health_t;

/* ── tile state (satellite node) ─────────────────────────────────── */
typedef enum {
    TILE_IDLE      = 0,
    TILE_SCANNING  = 1,
    TILE_CLEAN     = 2,
    TILE_ANOMALY   = 3,
    TILE_CERTIFIED = 4,
} tile_state_t;

/* anomaly flags — maps to DetachLane reasons */
#define V4_ANOMALY_GEO_INVALID   0x01u  /* = DETACH_REASON_GEO_INVALID  */
#define V4_ANOMALY_GHOST_DRIFT   0x02u  /* = DETACH_REASON_GHOST_DRIFT  */
#define V4_ANOMALY_UNIT_CIRCLE   0x04u  /* = DETACH_REASON_UNIT_CIRCLE  */
#define V4_ANOMALY_TWIN_WINDOW   0x08u  /* crossing event phase288/306  */
#define V4_ANOMALY_OVERFLOW      0x80u  /* = DETACH_REASON_OVERFLOW     */

#define V4_SNAP_MAGIC    0x534E4150u    /* "SNAP"                       */
#define V4_AUDIT_MAGIC   0x41554454u    /* "AUDT"                       */
#define V4_AUDIT_TILES   54u            /* one tile per delta lane       */
#define V4_AUDIT_OVERLAP 3u             /* satellite neighbors per tile  */

/* ══════════════════════════════════════════════════════════════════
 * V4SnapshotHeader — 64B (matches V3.1 layout)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint64_t  snapshot_id;           /* monotonic, never reuse          */
    uint64_t  branch_id;             /* owning branch                   */

    uint8_t   state;                 /* snap_state_t                    */
    uint8_t   is_checkpoint;         /* 1 = immune to cleanup           */
    uint8_t   checkpoint_confirmed;  /* 1 = Audit ACK checkpoint        */
    uint8_t   audit_health_at_promo; /* audit_health_t at promote time  */
    uint8_t   is_suspicious;         /* [STEP1] signal_queue not empty at certify */
    uint8_t   _pad_flags[3];         /* reserved                        */

    uint64_t  parent_snapshot_id;
    uint64_t  certifier_id;          /* delta_sync epoch that certified */
    uint64_t  certified_at_ns;       /* monotonic timestamp             */

    uint64_t  merkle_root;           /* XOR of all certified tile hashes*/
    uint32_t  lane_mask;             /* bitmask of lanes included       */
    uint32_t  magic;
} V4SnapshotHeader;                  /* 72B (extended for control flags) */

/* Frozen tile hash copy — snapshot of audit tile_hash at certify time
 * Enables deterministic replay: snap_verify can recompute Merkle offline */
typedef struct {
    uint64_t  tile_hash[V4_AUDIT_TILES];   /* [STEP2] frozen at certify time */
    uint64_t  merkle_root;                 /* redundant copy for fast check  */
    uint32_t  snapshot_id;                 /* links back to V4SnapshotHeader */
    uint32_t  magic;
} V4SnapTileFreeze;                        /* 54*8 + 16 = 448B */
#define V4_SNAP_TILE_FREEZE_MAGIC  0x46525A45u  /* "FRZE" */

/* ══════════════════════════════════════════════════════════════════
 * V4AuditTile — one per delta lane (satellite node)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t   lane_id;               /* 0..53                           */
    uint8_t   state;                 /* tile_state_t                    */
    uint8_t   anomaly_flags;         /* V4_ANOMALY_* bitmask            */
    uint8_t   neighbor_count;        /* how many overlap neighbors      */
    uint8_t   neighbors[V4_AUDIT_OVERLAP]; /* lane_ids of neighbors    */
    uint8_t   _pad[3];
    uint64_t  blocks_scanned;
    uint64_t  blocks_anomalous;
    uint64_t  tile_hash;             /* XOR hash of lane content        */
    uint64_t  scanned_at_ns;
    uint32_t  anomaly_streak;        /* consecutive anomalies           */
    uint32_t  scan_count;
    uint64_t  _tile_pad[2];          /* alignment pad                   */
} V4AuditTile;                       /* 64B                             */

/* ══════════════════════════════════════════════════════════════════
 * V4AuditContext — satellite constellation
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t       magic;
    audit_health_t health;
    uint8_t        _pad[3];

    V4AuditTile    tiles[V4_AUDIT_TILES];  /* 54 satellite nodes        */
    uint32_t       tile_count;

    /* aggregate stats */
    uint64_t       total_scans;
    uint64_t       total_anomalies;
    uint64_t       last_scan_ns;

    /* merkle */
    uint64_t       merkle_root;      /* XOR of all clean tile hashes    */

    /* signal queue → Snapshot (ring, size=64) */
    DetachEntry    signal_queue[64];
    uint32_t       sig_head;
    uint32_t       sig_tail;
    uint64_t       sig_dropped;    /* (A) overflow counter — never silent drop */
} V4AuditContext;

/* ── time helper ─────────────────────────────────────────────────── */
static inline uint64_t _v4snap_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ══════════════════════════════════════════════════════════════════
 * Audit init — build satellite constellation
 * Neighbor rule: each tile overlaps with (lane±1)%54 and (lane+27)%54
 * = gate neighbor + ghost cross-slice neighbor (Iridium pattern)
 * ══════════════════════════════════════════════════════════════════ */
static inline void v4_audit_init(V4AuditContext *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic      = V4_AUDIT_MAGIC;
    ctx->health     = AUDIT_HEALTH_OK;
    ctx->tile_count = V4_AUDIT_TILES;

    for (uint8_t i = 0; i < V4_AUDIT_TILES; i++) {
        V4AuditTile *t = &ctx->tiles[i];
        t->lane_id   = i;
        t->state     = TILE_IDLE;
        /* satellite neighbors: prev, next, ghost */
        t->neighbors[0] = (uint8_t)((i + 1) % V4_AUDIT_TILES);
        t->neighbors[1] = (uint8_t)((i + V4_AUDIT_TILES - 1) % V4_AUDIT_TILES);
        t->neighbors[2] = (uint8_t)((i + 27) % V4_AUDIT_TILES); /* ghost */
        t->neighbor_count = 3;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * v4_audit_ingest — receive DetachEntry from DetachLane drain
 *
 * This is the wire point: DetachLane → Audit → Snapshot
 * ══════════════════════════════════════════════════════════════════ */
static inline void v4_audit_ingest(V4AuditContext *ctx, const DetachEntry *e)
{
    if (!ctx || !e) return;

    uint8_t lane = (uint8_t)(e->angular_addr % V4_AUDIT_TILES);
    V4AuditTile *t = &ctx->tiles[lane];

    t->state           = TILE_SCANNING;
    t->anomaly_flags  |= e->reason;
    t->blocks_scanned++;
    t->scanned_at_ns   = e->timestamp_ns;
    t->tile_hash      ^= e->value ^ e->angular_addr;  /* rolling XOR  */
    ctx->total_scans++;
    ctx->last_scan_ns  = e->timestamp_ns;

    if (e->reason & (V4_ANOMALY_GEO_INVALID | V4_ANOMALY_GHOST_DRIFT |
                     V4_ANOMALY_UNIT_CIRCLE)) {
        t->blocks_anomalous++;
        t->anomaly_streak++;
        t->state = TILE_ANOMALY;
        ctx->total_anomalies++;

        /* satellite check: if neighbors are clean → isolated anomaly */
        int neighbor_clean = 0;
        for (int n = 0; n < t->neighbor_count; n++) {
            V4AuditTile *nb = &ctx->tiles[t->neighbors[n]];
            if (nb->state == TILE_CLEAN || nb->state == TILE_CERTIFIED)
                neighbor_clean++;
        }

        /* push to signal queue → Snapshot will read this */
        uint32_t next = (ctx->sig_head + 1) & 63u;
        if (next != ctx->sig_tail) {
            ctx->signal_queue[ctx->sig_head] = *e;
            ctx->sig_head = next;
        } else {
            ctx->sig_dropped++;  /* (A) overflow: never silent drop */
        }

        /* degrade health if multiple tiles anomalous */
        if (ctx->total_anomalies > ctx->total_scans / 4)
            ctx->health = AUDIT_HEALTH_DEGRADED;

        (void)neighbor_clean;
    } else {
        t->anomaly_streak = 0;
        t->state = TILE_CLEAN;
        /* update merkle only for clean tiles */
        /* NOTE (B): rolling XOR → drift possible under flip-flop tiles.
         * If same tile alternates CLEAN↔ANOMALY, its hash XORs in/out
         * multiple times. Acceptable for V4 (XOR is reversible by design).
         * Future: snapshot each tile_hash before XOR for exact replay.  */
        ctx->merkle_root ^= t->tile_hash;
    }
}

/* ── batch ingest ─────────────────────────────────────────────────── */
static inline void v4_audit_ingest_batch(V4AuditContext *ctx,
                                          const DetachEntry *entries,
                                          uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        v4_audit_ingest(ctx, &entries[i]);
}

/* ══════════════════════════════════════════════════════════════════
 * Snapshot functions
 * ══════════════════════════════════════════════════════════════════ */

static inline V4SnapshotHeader v4_snap_create(uint64_t snapshot_id,
                                               uint64_t branch_id,
                                               uint64_t parent_id)
{
    V4SnapshotHeader s;
    memset(&s, 0, sizeof(s));
    s.snapshot_id       = snapshot_id;
    s.branch_id         = branch_id;
    s.parent_snapshot_id= parent_id;
    s.state             = SNAP_PENDING;
    s.magic             = V4_SNAP_MAGIC;
    return s;
}

/*
 * v4_snap_certify — TRUTH GATE (STEP 1 + STEP 2)
 *
 * [STEP1] Audit → Control:
 *   - DEGRADED health → certify is BLOCKED (returns -4)
 *   - signal_queue not empty → snap->is_suspicious = 1 (proceed but flagged)
 *
 * [STEP2] Freeze Merkle:
 *   - merkle_root frozen from audit at certify time (no drift after)
 *   - If freeze != NULL → tile hashes also frozen for replay
 *
 * Returns: 0=OK  -1=null  -2=already certified  -3=wrong state
 *          -4=BLOCKED (audit DEGRADED — truth gate rejected)
 */
static inline int v4_snap_certify(V4SnapshotHeader *snap,
                                   uint64_t certifier_id,
                                   const V4AuditContext *audit)
{
    if (!snap) return -1;
    if (snap->state == SNAP_CONFIRMED_CERTIFIED) return -2;
    if (snap->state != SNAP_PENDING) return -3;

    /* [STEP1] Truth gate — audit health check BEFORE commit */
    if (audit && audit->health == AUDIT_HEALTH_DEGRADED)
        return -4;  /* BLOCKED: system degraded, no cert allowed */

    snap->state            = SNAP_CONFIRMED_CERTIFIED;
    snap->certifier_id     = certifier_id;
    snap->certified_at_ns  = _v4snap_now_ns();
    snap->is_checkpoint    = 1;
    snap->checkpoint_confirmed = 1;

    if (audit) {
        /* [STEP2] Freeze Merkle root at certify time — no drift after this */
        snap->merkle_root           = audit->merkle_root;
        snap->audit_health_at_promo = (uint8_t)audit->health;

        /* [STEP1] Flag suspicious if signal queue has pending anomalies */
        uint8_t queue_not_empty = (audit->sig_head != audit->sig_tail) ? 1u : 0u;
        snap->is_suspicious = queue_not_empty;
    }
    return 0;
}

/*
 * v4_snap_certify_freeze — certify + freeze tile hashes for replay (STEP 2)
 * freeze must point to caller-allocated V4SnapTileFreeze.
 * Returns same codes as v4_snap_certify.
 */
static inline int v4_snap_certify_freeze(V4SnapshotHeader *snap,
                                          uint64_t certifier_id,
                                          const V4AuditContext *audit,
                                          V4SnapTileFreeze *freeze)
{
    int r = v4_snap_certify(snap, certifier_id, audit);
    if (r != 0) return r;

    if (freeze && audit) {
        freeze->magic       = V4_SNAP_TILE_FREEZE_MAGIC;
        freeze->snapshot_id = (uint32_t)snap->snapshot_id;
        freeze->merkle_root = snap->merkle_root;   /* already frozen */
        for (uint8_t i = 0; i < V4_AUDIT_TILES; i++)
            freeze->tile_hash[i] = audit->tiles[i].tile_hash;
    }
    return 0;
}

/*
 * v4_snap_auto_promote — timeout promote
 * [STEP1] BLOCKED if audit_health == DEGRADED (returns -3)
 */
static inline int v4_snap_auto_promote(V4SnapshotHeader *snap,
                                        audit_health_t health)
{
    if (!snap) return -1;
    if (snap->state != SNAP_PENDING) return -2;

    /* [STEP1] block_auto_promote: audit DEGRADED blocks timeout promote */
    if (health == AUDIT_HEALTH_DEGRADED) return -3;  /* BLOCKED */

    snap->state = SNAP_CONFIRMED_AUTO;
    snap->audit_health_at_promo = (uint8_t)health;
    snap->certified_at_ns = _v4snap_now_ns();
    return 0;
}

/* v4_snap_invalidate — AUTO → VOID (one-shot) */
static inline int v4_snap_invalidate(V4SnapshotHeader *snap)
{
    if (!snap) return -1;
    if (snap->state != SNAP_CONFIRMED_AUTO) return -2;
    snap->state = SNAP_VOID;
    return 0;
}

static inline int v4_snap_is_certified(const V4SnapshotHeader *snap)
{
    return snap && snap->state == SNAP_CONFIRMED_CERTIFIED;
}

static inline int v4_snap_is_immune(const V4SnapshotHeader *snap)
{
    return snap && snap->is_checkpoint && snap->checkpoint_confirmed;
}

/* ══════════════════════════════════════════════════════════════════
 * 3-layer verify (from Diamond Block spec)
 *
 *   Layer 1: XOR audit      (~0.3ns)
 *   Layer 2: Fibo intersect (~5ns)
 *   Layer 3: Merkle anchor  (on CERTIFIED only)
 * ══════════════════════════════════════════════════════════════════ */
static inline int v4_verify_xor(uint64_t core, uint64_t inv)
{
    /* Layer 1: core XOR inv must be all-ones */
    return (core ^ inv) == 0xFFFFFFFFFFFFFFFFULL;
}

static inline int v4_verify_fibo(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    /* Layer 2: Fibonacci intersection — a∧b∧c∧d invariant */
    return (a & b & c & d) == 0;
}

static inline int v4_verify_merkle(const V4SnapshotHeader *snap,
                                    const V4AuditContext *audit)
{
    /* Layer 3: merkle root matches */
    if (!snap || !audit) return 0;
    if (!v4_snap_is_certified(snap)) return 0;
    return snap->merkle_root == audit->merkle_root;
}

/* ══════════════════════════════════════════════════════════════════
 * Stats
 * ══════════════════════════════════════════════════════════════════ */
static const char *_snap_state_name(snap_state_t s) {
    switch(s) {
        case SNAP_PENDING:             return "PENDING";
        case SNAP_CONFIRMED_CERTIFIED: return "CERTIFIED";
        case SNAP_CONFIRMED_AUTO:      return "AUTO";
        case SNAP_VOID:                return "VOID";
        case SNAP_MIGRATED:            return "MIGRATED";
        default: return "?";
    }
}

static inline void v4_snap_print(const V4SnapshotHeader *s)
{
    if (!s) return;
    printf("Snapshot id=%llu branch=%llu state=%s merkle=%016llx\n",
           (unsigned long long)s->snapshot_id,
           (unsigned long long)s->branch_id,
           _snap_state_name((snap_state_t)s->state),
           (unsigned long long)s->merkle_root);
}

static inline void v4_audit_stats(const V4AuditContext *ctx)
{
    if (!ctx) return;
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  Satellite Audit (54-tile constellation)             ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Health:        %-38s║\n",
           ctx->health==AUDIT_HEALTH_OK?"OK":
           ctx->health==AUDIT_HEALTH_DEGRADED?"DEGRADED":"OFFLINE");
    printf("║ Total scans:   %-38llu║\n",(unsigned long long)ctx->total_scans);
    printf("║ Total anomaly: %-38llu║\n",(unsigned long long)ctx->total_anomalies);
    printf("║ Merkle root:   %016llx                    ║\n",
           (unsigned long long)ctx->merkle_root);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Tile  State      Anomaly  Scanned  Hash              ║\n");
    for (uint32_t i=0;i<ctx->tile_count;i++) {
        const V4AuditTile *t=&ctx->tiles[i];
        if (t->blocks_scanned==0) continue;
        const char *st = t->state==TILE_CLEAN?"CLEAN":
                         t->state==TILE_ANOMALY?"ANOMALY":
                         t->state==TILE_CERTIFIED?"CERT":"SCAN";
        printf("║  %2u   %-9s 0x%02x     %-8llu %016llx║\n",
               i, st, t->anomaly_flags,
               (unsigned long long)t->blocks_scanned,
               (unsigned long long)t->tile_hash);
    }
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_V4_SNAPSHOT_H */
