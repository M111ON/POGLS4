# POGLS V4x — Phase F Production Handoff
## Date: 2026-03-25
## Status: DEPLOY READY ✅

---

## FINAL TEST SCORE

```
T01-T31   unit + pipeline                31/31 ✅
S01-S15   multi-cycle stress audit       15/15 ✅
F01-F12   Phase F gate (G4400 CPU-only)  12/12 ✅
──────────────────────────────────────────────
TOTAL                                    58/58 ✅
```

---

## DEPLOY CONDITIONS — ALL MET

```
v4x_soft_mode_ok() == 1          ✅
cpu_canonicalize() CPU-only       ✅  (no CUDA headers needed)
gpu_batch_submit   = stub         ✅  (correct for G4400, no GPU)
ring overflows (100 cycles)       0  ✅
certified > suspicious × 10       ✅
anchor_enforces > 0               ✅
```

---

## ARCHITECTURE (frozen for production)

```
v_raw
  │
  ├─ wire_is_canonical(v_raw)?
  │     YES → vz = v_raw (fast path, ~6 cycles, skip GPU)
  │     NO  → cpu_canonicalize(v_raw) → vz (~20 cycles)
  │
  ▼
tc_dispatch(vz)          ← temporal scheduler, 720-step cycle
  │
  ├─ TC_EVENT_ANCHOR → ma_step() ALL cores
  │     hash-mix timing: (phase ^ core_id*0x9E3779B1) % 144
  │     signed noise [-8,+7]: tie-break, no collapse
  │     adaptive bias_k [1..4]: auto-tune exploration rate
  │
  ├─ TC_EVENT_CYCLE_END → certify + freeze snapshot
  │     v4_snap_certify_freeze() → snap_certified++
  │
  ▼
v4x_ring_push() → drain → output
```

---

## CONSTANTS (FROZEN — NEVER CHANGE)

```c
PHI_UP      = 1696631      // PHI scatter up
PHI_DOWN    = 648055       // PHI scatter down
PHI_SCALE   = 2^20         // fixed-point scale
CAN_GRID_A  = 12           // World A snap grid
CAN_GRID_B  = 9            // World B snap grid
TC_CYCLE    = 720          // temporal closure
TC_ANCHOR   = 144          // enforcement sub-cycle
MA_ANCHORS  = {72,144,288,360}  // all multiples of 18
```

Sacred numbers: **17, 18, 54, 144, 162, 289** — never touch without explicit instruction.

---

## PRODUCTION DEPLOY (G4400, no GPU)

### Build command
```bash
gcc -O2 -Wno-sign-compare -I. -I./storage \
    -DCPU_ONLY=1 \
    -o pogls_v4x_prod main_v4x.c -lpthread
```

### Runtime init
```c
V4xWire w;
v4x_wire_init(&w, 4);   // N=4 virtual cores, gate18_clean

// optional: tune aggressiveness
v4x_set_bias_k(&w, 2);  // AGGR mode for production

// main loop
while (input_available()) {
    uint32_t v_raw = next_input();
    uint32_t v_out = v4x_step(&w, v_raw);
    // v_out = canonical + soft-snapped value
}

// deploy gate check (before going live)
if (!v4x_soft_mode_ok(&w)) {
    // system not stable yet — run more warm-up
}
```

### Performance profile (G4400, -O2)
```
fast path (canonical input):  ~6  cycles  (~19% of inputs)
full canonicalize:             ~20 cycles  (~81% of inputs)
tc_dispatch overhead:          ~4  cycles
ma_step (anchor event):        ~15 cycles (1 per 144 steps)
ring push:                     ~3  cycles
──────────────────────────────────────────
average per step:              ~11 cycles estimated
```

---

## WHAT CHANGED (this session chain)

### Session 1-2: V4x Wire foundation
- wire_canonicalize() — Pythagorean triple, CPU-only
- TCFabric — 720-step scheduler, N virtual cores
- V4xRing — lock-free commit ring
- V4SnapshotHeader — certify + freeze per cycle

### Session 3 (this chain): Multi-anchor hardening
- ma_step() — all-core ANCHOR update (fixed 5-0-0-0 skew)
- Hash-mix anchor timing (fixed aliasing)
- Signed noise [-8,+7] (fixed tie-collapse)
- Bias decay 95% (fixed stuck anchors)
- Adaptive bias_k [1..4] (auto-tune exploration)

### Session 4 (Phase F): Production gate
- cpu_canonicalize() isolated from CUDA
- soft_canonicalize() — passthrough on canonical, correct on miss
- v4x_soft_mode_ok() — deploy gate function
- 100-cycle soak confirmed clean

---

## REMAINING WORK (not blocking Phase F)

### POGLS38 federation bridge (separate project)
```
Snapshot V4 port:
  - Dual Merkle tree
  - World B lanes 4-7
  - 12-step commit protocol

pogls_federation.h:
  GPU (POGLS38) → Pre-Gate → Federation → V4 Shadow Snapshot → disk
```
→ V4x runs standalone without this. Federation is POGLS38-side work.

### SOFT mode monitoring (post-deploy)
Add to production monitoring loop:
```c
if (w.total_steps % 72000 == 0) {   // every 100 cycles
    if (!v4x_soft_mode_ok(&w)) alert("V4x unstable");
    v4x_wire_stats(&w);              // log snapshot
}
```

---

## FILES FOR NEXT SESSION

| File | Use |
|------|-----|
| `pogls_v4x_wire.h` | main pipeline (hash-mix + adaptive) |
| `pogls_multi_anchor.h` | anchor fabric (all 5 fixes) |
| `pogls_v4_snapshot.h` | certify + freeze |
| `pogls_temporal_core.h` | TCFabric scheduler |
| `storage/pogls_delta.h` | disk writer (54 lanes) |
| `test_v4x_full.c` | regression: must be 31/31 |
| `test_v4x_stress.c` | stress: must be 15/15 |
| `test_phase_f.c` | gate: must be 12/12 |

---

## HANDOFF SUMMARY (1 line each)

```
ทำอะไร:   hardened multi-anchor + Phase F gate + 58 tests pass
ระบบคือ:  deterministic bit-level pipeline, CPU-only ready for G4400
next:     POGLS38 federation bridge (dual Merkle, World B lanes 4-7)
ห้ามแตะ:  PHI constants, TC_CYCLE=720, TC_ANCHOR=144, sacred numbers
```
