# POGLS Diamond Layer v1.2 — Handoff
**Date:** 2026-03  |  Ratchaburi, Thailand

---

## 47/47 PASS ✅

---

## 4 Fixes Applied

| Fix | Change | Key insight |
|-----|--------|-------------|
| FIX-1 | bias clamp ±32 (was ±60) | SEQ converges +21, GHOST -1 — stable |
| FIX-2 | heat boost only when bias >= 0 | Hot GHOST zone must NOT get reward |
| FIX-3 | cold start: count < 8 → score = 0 | No false demotion on early noise |
| FIX-4 | last_type pattern reinforce | Same streak → +1 extra per event |

---

## DiamondCell v1.2 (6B)

```c
typedef struct {
    int8_t   bias;       // ±32 clamped routing signal
    uint8_t  heat;       // activity (15/16 decay, converges at 15)
    uint16_t count;      // confidence counter (cold gate < 8)
    uint8_t  last_type;  // pattern memory (0xFF = unset)
    uint8_t  _pad;
} DiamondCell;           // 6B
```

---

## Convergence table

| Input | bias converge | heat converge | demote? |
|-------|--------------|---------------|---------|
| GHOST pure | -1 | 15 | yes (after count≥8) |
| SEQ pure | +21 | 15 | no |
| Alternating SEQ/GHOST | 0 | 15 | no |
| SEQ streak | +21+ (reinforce) | 15 | no |

GHOST demotes at **event 8** (cold gate lifts).

---

## heat_boost design rule (important)

```
heat >= 15 AND bias >= 0  →  +1 boost
heat >= 15 AND bias < 0   →  0  (quality gate)
```

Hot GHOST zones are active but low quality.
Activity alone ≠ routing quality.

---

## V4 route_final (complete, 3 signals)

```c
// 1. page-level (ReflexBias)
int8_t page_bias = reflex_lookup(&pw->reflex, angular_addr);

// 2. cluster-level (DiamondLayer)
uint32_t mask = (1u<<20)-1u;
uint32_t addr20 = (uint32_t)(angular_addr & mask);
int32_t da = (int32_t)(((uint64_t)addr20 * POGLS_PHI_UP)   >> 20) & (int32_t)mask;
int32_t db = (int32_t)(((uint64_t)addr20 * POGLS_PHI_DOWN)  >> 20) & (int32_t)mask;
uint32_t did = diamond_id(da, db, 0);   // delta=0 if no MeshEntry context
int cluster_sig = diamond_route_signal(&pw->diamond, did, NULL, NULL);

// 3. combined decision
if (l3_route == ROUTE_MAIN &&
    ((int)page_bias + cluster_sig) < 0) {
    l3_route = ROUTE_GHOST;
    pw->anchor_ghost++;
}
```

Add to PipelineWire:
```c
DiamondLayer  diamond;   // after ReflexBias reflex
```

Init in pipeline_wire_init():
```c
diamond_init(&pw->diamond);
```

Add to _pw_mesh_reflex_cb():
```c
reflex_update(&pw->reflex, e);
diamond_process(&pw->diamond, e);   // one extra line
```

---

## Learning Stack Complete

```
Page     ReflexBias   256 buckets  7/8 decay   short-term  ✅
Cluster  DiamondLayer  64 buckets  7/8 decay   mid-term    ✅ v1.2
Temporal POGLS38      MeshEntryBuf             long-term   ⏳
```

---

## Next priorities (from original handoff)

1. **Snapshot V4** — BLOCKING: no data integrity guarantee
2. **POGLS38 consumer** — drain MeshEntryBuf → cluster → long-term memory
3. **DHC tail callback** — 30 min, wire dhc_cb in detach_flush_pass
4. **EngineSlice wire** — tag WireBlock with slice_id

*POGLS V4 — Ratchaburi, Thailand | 2026-03*
