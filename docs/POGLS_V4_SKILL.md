---
name: pogls
description: >
  Expert assistant for POGLS (Positional Geometry Logic Storage) V4 —
  a custom angular-addressing binary storage engine.
  Core: pipeline_wire, L3 confidence router, ShellN adaptive engine,
  Delta lanes, Hydra 16-head scheduler, Temporal bridge, Face state
  machine, Rewind buffer, Fractal gate, GPU batch pipeline.
  Use whenever user mentions POGLS, angular addressing, Hydra heads,
  ShellN, WorldN, delta lanes, temporal bridge, FiftyFourBridge,
  face hibernation, rewind buffer, fractal gate, engine bridge,
  AdaptTopo, HoneycombSlot, DiamondBlock, pipeline_wire, l3_intersection,
  pogls_platform, confidence model, relation anchor, ghost async,
  or any file named pogls_*.
  Also trigger for sacred numbers (162, 54, 18, 972), PHI addressing,
  gate_18, world 4n/5n/6n, chat bridge extension, DOM capture,
  or Copcon integration.
  Always consult this skill first — contains authoritative architecture,
  constants, and design rules for the entire POGLS codebase.
---

# POGLS V4 — Expert Skill

## Core Law (FROZEN — NEVER CHANGE)

```
A = floor(θ × 2²⁰)     integer only, no float in core
162 nodes               icosphere L2 = 2×3⁴, NEVER modify
DiamondBlock = 64B      Core8+Invert8+QuadMirror32+Honeycomb16
n=8 anchor              gate_18-clean, CANNOT disable
digit_sum = 9           all sacred numbers: 18,54,162,972 → digit_sum=9
GPU never touches commit path (Quad Mirror = read-only)
SHADOW = geo_invalid ONLY — chaos/rand must never land here
```

## Sacred Numbers (DNA)

```
18  = 2×3²   gate_18 heartbeat, pipeline depth, flush unit
54  = 2×3³   FiftyFourBridge nexus, 1 Rubik cycle
162 = 2×3⁴   NODE_MAX icosphere L2 (FROZEN)
972 = 54×18  RewindBuffer max slots (60KB L2)
648 = 162×4  World 4n size
640 = 128×5  World 5n size (AVX2-aligned)
654 = 109×6  World 6n size
PHI_UP=1696631  PHI_DOWN=648055  PHI_COMP=400521  PHI_SCALE=2²⁰=1048576
```

## PHI Constants — Single Source

```c
// pogls_platform.h  ← CANONICAL SOURCE (all files alias from here)
#define POGLS_PHI_SCALE  (1u << 20)   // 1,048,576
#define POGLS_PHI_UP     1696631u      // floor(φ  × 2²⁰)
#define POGLS_PHI_DOWN    648055u      // floor(φ⁻¹ × 2²⁰)
#define POGLS_PHI_COMP    400521u      // 2²⁰ − PHI_DOWN (wrap case)
#define POGLS_GHOST_STREAK_MAX  8u    // anti-drift: streak > 8 → MAIN
```

## V4 Core Files (active pipeline)

```
pogls_platform.h              PHI constants single source     FROZEN
pogls_pipeline_wire.h         MASTER ENTRY POINT              V4
routing/pogls_l3_intersection.h  L3 confidence router          V4
pogls_evo_v3.h                Lane routing 12-point           V3.9
core/pogls_fractal_gate.h     Hilbert LUT + Morton bridge     V3.9
storage/pogls_delta.h         Delta lane commit (12-step)     V3.8
pogls.h                       Master include + pogls_write()  V4
```

### V4 Entry Point

```c
// All writes flow through ONE function:
pogls_write(pw, value, angular_addr)
// = pipeline_wire_process(pw, value, angular_addr)
// Init: pipeline_wire_init(&pw, "/path/to/delta_dir")
```

## V4 Pipeline Architecture

