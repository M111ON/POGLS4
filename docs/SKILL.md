---
name: pogls geometry
description: Expert assistant for POGLS (Positional Geometry Logic Storage) V3.8/V3.9 — a custom angular-addressing binary storage engine. Core: ShellN, Delta lanes, Hydra 16-head scheduler, Temporal bridge, Face state machine, Rewind buffer, Fractal gate. Use whenever user mentions POGLS, angular addressing, Hydra heads, ShellN, WorldN, delta lanes, temporal bridge, FiftyFourBridge, face hibernation, rewind buffer, fractal gate, engine bridge, AdaptTopo, HoneycombSlot, DiamondBlock, or any file named pogls_*. Also trigger for sacred numbers (162, 54, 18, 972), PHI addressing, gate_18, world 4n/5n/6n, chat bridge extension, DOM capture, or Copcon integration. Always consult this skill first — contains authoritative architecture, constants, and rules for entire pogls
---

# POGLS V3.9 — Expert Skill

## Core Law (FROZEN — NEVER CHANGE)
```
A = floor(θ × 2²⁰)     integer only, no float in core
162 nodes               icosphere L2 = 2×3⁴, NEVER modify
DiamondBlock = 64B      Core8+Invert8+QuadMirror32+Honeycomb16
n=8 anchor              gate_18-clean, CANNOT disable
digit_sum = 9           all sacred numbers: 18,54,162,972 → digit_sum=9
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
PHI_UP=1696631  PHI_DOWN=648055  PHI_SCALE=2²⁰=1048576
```

## Architecture Stack

```
pogls_engine_bridge.h   ← MASTER PIPELINE (all layers wired)
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
```

## File Map (V3.8 stable package)

### core/
```
pogls_shell.h           ShellN engine, n=4..16        65/65
pogls_shell_pipeline.h  18-Gate, WolfBitmask, Rubik undo
pogls_temporal.h        FiftyFourBridge ring+hash      34/34
pogls_temporal_lane5.h  NegShadow+InvTimeline          28/28
pogls_pressure_bridge.h Mask+Backpressure+WorkSteal    13/13
pogls_fold.h/.c         DiamondBlock 64B, XOR audit
pogls_rewind.h          Write-Behind Buffer 972 slots  27/27  NEW
pogls_face_sleep.h      Face Hibernation (madvise)     24/24  NEW
pogls_face_state.h      Activity+StateMachine+Hydra    25/25  NEW
pogls_fractal_gate.h    Mandelbrot+Hilbert bridge      22/22  NEW
```

### world/
```
pogls_world.h           World4n/5n/6n structs (DEPRECATED→use ShellN)
pogls_world_n.h/.c      ShellN wrappers API           165/165
pogls_world_n_config.h  3-tier runtime config          70/70
```

### adaptive/
```
pogls_adaptive_n.h      AdaptEngine FSM                45/45
pogls_adapt_topo.h/.c   Unified AdaptTopo + monitor    60/60
```

### topology/
```
pogls_topo.h            Split/Merge protocol           67/67
pogls_topo_delta.h      TopoEvent → Delta lane
```

### compute/
```
pogls_rubik.h           Rubik permutation LUT
pogls_morton.h          Morton spatial index
pogls_bitboard.h        256-bit NodeMask
pogls_node_soa.h        SoA 162 nodes, ~21KB
pogls_node_lut.h        addr→node 256B LUT
pogls_frontier.h        FrontierMask helpers
pogls_exec_window.h     ExecWindow 256 ops/50µs
pogls_compute_lut.h/.c  ComputeLUT g_clut
pogls_fibo_addr.h       Fibonacci addressing
```

### storage/
```
pogls_delta.h/.c        Delta lanes 0-3, 12-step commit  32/32
pogls_delta_world_b.h/.c World B lanes 4-7              53/53
pogls_fold_delta.h/.c   fold_batch_verify→delta
pogls_gear.h/.c         Gear storage
pogls_detach.h/.c       Detach zone (quarantine)
pogls_detach_delta.h    Detach→delta
```

### hydra/
```
pogls_hydra_scheduler.h/.c  16-head work-stealing (DO NOT MODIFY)
pogls_hydra_adapt_bridge.h/.c  Hydra↔AdaptTopo
pogls_hydra_temporal.h     ht_execute hook            16/16
pogls_hydra_weight.h       Weighted routing           21/21  NEW
```

