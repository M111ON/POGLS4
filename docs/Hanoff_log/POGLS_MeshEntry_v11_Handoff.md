# POGLS MeshEntry v1.1 — Handoff
**Date:** 2026-03  |  Ratchaburi, Thailand

---

## Changes from v1.0

| Fix | Issue | Solution |
|-----|-------|----------|
| FIX-1 | sig weak: XOR collides on sequential input | Fibonacci hash: `addr×2^64/φ ^ value×2^32/φ ^ phase<<16` |
| FIX-2 | delta: implicit narrow cast | Explicit `(int32_t)p288 - (int32_t)p306` before int16 |
| FIX-3 | decay: O(N) global sweep | Lazy per-bucket: epoch counter, catch-up on access |
| FIX-4 | no positive reinforcement → permanent negative drift | SEQ on recovering zone (+1), `reflex_reward()` API |

---

## Test Results

```
40 / 40 PASS
0 collisions in 2000 sig pairs (Fibonacci hash)
Lazy decay == global decay (T17 verified equivalent)
SEQ rewards neutral zone, still punishes deeply bad zone
```

---

## API Summary (pogls_mesh_entry.h v1.1)

### MeshEntry (24B, frozen)
```c
typedef struct {
    uint64_t addr;      // angular address
    uint64_t value;     // raw data
    uint32_t sig;       // Fibonacci hash fingerprint
    uint8_t  type;      // SEQ=2 > BURST=1 > GHOST=0 > ANOMALY=3
    uint8_t  phase18;   // gate heartbeat
    int16_t  delta;     // (int32)phase288 - (int32)phase306
} MeshEntry;
```

### ReflexBias key functions
```c
reflex_init(r)                    // init all buckets to 0
reflex_update(r, entry)           // neg penalty + FIX-4 pos reward for SEQ
reflex_reward(r, addr)            // explicit +1 reward for stable zone
reflex_lookup(r, addr)            // O(1) read with lazy decay catch-up
reflex_should_demote(r, addr)     // returns 1 if bias ≤ -4
```

### Decay model
```
lazy: each bucket tracks last_decay epoch
on access: catch up missed epochs in O(min(missed,16)) steps
global_epoch advances every REFLEX_DECAY_INTERVAL (64) pushes
result: same as global sweep, O(1) per access
```

### Positive reinforcement logic
```
SEQ + bias > -2  →  +1 reward  (recovering zone)
SEQ + bias ≤ -2  →  -1 penalty (still bad zone)
BURST            →  -3 always
ANOMALY          →  -2 always
GHOST            →  -1 always
```

---

## Apply to POGLS4 repo

Same 7 patches as v1.0 handoff (A-G in pogls_pipeline_reflex_patch.h).
Replace `pogls_mesh_entry.h` with this v1.1 file — API compatible, drop-in.

---

## Memory model status

```
Short-term: ReflexBias  ✅ (this file)
Long-term:  POGLS38     ⏳ MeshEntryBuf ready, consumer not yet built
```

Next: POGLS38 consumer — `mesh_entry_drain()` → cluster → detect stable pattern → feedback

---

*POGLS V4 — Ratchaburi, Thailand | 2026-03*