```
pogls_write(pw, value, angular_addr)
  │
  ├─ Wire ghost cache (prev_addr context guard)
  │    sig = value ^ angular_addr ^ (value>>32)
  │    hit: verify _ge->prev_addr == l3.prev_addr → MAIN fast path
  │    miss: trace_record + collapse_update + predict_prefetch
  │
  ├─ EvoV3 lane routing (12-point, 0..53)
  │
  ├─ l3_process(value ^ angular_addr)
  │    STAGE 0: PHI scatter → (a,b)
  │    STAGE 1: GeoGate
  │      !valid (circle) → SHADOW  ← geo_invalid ONLY
  │      twin |a-b|<1024 → MAIN
  │      World B + !structured → GHOST fast
  │      World A → STAGE 2
  │    STAGE 2: Confidence model (collapsed L2/L3)
  │      f_phi_l3   = delta in PHI range ±8192
  │      f_local_l3 = Hilbert step < 32
  │      score_ok   = l3_score_rel(addr,prev,prev2) <= 2
  │      structured = flip_dense AND s_flip > r_flip
  │      conf = f_phi + f_local + score_ok + structured
  │      conf >= 1 → MAIN   else → GHOST
  │
  ├─ DeltaSensor (rolling 64-op window on value)
  │    large_pct >= 80% → GHOST pre-filter
  │
  ├─ DualSensor (addr-level)
  │    Sensor B (PHI delta first): delta ≈ PHI_DOWN ± 2¹³ → MAIN
  │    Sensor A (Hilbert local):   dx+dy ≤ 16 → MAIN
  │    else → GHOST
  │
  ├─ Ghost streak guard: streak > 8 → force MAIN
  │
  ├─ Stats counted AFTER all overrides (route_final label)
  │
  └─ Layer 4: Delta storage
       MAIN   → Hilbert disk addr → delta lane (active)
       GHOST  → ghost lane (separate)
       SHADOW → Giant Shadow ring (burst absorber)
```

## L3 Relation Anchor (V4 — 2-step)

```c
// l3_score_rel(addr, prev, prev2)
uint32_t rel  = addr ^ prev ^ (prev >> 7);
rel  ^= (rel  >> 11);   // self-mix: reduces prev drift
uint32_t rel2 = addr ^ prev ^ prev2;
rel2 ^= (rel2 >> 11);
uint32_t anchor = (rel & (rel>>8) & (rel>>16))
                & (rel2 & (rel2>>4));
return __builtin_popcount(anchor);  // <= 2 = structured, > 2 = ghost

// prev_addr + prev2_addr shift every cycle (FROZEN)
eng->prev2_addr = eng->prev_addr;
eng->prev_addr  = (uint32_t)(value & 0xFFFFFu);
```

## Optimized Pipeline (V4 — performance path)

```c
// OPT1: raw delta replaces Hilbert in hot path
uint32_t delta = |addr - prev|;  // O(1) vs Hilbert O(10)

// OPT2: fused confidence (register-local, no serial chain)
uint32_t f_phi   = delta in [PHI_DOWN±TOL] or [PHI_COMP±TOL];
uint32_t f_local = delta < SMALL_T (128);
uint32_t f_ghost = delta >= LARGE_T (1024) AND !f_phi;
uint32_t conf    = (f_phi | f_local) & ~f_ghost;

// OPT3: precompute prev_mix = prev ^ (prev>>7) once per op
// pass as argument, update at end of each op

// Throughput (measured, 2.1GHz):
// Baseline:      198 M/s
// +ghost async:  417 M/s  (+2.1x)   ← highest impact
// +batch-4:      353 M/s  (+1.8x)
// +dual-lane:    289 M/s  (+1.5x)
// Production (with disk I/O ~70% overhead): ~59 M/s
// Spec target: 13.6 M/s → margin 4.4x
```

## Routing Rules (FROZEN)