### bridge/
```
pogls_engine_bridge.h   Full 10-step pipeline (patched V3.9)
```

## Key Constants

```c
/* Delta */
DELTA_MAGIC        = 0x504C4400  /* "PLD\0" */
DELTA_BLOCK_SIZE   = 256B
MAX_PAYLOAD        = 224B
BRIDGE_BATCH_SIZE  = 64
EW_MAX_OPS         = 256
EW_MAX_WINDOW_NS   = 50000ULL  /* 50µs */

/* Shell/World */
SHELL_N_MIN=4  MAX=16  ANCHOR=8
TOPO_ADDR_BASE = 0xF0000000

/* Temporal */
TEMPORAL_RING_SIZE   = 256
TEMPORAL_54_BRIDGE   = 54
TEMPORAL_WORLD_4N    = 4
TEMPORAL_WORLD_5N    = 5
TEMPORAL_WORLD_6N    = 6

/* Rewind (V3.9) */
REWIND_GATE   = 18
REWIND_NEXUS  = 54
REWIND_SPHERE = 162
REWIND_MAX    = 972   /* 60KB L2 */

/* FaceState (V3.9) */
ACTIVITY_SPLIT_THRESH = 6
ACTIVITY_GHOST_THRESH = 1
FSTATE_GHOST_TICKS    = 1000
FSTATE_ANCHOR_STRIDE  = 20  /* anchor nodes: 0,20,40...140 */

/* FractalGate (V3.9) */
FRACTAL_MAX_ITER    = 162
FRACTAL_SPLIT_THRESH = 54
FRACTAL_GHOST_THRESH = 9
FRACTAL_ESCAPE_SQ   = 4 × PHI_SCALE

/* HydraWeight (V3.9) */
ITER_HISTORY_DEPTH = 4
HYDRA_WEIGHT_MAX   = 255
```

## Rules Never Break
```
1. 162 nodes FROZEN
2. GPU never touches commit path (Quad Mirror = read-only)
3. Atomic rename = Step 11 (nothing after can fail)
4. Delta block max payload = 224B
5. n=8 anchor cannot be disabled
6. World A delta.c/delta.h — additive only
7. DiamondBlock = 64B fixed
8. digit_sum of active Hydra heads = 9
9. anchor nodes (0,20,40...140) CANNOT sleep or split
10. FACE_WAKE_BEFORE_SPLIT always before topo_split()
```

## Delta Commit Protocol (12 steps)
```
1-2. audit: X.seq==nX.seq, Y.seq==nY.seq
3-4. merkle: SHA256 per lane
5.   write snapshot.merkle.pending
6-8. fsync all pending lanes + merkle.pending
9-10. rename lane_*.pending → lane_*.delta
11.   ATOMIC: rename merkle.pending → snapshot.merkle
12.  epoch++, re-open .pending (O_RDWR|O_CREAT|O_APPEND)
CLEAN: merkle + no .pending with st_size>0
TORN:  .pending st_size>0 → discard → fallback epoch-1
check st_size>0 NOT stat()==0
```

## HoneycombSlot Layout (16B at DiamondBlock[48-63])
```
[0-7]  merkle_root  8B
[8]    algo_id      1B  0=md5 1=sha256
[9]    migration    1B
[10-11] dna_count  2B
[12] topo_state    1B  ← NORMAL/SPLIT/MERGED/ORPHAN (topo.h)
[13] last_op       1B  ← topo_op_t
[14] face_state    1B  ← FSTATE_* (face_state.h) NEW
[15] activity      1B  ← activity score NEW
```

## Two Worlds
```
World A: binary 2ⁿ, ENGINE_ID bit6=0, lanes 0-3
World B: ternary 3ⁿ, ENGINE_ID bit6=1, lanes 4-7
Switch Gate: bit6 of addr
PHI_SCALE=2²⁰, PHI_UP=1696631, PHI_DOWN=648055
twin = addr XOR TWIN_INVERT_MASK → A corrupt → B swap instant
```

## V3.9 New Modules

### pogls_rewind.h (Write-Behind Log)
```
WAL  = บันทึกก่อน → replay forward
Rewind = buffer 972 slots → rewind backward (972 choices)
3 layers: RewindBuffer → Delta → Atomic rename
epoch field: global_id = epoch×972+slot → lock-free timeline
flush every gate_18 (18 ops)
```

