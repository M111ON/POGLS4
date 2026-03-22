/*
 * pogls_detach_callbacks.h — POGLS V4  DetachLane Callback Extensions
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Extends DetachLane with dual async callbacks:
 *   mesh_cb  → MeshEntry translation → ReflexBias + DiamondLayer
 *   dhc_cb   → DHC (Diamond/Honeycomb/Shadow) ingest
 *
 * Architecture:
 *   detach_flush_pass()
 *        │  for each anomaly entry:
 *        ├─ is_mesh_anomaly() guard
 *        │      ↓ mesh_translate()
 *        │   mesh_cb(MeshEntry, ctx)    ← ReflexBias + DiamondLayer
 *        │
 *        └─ dhc_cb(DetachEntry, ctx)    ← DHC ingest (raw entry, no translate)
 *
 * Rules:
 *   - Both callbacks are OPTIONAL (NULL = disabled)
 *   - Both called AFTER delta_append — never block hot path
 *   - DHC receives raw DetachEntry (no translation needed — DHC has own scatter)
 *   - mesh_cb receives translated MeshEntry (24B, enriched)
 *   - Callback order: mesh_cb first, dhc_cb second (mesh is lighter)
 *
 * DetachLane patch (add to struct after uint32_t magic):
 *   void (*mesh_cb)(const MeshEntry    *e, void *ctx);
 *   void  *mesh_ctx;
 *   void (*dhc_cb) (const DetachEntry  *e, void *ctx);
 *   void  *dhc_ctx;
 *
 * detach_flush_pass() patch (after delta_append call):
 *   _detach_fire_callbacks(dl, ring_base, t, avail);
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_DETACH_CALLBACKS_H
#define POGLS_DETACH_CALLBACKS_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <string.h>
#include "pogls_mesh_entry.h"   /* MeshEntry, mesh_translate, is_mesh_anomaly */

/* DetachEntry and DETACH_RING_MASK come from pogls_mesh_entry.h
 * (which already has a forward decl) or from pogls_detach_lane.h.
 * Define DETACH_RING_MASK here if not already defined.             */
#ifndef DETACH_RING_MASK
#  define DETACH_RING_MASK  4095u
#endif

/* ── callback types ──────────────────────────────────────────────── */
typedef void (*DetachMeshCb)(const MeshEntry   *e, void *ctx);
typedef void (*DetachDhcCb) (const DetachEntry *e, void *ctx);

/* ══════════════════════════════════════════════════════════════════
 * DetachCallbacks — the two callback slots (add to DetachLane)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    DetachMeshCb  mesh_cb;    /* MeshEntry callback (translated)      */
    void         *mesh_ctx;
    DetachDhcCb   dhc_cb;     /* DHC raw DetachEntry callback         */
    void         *dhc_ctx;

    /* stats */
    uint64_t mesh_fired;      /* times mesh_cb was called             */
    uint64_t dhc_fired;       /* times dhc_cb was called              */
    uint64_t mesh_skipped;    /* entries that failed is_mesh_anomaly  */
} DetachCallbacks;

static inline void detach_callbacks_init(DetachCallbacks *cb)
{
    if (!cb) return;
    memset(cb, 0, sizeof(*cb));
}

/* ══════════════════════════════════════════════════════════════════
 * _detach_fire_callbacks — call from detach_flush_pass() after delta
 *
 * ring     : DetachEntry ring buffer pointer
 * tail     : tail index at start of this flush batch
 * count    : number of entries in this batch
 * cb       : callback context
 *
 * Processes entries tail..tail+count-1 (with DETACH_RING_MASK wrap).
 * ══════════════════════════════════════════════════════════════════ */
static inline void _detach_fire_callbacks(DetachCallbacks    *cb,
                                           const DetachEntry  *ring,
                                           uint32_t            tail,
                                           uint32_t            count)
{
    if (!cb || !ring || count == 0) return;
    if (!cb->mesh_cb && !cb->dhc_cb) return;  /* fast exit if both NULL */

    for (uint32_t i = 0; i < count; i++) {
        const DetachEntry *de = &ring[(tail + i) & DETACH_RING_MASK];

        /* mesh_cb: translate + fire (anomaly guard) */
        if (cb->mesh_cb) {
            if (is_mesh_anomaly(de)) {
                MeshEntry me = mesh_translate(de);
                cb->mesh_cb(&me, cb->mesh_ctx);
                cb->mesh_fired++;
            } else {
                cb->mesh_skipped++;
            }
        }

        /* dhc_cb: raw entry, no translation, no guard
         * DHC has its own PHI scatter — receives everything */
        if (cb->dhc_cb) {
            cb->dhc_cb(de, cb->dhc_ctx);
            cb->dhc_fired++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * Patch instructions for pogls_detach_lane.h
 * ══════════════════════════════════════════════════════════════════
 *
 * [A] Add to DetachLane struct (after uint32_t magic):
 *
 *   DetachCallbacks callbacks;
 *
 * [B] In detach_lane_init(), after memset:
 *
 *   detach_callbacks_init(&dl->callbacks);
 *
 * [C] In detach_flush_pass(), after delta_append call:
 *
 *   _detach_fire_callbacks(&dl->callbacks, dl->ring, t, avail);
 *
 * That's it — 3 lines total, zero hot path impact.
 * ══════════════════════════════════════════════════════════════════ */

/* ── convenience wires ───────────────────────────────────────────── */

static inline void detach_set_mesh_cb(DetachCallbacks *cb,
                                       DetachMeshCb     fn,
                                       void            *ctx)
{
    if (!cb) return;
    cb->mesh_cb  = fn;
    cb->mesh_ctx = ctx;
}

static inline void detach_set_dhc_cb(DetachCallbacks *cb,
                                      DetachDhcCb      fn,
                                      void            *ctx)
{
    if (!cb) return;
    cb->dhc_cb  = fn;
    cb->dhc_ctx = ctx;
}

#endif /* POGLS_DETACH_CALLBACKS_H */