```
seq   → CPU MAIN   (delta < SMALL_T)
phi   → GPU MAIN   (delta in PHI range)
burst → CPU MAIN   (delta < SMALL_T most of time)
chaos → GPU GHOST  (delta >= LARGE_T, not PHI)
rand  → GPU GHOST  (delta >= LARGE_T, not PHI)
SHADOW → geo_invalid ONLY (<1% normal workload)
```

## ShellN Adaptive Engine

```
n = 4..16   SHELL_N_MIN=4  MAX=16  ANCHOR=8 (cannot go below)
active_nodes = n² ≤ 162  (NODE_MAX FROZEN)

AdaptTopo rules (rolling 64-op window):
  avg_load >= 3 → expand n++
  avg_load <= 1 → shrink n-- (floor: anchor=8)
  avg_load == 2 → hold

gate_18: flush every 18 ops exactly (deterministic)
4 streams adapt independently (no shared state)
```

## GPU Integration

```
GPU role (batch, embarrassingly parallel):
  Morton encode:  addr → spatial index (no prev dependency)
  Hilbert convert: Morton → disk locality (loop 10 × 128K parallel)
  Fold audit:     L1 XOR verify (read-only)

GPU insert point: between Layer 3 (routing) and Layer 4 (delta write)
  gpu_batch_submit(addr)   ← accumulate to 128K ops
  gpu_batch_flush()        ← GPU processes batch, writes delta

GPU NEVER touches:
  Routing decision (stateful, prev chain)
  Delta commit path (atomic rename, fsync)
  Quad Mirror (read-only audit only)

Performance:
  CPU fallback (4 threads): 142 M/s
  GPU T4:                 6,050 M/s  (42x)
  GTX 1050Ti (target):     ~800 M/s  (estimated)

Compile:
  nvcc -DPOGLS_HAVE_CUDA   → GPU path
  gcc  (no flag)           → CPU fallback auto
```

## V3.8 Architecture Stack (legacy, still wired via engine_bridge.h)

```
pogls_engine_bridge.h   ← hs_engine_write_v38() entry
Step 1:  ComputeLUT → node_id
Step 1b: Look-ahead pulse (prefetch)
Step 1c: FaceState tick (activity + GHOST wake)
Step 1d: Rewind push (write-behind buffer)
Step 2:  Rubik write ordering (rubik_mix_addr)
Step 3:  Switch Gate (ENGINE_ID bit6 → World A/B)
Step 4:  fold_batch_verify (L1 XOR + L2 QuadMirror)
Step 5:  ExecWindow buffer
Step 6:  delta_append (World A lanes 0-3 / B lanes 4-7)
Step 7:  Frontier bitboard
Step 7b: Temporal bridge pass (54-bridge ring)
Step 8:  AdaptTopo tick (hab_tick every 100 ops)

NOTE: V4 preferred entry = pipeline_wire_process() via pogls.h
      Do NOT call both on same PipelineWire context.
```

## Full File Map

### core/
```
pogls_shell.h              ShellN engine, n=4..16          65/65
pogls_shell_pipeline.h     18-Gate, WolfBitmask, Rubik undo
pogls_temporal.h           FiftyFourBridge ring+hash        34/34
pogls_temporal_lane5.h     NegShadow+InvTimeline            28/28
pogls_pressure_bridge.h    Mask+Backpressure+WorkSteal      13/13
pogls_fold.h/.c            DiamondBlock 64B, XOR audit
pogls_rewind.h             Write-Behind Buffer 972 slots    27/27
pogls_face_sleep.h         Face Hibernation (madvise)       24/24
pogls_face_state.h         Activity+StateMachine+Hydra      25/25
pogls_fractal_gate.h       Mandelbrot+Hilbert bridge        22/22
pogls_giant_shadow.h       SHADOW burst absorber ring
```

