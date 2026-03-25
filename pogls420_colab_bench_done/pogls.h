/*
 * pogls.h — POGLS V4  Master Include
 *
 * Single entry point. All writes flow through pipeline_wire_process().
 * #include "pogls.h"  ← นี่อย่างเดียวพอ
 *
 * Rules (FROZEN):
 *   1. Float orphans (castle_natural, adaptive_routing, adaptive_v2,
 *      v394_unified) — NOT included. Guarded with #error in V4.
 *   2. evo_v1/v2 — NOT included. Use evo3_process() via pipeline.
 *   3. All routing → pipeline_wire_process(pw, value, angular_addr)
 *   4. SHADOW = geo_invalid only (rare). chaos → GHOST.
 */
#ifndef POGLS_H
#define POGLS_H

#include "pogls_platform.h"

/* ── Core ──────────────────────────────────────────────────────── */
#include "core/pogls_rewind.h"
#include "core/pogls_face_sleep.h"
#include "core/pogls_face_state.h"
#include "core/pogls_fractal_gate.h"
#include "core/pogls_giant_shadow.h"

/* ── Storage ────────────────────────────────────────────────────── */
#include "storage/pogls_delta.h"
#include "storage/pogls_spectre.h"
#include "storage/pogls_spectre_delta_bridge.h"
#include "storage/pogls_shadow_delta_wired.h"

/* ── Hydra ──────────────────────────────────────────────────────── */
#include "hydra/pogls_hydra_dynamic.h"
#include "hydra/pogls_resource_guard.h"
#include "hydra/pogls_rate_limiter.h"
#include "hydra/pogls_hydra_weight.h"

/* ── Routing: active pipeline only ─────────────────────────────── */
#include "routing/pogls_l3_intersection.h"   /* L3 engine (relation anchor) */
#include "routing/pogls_infinity_castle.h"   /* SOE geometry                */
#include "pogls_evo_v3.h"                    /* lane routing (12-point)     */
#include "pogls_pipeline_wire.h"             /* MASTER PIPELINE ← use this  */

/* ── Bridge ─────────────────────────────────────────────────────── */
#include "bridge/pogls_engine_bridge.h"

/* ── Convenience API ────────────────────────────────────────────── */
/*
 * pogls_write(pw, value, angular_addr)
 *   → RouteTarget  (ROUTE_MAIN / ROUTE_GHOST / ROUTE_SHADOW)
 *
 * Init: pipeline_wire_init(&pw, "/path/to/delta_dir")
 * Write: pogls_write(&pw, value, addr)
 */
#define pogls_write(pw, val, addr) \
    pipeline_wire_process((pw), (val), (addr))

#endif /* POGLS_H */
