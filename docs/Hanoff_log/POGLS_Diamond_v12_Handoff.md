# POGLS Diamond Layer — Handoff
**Date:** 2026-03  |  Ratchaburi, Thailand

---

## Milestone: Diamond Layer จบแล้ว ✅

```
ก่อน: V4 = fast แต่จำอะไรไม่ได้
หลัง: V4 = fast + เริ่มจำ pattern cluster ได้

Memory stack ตอนนี้:
  ReflexBias    (page 4KB, 256 buckets, short-term)  ✅ v1.1
  DiamondLayer  (PHI cluster, 64 buckets, mid-term)  ✅ THIS
  POGLS38       (temporal, long-term)                 ⏳ future
```

---

## Files

| File | Role | Tests |
|------|------|-------|
| `pogls_diamond_layer.h` | DiamondCell[64] + update + feedback | 55/55 ✅ |
| `pogls_mesh_entry.h` v1.1 | MeshEntry + ReflexBias (dependency) | 40/40 ✅ |

---

## API (4 ส่วน)

### 1. diamond_id — Morton + PHI + delta → [0..63]
```c
uint32_t id = diamond_id(int32_t a, int32_t b, int16_t delta);
// a,b = PHI scatter ของ addr (จาก l3_scatter หรือ diamond_process)
// delta = MeshEntry.delta (world crossing signal)
```

### 2. DiamondCell[64]
```c
typedef struct {
    int8_t   bias;   // routing signal: + boost, - demote
    uint8_t  heat;   // activity level (decays 15/16 per update)
    uint16_t count;  // total hits (capped at 65535)
} DiamondCell;       // 4B — 64 cells = 256B (4 cache lines)
```

### 3. diamond_update — update rule
```c
void diamond_update(DiamondLayer *dl, uint32_t id, const MeshEntry *m);
// SEQ   → bias +2 (structured pattern, reward)
// BURST → bias +1 (active but not catastrophic)
// GHOST → bias -2 (bad routing, demote)
// decay: bias × 7/8, heat × 15/16 (each call)
// convergence: SEQ → ~+14, GHOST → ~-7
```

### 4. diamond_bias — feedback to V4
```c
int8_t b = diamond_bias(&dl, id);
route_score += b;  // positive = boost MAIN, negative = toward GHOST
```

---

## V4 Integration (route_final section)

```c
// existing
int8_t page_bias = reflex_lookup(&pw->reflex, angular_addr);

// NEW: add diamond layer signal
{
    uint32_t mask = (1u<<20)-1u;
    uint32_t addr20 = (uint32_t)(angular_addr & mask);
    int32_t da = (int32_t)(((uint64_t)addr20 * POGLS_PHI_UP)   >> 20) & (int32_t)mask;
    int32_t db = (int32_t)(((uint64_t)addr20 * POGLS_PHI_DOWN)  >> 20) & (int32_t)mask;
    uint32_t did = diamond_id(da, db, 0);  // delta=0 if no MeshEntry context
    int8_t cluster_bias = diamond_bias(&pw->diamond, did);

    if (l3_route == ROUTE_MAIN &&
        ((int)page_bias + (int)cluster_bias) <= -8) {
        l3_route = ROUTE_GHOST;
        pw->anchor_ghost++;
    }
}
```

Add to PipelineWire struct:
```c
DiamondLayer  diamond;   // หลัง ReflexBias reflex;
```

Add to pipeline_wire_init():
```c
diamond_init(&pw->diamond);
```

Add to _pw_mesh_reflex_cb():
```c
diamond_process(&pw->diamond, e);  // one line, after reflex_update
```

---

## Distribution Results
```
64/64 diamonds filled, skew 2.34x
(uniform PHI scatter fills all clusters evenly)
```

## Convergence
```
SEQ  pure signal  → converges ~+14
GHOST pure signal → converges ~-7
Mixed (2:1 SEQ:GHOST) → positive net → system biases toward stable
```

---

## Complete Learning Stack Status

```
observe  ✅  DetachEntry.push() + is_mesh_anomaly guard
translate ✅  mesh_translate() → MeshEntry
reflex   ✅  ReflexBias page-level (256 buckets)
cluster  ✅  DiamondLayer cluster-level (64 buckets)   ← NEW
temporal ⏳  POGLS38 long-term (MeshEntryBuf ready)
feedback ✅  route_score += page_bias + cluster_bias
```

---

## Next (จาก handoff เดิม, ยังค้าง)

1. **Snapshot V4** — BLOCKING, ไม่มี data integrity guarantee
2. **POGLS38 consumer** — `mesh_entry_drain()` → cluster → long-term memory
3. **DHC tail callback** — 30 min, wire dhc_cb ใน detach_flush_pass
4. **EngineSlice wire** — tag WireBlock ด้วย slice_id

*POGLS V4 — Ratchaburi, Thailand | 2026-03*