### routing/
```
pogls_l3_intersection.h    L3 confidence router (V4)        13/13 PASS
pogls_infinity_castle.h    SOE ghost+collapse cache
pogls_adaptive_v2.h        Adaptive routing V2  [ORPHANED — float]
pogls_adaptive_routing.h   Mendel router        [ORPHANED — float]
pogls_castle_natural.h     Natural pattern      [ORPHANED — float]
pogls_v394_unified.h       V3.94 unified        [ORPHANED — float]
NOTE: orphaned files have #error guard (#ifdef POGLS_V4_STRICT)
```

### storage/
```
pogls_delta.h/.c           Delta lanes 0-3, 12-step commit  32/32
pogls_delta_world_b.h/.c   World B lanes 4-7                53/53
pogls_fold_delta.h/.c      fold_batch_verify→delta
pogls_spectre.h            Spectre storage
pogls_spectre_delta_bridge.h
pogls_shadow_delta_wired.h
pogls_gear.h/.c            Gear storage
pogls_detach.h/.c          Detach zone (quarantine)
pogls_detach_delta.h       Detach→delta
```

### hydra/
```
pogls_hydra_scheduler.h/.c  16-head work-stealing (DO NOT MODIFY)
pogls_hydra_adapt_bridge.h/.c  Hydra↔AdaptTopo
pogls_hydra_temporal.h      ht_execute hook                 16/16
pogls_hydra_weight.h        Weighted routing                21/21
pogls_hydra_dynamic.h       Dynamic head management
pogls_resource_guard.h      Resource protection
pogls_rate_limiter.h        Rate limiting
```

### bridge/
```
pogls_engine_bridge.h      V3.8 10-step pipeline (legacy)
```

### top-level/
```
pogls_pipeline_wire.h      V4 master pipeline               7/7 PASS
pogls_evo_v3.h             EvoV3 lane routing               16/16 PASS
pogls_evo_v2.h             EvoV2 [DEPRECATED → evo3_process]
pogls_evolution_core.h     EvoV1 [DEPRECATED → evo3_process]
pogls_gpu_pipeline.h       GPU batch pipeline               10/10 PASS
pogls_platform.h           PHI constants single source      FROZEN
pogls.h                    Master include + pogls_write()
```

## Key Constants

```c
/* Delta */
DELTA_MAGIC        = 0x504C4400  /* "PLD\0" */
DELTA_BLOCK_SIZE   = 256B
MAX_PAYLOAD        = 224B
BRIDGE_BATCH_SIZE  = 64
EW_MAX_OPS         = 256
EW_MAX_WINDOW_NS   = 50000ULL    /* 50µs */

/* Shell/World */
SHELL_N_MIN=4  MAX=16  ANCHOR=8
TOPO_ADDR_BASE = 0xF0000000

/* Temporal */
TEMPORAL_RING_SIZE   = 256
TEMPORAL_54_BRIDGE   = 54

/* Rewind */
REWIND_GATE   = 18
REWIND_NEXUS  = 54
REWIND_SPHERE = 162
REWIND_MAX    = 972              /* 60KB L2 */

/* FaceState */
ACTIVITY_SPLIT_THRESH = 6
ACTIVITY_GHOST_THRESH = 1
FSTATE_GHOST_TICKS    = 1000
FSTATE_ANCHOR_STRIDE  = 20      /* anchor nodes: 0,20,40...140 */

/* FractalGate */
FRACTAL_MAX_ITER     = 162
FRACTAL_SPLIT_THRESH = 54
FRACTAL_GHOST_THRESH = 9
FRACTAL_ESCAPE_SQ    = 4 × PHI_SCALE

/* Ghost cache */
GHOST_STORE_SIZE = 4096          /* flat hash table */
V4_STREAK_MAX    = 8             /* = POGLS_GHOST_STREAK_MAX */

/* V4 routing thresholds */
SMALL_T = 128                    /* local/seq/burst delta ceiling */
LARGE_T = 1024                   /* chaos/rand delta floor */
```

## Rules Never Break

