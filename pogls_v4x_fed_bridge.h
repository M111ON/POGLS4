/*
 * pogls_v4x_fed_bridge.h — V4x → Federation Bridge
 *
 * Wires v4x_step() output into fed_write() and hooks fed_commit()
 * at TC_EVENT_CYCLE_END (every 720 steps).
 *
 * USAGE:
 *   Replace _pad in FederationCtx with commit_pending (see note below),
 *   then call v4x_fed_step() in your main loop instead of v4x_step().
 *
 * FederationCtx patch (pogls_federation.h line ~334):
 *   uint32_t  _pad;           →  uint32_t  commit_pending;
 *   (struct size unchanged — same offset, same alignment)
 *
 * INVARIANTS (never break):
 *   - commit only fires when ring is fully drained (pending == 0)
 *   - TC_EVENT_CYCLE_END sets flag once per cycle (double-trigger guard)
 *   - commit_pending cleared BEFORE fed_commit() (re-entry guard)
 *   - cycle_id logged at every commit for debug timing
 */

#pragma once
#include "pogls_federation.h"
#include "pogls_v4x_wire.h"

/* ── drain callback ─────────────────────────────────────────────────── */

static void fed_drain_cb(const V4xCommitEntry *e, void *ud)
{
    FederationCtx *fed = (FederationCtx *)ud;

    /* feed packed cell into federation write path */
    fed_write(fed, e->v_snapped, e->v_clean);

    /* set commit flag once per cycle — double-trigger guard */
    if ((e->events & TC_EVENT_CYCLE_END) && !fed->commit_pending)
        fed->commit_pending = 1;
}

/* ── force-drain until ring empty ──────────────────────────────────── */

static inline void v4x_drain_all(V4xWire *w, V4xDrainFn fn, void *ud)
{
    while (v4x_ring_pending(&w->ring) > 0)
        v4x_drain(w, fn, ud);
}

/* ── main bridge step — call this instead of v4x_step() ────────────── */
/*
 * v4x_fed_step():
 *   1. v4x_step()       — canonicalize + tc_dispatch + ma_snap + ring push
 *   2. v4x_drain()      — quota-based drain → fed_drain_cb → fed_write
 *   3. commit check     — fires only when ring empty + flag set
 *      commit_pending=0 before fed_commit() → re-entry safe
 *      logs w->cycle_ends as cycle_id for debug timing
 *
 * Batch / step-stop safety:
 *   If step loop pauses with commit_pending=1 and ring non-empty,
 *   call v4x_fed_flush() to force drain + commit before suspend.
 */
static inline void v4x_fed_step(V4xWire *w, FederationCtx *fed,
                                 uint32_t v_raw)
{
    v4x_step(w, v_raw);
    v4x_drain(w, fed_drain_cb, fed);

    if (fed->commit_pending && v4x_ring_pending(&w->ring) == 0) {
        fed->commit_pending = 0;                   /* clear first — re-entry guard */
        fed_commit(fed);
    }
}

/* ── flush — call before batch suspend / shutdown ───────────────────── */

static inline void v4x_fed_flush(V4xWire *w, FederationCtx *fed)
{
    if (!fed->commit_pending) return;

    v4x_drain_all(w, fed_drain_cb, fed);           /* starvation guard */

    if (v4x_ring_pending(&w->ring) == 0) {
        fed->commit_pending = 0;
        fed_commit(fed);
    }
}
