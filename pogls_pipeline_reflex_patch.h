/*
 * pogls_pipeline_reflex_patch.h — POGLS V4  Pipeline Reflex Patch
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Patch file สำหรับ pogls_pipeline_wire.h
 * Apply 4 changes เพื่อ close the loop:
 *
 *   [A] #include "pogls_mesh_entry.h"        ← ใส่ใน includes
 *   [B] ReflexBias reflex;                   ← ใส่ใน PipelineWire struct
 *   [C] _pw_mesh_reflex_cb() function        ← ใส่ก่อน pipeline_wire_init()
 *   [D] 4 lines ใน pipeline_wire_init()      ← reflex_init + wire mesh_cb
 *   [E] 4 lines ใน route_final:              ← apply bias demotion
 *
 * ══════════════════════════════════════════════════════════════════════════
 *
 * DIFF INSTRUCTIONS (apply to pogls_pipeline_wire.h):
 *
 * ── [A] After existing includes, add: ──────────────────────────────────
 *
 *   #include "pogls_mesh_entry.h"
 *
 * ── [B] In PipelineWire struct, after `DetachLane detach;` add: ────────
 *
 *   // Reflex bias — instant feedback from Mesh anomaly history
 *   // Updated by _pw_mesh_reflex_cb (async, off hot path)
 *   // Read by pipeline_wire_process() in route_final section
 *   ReflexBias        reflex;
 *
 * ── [C] Add this function BEFORE pipeline_wire_init(): ─────────────────
 *
 *   static void _pw_mesh_reflex_cb(const MeshEntry *e, void *ctx)
 *   {
 *       PipelineWire *pw = (PipelineWire *)ctx;
 *       if (!pw || !e) return;
 *       reflex_update(&pw->reflex, e);
 *   }
 *
 * ── [D] In pipeline_wire_init(), after detach_lane_start() add: ────────
 *
 *   // wire Mesh callback → reflex bias update
 *   reflex_init(&pw->reflex);
 *   pw->detach.mesh_cb  = _pw_mesh_reflex_cb;
 *   pw->detach.mesh_ctx = pw;
 *
 * ── [E] In route_final: section, BEFORE switch(l3_route) add: ──────────
 *
 *   // Reflex bias: instant feedback from past anomalies
 *   // bias <= -4 → zone has anomaly history → demote MAIN → GHOST
 *   // Threshold -4 requires meaningful signal (not single event)
 *   if (reflex_should_demote(&pw->reflex, angular_addr) &&
 *       l3_route == ROUTE_MAIN) {
 *       l3_route = ROUTE_GHOST;
 *       pw->anchor_ghost++;
 *   }
 *
 * ── [F] In pogls_detach_lane.h, add to DetachLane struct: ──────────────
 *
 *   // Mesh bridge callback (optional, async only)
 *   // Called from detach_flush_pass() — NEVER from push() hot path
 *   void   (*mesh_cb)(const MeshEntry *e, void *ctx);
 *   void    *mesh_ctx;
 *
 * ── [G] In detach_flush_pass(), after delta_append call add: ───────────
 *
 *   // Mesh bridge: translate + push (anomaly only, async)
 *   if (dl->mesh_cb) {
 *       for (uint32_t _mi = 0; _mi < avail; _mi++) {
 *           DetachEntry *_de = &dl->ring[(t + _mi) & DETACH_RING_MASK];
 *           if (!is_mesh_anomaly(_de)) continue;
 *           MeshEntry _me = mesh_translate(_de);
 *           dl->mesh_cb(&_me, dl->mesh_ctx);
 *       }
 *   }
 *
 * ══════════════════════════════════════════════════════════════════════════
 * STANDALONE VERIFICATION (no full POGLS codebase needed):
 *
 *   gcc -O2 -std=c11 tests/test_mesh_entry.c -o t1 && ./t1
 *   gcc -O2 -std=c11 tests/test_mesh_wire.c  -o t2 && ./t2
 *
 * Full pipeline test requires pogls_pipeline_wire.h + all deps.
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_PIPELINE_REFLEX_PATCH_H
#define POGLS_PIPELINE_REFLEX_PATCH_H

/* This file is documentation only — see DIFF INSTRUCTIONS above */
/* Actual implementation is in pogls_mesh_entry.h                */

#endif /* POGLS_PIPELINE_REFLEX_PATCH_H */