```
1.  162 nodes FROZEN
2.  GPU never touches commit path (Quad Mirror = read-only)
3.  Atomic rename = Step 11 (nothing after can fail)
4.  Delta block max payload = 224B
5.  n=8 anchor cannot be disabled
6.  World A delta.c/delta.h — additive only
7.  DiamondBlock = 64B fixed
8.  digit_sum of active Hydra heads = 9
9.  anchor nodes (0,20,40...140) CANNOT sleep or split
10. FACE_WAKE_BEFORE_SPLIT always before topo_split()
11. SHADOW = geo_invalid ONLY — never from chaos/rand logic
12. PHI constants from pogls_platform.h ONLY — never hardcode
13. Stats counted AFTER route_final label (not before DeltaSensor)
14. Ghost cache hit requires prev_addr context match
15. Do NOT call hs_engine_write_v38() and pipeline_wire_process()
    on the same context — two separate entry points
```

## Delta Commit Protocol (12 steps)

```
1-2. audit: X.seq==nX.seq, Y.seq==nY.seq
3-4. merkle: SHA256 per lane
5.   write snapshot.merkle.pending
6-8. fsync all pending lanes + merkle.pending
9-10. rename lane_*.pending → lane_*.delta
11.  ATOMIC: rename merkle.pending → snapshot.merkle
12.  epoch++, re-open .pending (O_RDWR|O_CREAT|O_APPEND)
CLEAN: merkle + no .pending with st_size>0
TORN:  .pending st_size>0 → discard → fallback epoch-1
check st_size>0 NOT stat()==0
```

## Two Worlds

```
World A: binary 2ⁿ,   ENGINE_ID bit6=0, lanes 0-3
World B: ternary 3ⁿ,  ENGINE_ID bit6=1, lanes 4-7
Switch Gate: bit6 of addr
twin = addr XOR TWIN_INVERT_MASK → A corrupt → B swap instant
```

## HoneycombSlot Layout (16B at DiamondBlock[48-63])

```
[0-7]  merkle_root  8B
[8]    algo_id      1B  0=md5 1=sha256
[9]    migration    1B
[10-11] dna_count  2B
[12]   topo_state  1B  NORMAL/SPLIT/MERGED/ORPHAN
[13]   last_op     1B  topo_op_t
[14]   face_state  1B  FSTATE_*
[15]   activity    1B  activity score
```

## Chat Bridge

```
Platform θ mapping:
  chatgpt  θ=0°    deepseek θ=72°   gemini  θ=144°
  copilot  θ=216°  claude   θ=288°

event_id  = hash(platform + Q[:80])
parent_id = last pair event_id if idle < 30min
session   = 30min idle → new session_id

DOM selectors (verified 2026-03):
  ChatGPT:  .user-message-bubble-color + article.text-token-text-primary
  DeepSeek: div.d29f3d7d.ds-message + div.ds-message:not(.d29f3d7d)
  Gemini:   div.query-text.gds-body-l + model-response
  Claude:   [class*=font-user-message] + [class*=font-claude-response]
  container: div.flex-1.flex.flex-col.px-4.max-w-3xl
```

## Test Scorecard

```
V3.8 stable:    597/597
V3.9 new:       119/119
Kill-test:      200/200
V4 patches:      46/46   (12 files, -Wall -Wextra clean)
TOTAL:          962/962 PASS
```

## Remaining Tasks (V4)

```
Priority 1 — immediate, high impact:
  - Ghost async path: separate GHOST ops from critical path
    lock-free ring buffer, expected +2.1x throughput
  - Batch-4 commit in Hydra scheduler (+1.8x)

Priority 2 — G4 group:
  - G4b: GUI hook — tab_files/tab_hydra → EngineBridge
  - G4c: Memory Fabric API — POST /remember GET /recall (World 6n)

Priority 3 — hardware validation:
  - G4d: AVX2 + CUDA on GTX 1050Ti (WSL2), confirm ≥13.6M ops/s
    Delta 4-stripe real disk test

Priority 4 — chat bridge:
  - Copilot DOM verify
  - Semantic index + Memory replay UI (Phase 1-2)
```