### pogls_face_sleep.h (Face Hibernation)
```c
face_sleep(ctx, node_id, ptr, size)  // madvise(MADV_DONTNEED)
face_wake(ctx, node_id, ptr, size)   // madvise(MADV_WILLNEED)
face_sleep_idle_nodes(ctx, density, threshold, base, stride)
FACE_WAKE_BEFORE_SPLIT(ctx, node_id, base, stride)  // macro
```

### pogls_face_state.h (Activity + State Machine + HydraContext)
```c
// activity: score = score - (score>>3) + hit  (7/8 decay, zero-float)
// states: FSTATE_NORMAL/ACTIVE_LOOP/GHOST/SPLIT/WAKING
// PHI split: child_a=61.8_, child_b=38.2_
// HydraContext: local rewind 54 slots, 9-Law active heads
fstate_table_init(ft)
fstate_tick(ft, node_id, hit)          // activity + auto-transition
fstate_split(ft, parent, ca, cb, hid)  // PHI load split
fstate_wake_complete(ft, node_id)
fstate_merge_complete(ft, parent)
hctx_spawn(pool, parent, ca, cb, lane) // execution island
hctx_rewind(ctx, n)                    // local timeline rewind
hctx_route(ctx, addr)                  // PHI routing
```

### pogls_fractal_gate.h (Mandelbrot + Hilbert)
```c
// Mandelbrot fixed-point: z²+c ด้วย int64>>20, PHI_SCALE=2²⁰
// window [-2,0.5]×[-1.25,1.25] via PHI walk
// iter>54 → split depth (+1 per gate_18), iter<9 → ghost
fractal_gate_check(addr)     → iter count 0..162
fractal_split_depth(iter)    → depth 0,1,2...
fractal_gate_advise(fg, addr) → -1=ghost, 0=normal, +N=split depth

// Hilbert bridge: Morton→Hilbert for disk locality
// 256B LUT, L2-resident
hilbert_lut_build(lut)
hilbert_from_morton(lut, morton) → hilbert_index
hilbert_disk_addr(base, lut, morton, slot_sz) → disk addr
```

### pogls_hydra_weight.h (Weighted Routing)
```c
// IterHistory[4]: ring buffer per node, avg/max/stable
iter_history_push(h, iter)
iter_history_avg(h)       // (sum of 4) >> 2
iter_history_stable(h, thresh)

// HydraWeight: activity-based weight per head
// decay = 7/8 bit-shift, +8 per hit
hydra_weight_hit(wt, head_id)
hydra_weight_decay(wt)     // call every hab_tick

// weighted routing: score = phi_distance×16 + weight
hs_route_weighted(wt, addr) → best head_id

// combined context
hydra_routing_update(ctx, node_id, iter, head_id)
```

## Chat Bridge + Extension (separate layer)

### pogls_chat_bridge/
```
Platform θ mapping:
chatgpt  θ=0°    deepseek θ=72°   gemini  θ=144°
copilot  θ=216°  claude   θ=288°
event_id = hash(platform + Q[:80])  ← content dedup
parent_id = last pair event_id if idle < 30min
session boundary = 30min idle → new session_id
DOM selectors (verified 2026-03):
ChatGPT:  .user-message-bubble-color + article.text-token-text-primary
DeepSeek: div.d29f3d7d.ds-message + div.ds-message:not(.d29f3d7d)
think block: ._74c0879 → filter .ds-markdown outside
Gemini:   div.query-text.gds-body-l + model-response
Claude:   [class*=font-user-message] + [class*=font-claude-response]
container: div.flex-1.flex.flex-col.px-4.max-w-3xl
```

### Noise filter (clipboard)
```python
# rejects: terminal logs, POGLS context, code signatures
# regex: r'^\d{2}:\d{2}:\d{2}\s+' ← time-prefixed logs
# "__POGLS_INJECTED__" ← context block
```

## Test Scorecard (total)
```
V3.8 stable:    597/597
V3.9 new:       119/119
Kill-test:      200/200
TOTAL:          916/916 PASS
```

## Remaining Tasks (V3.9)
```
1. WSL2 compile — AVX2 + CUDA on GTX 1050Ti (≥13.6M ops/s)
2. G4b GUI hook — tab_files/tab_hydra → EngineBridge
3. G4c Memory Fabric API — POST /remember GET /recall (World 6n)
4. Copilot DOM verify
5. Semantic index + Memory replay UI (chat bridge Phase 1-2)
```

---