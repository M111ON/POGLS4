# POGLS Quick Wins — DHC Callbacks + SliceTag Handoff
**Date:** 2026-03  |  Ratchaburi, Thailand

---

## 61/61 PASS ✅

---

## สิ่งที่ส่งมอบ

| File | บทบาท |
|------|-------|
| `pogls_detach_callbacks.h` | DHC + Mesh dual callback extension |
| `pogls_slice_tag.h` | WireBlock slice tagging |

---

## 1. DHC Callbacks (pogls_detach_callbacks.h)

### Apply ใน pogls_detach_lane.h — 3 lines

**[A] เพิ่มใน DetachLane struct (หลัง magic):**
```c
DetachCallbacks callbacks;
```

**[B] ใน detach_lane_init(), หลัง memset:**
```c
detach_callbacks_init(&dl->callbacks);
```

**[C] ใน detach_flush_pass(), หลัง delta_append:**
```c
_detach_fire_callbacks(&dl->callbacks, dl->ring, t, avail);
```

### Wire ใน pipeline_wire_init():
```c
// mesh_cb: ReflexBias + DiamondLayer
detach_set_mesh_cb(&pw->detach.callbacks, _pw_mesh_reflex_cb, pw);

// dhc_cb: DHC ingest (raw DetachEntry)
detach_set_dhc_cb(&pw->detach.callbacks, _pw_dhc_cb, &pw->dhc_ctx);
```

### Key design:
- **mesh_cb**: receives translated `MeshEntry` — anomaly guard ก่อน
- **dhc_cb**: receives raw `DetachEntry` — **ไม่มี guard**, DHC รับทุก entry

---

## 2. SliceTag (pogls_slice_tag.h)

### Apply ใน pogls_pipeline_wire.h — 3 changes

**[A] เพิ่มใน PipelineWire struct:**
```c
EngineSliceSet slices;
```

**[B] ใน pipeline_wire_init():**
```c
slice_set_init(&pw->slices);
```

**[C] ใน pipeline_wire_process(), หลัง WireBlock blk init:**
```c
uint8_t sid = slice_of_lane(lane);
slice_tag_block(&blk, sid,
                pw->slices.slices[sid].hop_count,
                (uint32_t)pw->total_in);
```

### WireBlock.data[5] layout:
```
bits  7- 0  slice_id   (0,1,2)
bits 15- 8  hop_count  (0=main, 1+=ghost)
bits 23-16  engine_id  (mirrors slice_id)
bits 31-24  reserved
bits 63-32  op_seq     (pw->total_in & 0xFFFFFFFF)
```

data[0..4] และ data[6..7] **ไม่ถูกแตะ** — backward compatible 100%

---

## Callback flow (สมบูรณ์แล้ว)

```
pipeline_wire_process()
    ↓ anomaly
DetachEntry.push() (frozen, hot path)
    ↓ flush_pass (async)
_detach_fire_callbacks()
    ├─ mesh_cb  → mesh_translate() → MeshEntry
    │              → reflex_update()   (page-level)
    │              → diamond_process() (cluster-level)
    │
    └─ dhc_cb   → DHC.ingest()  (raw DetachEntry, PHI scatter)
                   → HoneycombCell state update
                   → ShadowOffset d_a/d_b
```

---

## Backlog ที่เหลือ

```
Snapshot V4  ❌ BLOCKING — port from V3.8 (no data integrity without this)
POGLS38      ⏳ MeshEntryBuf consumer ready, long-term memory loop
Repair       ❌ รอ Snapshot → Rubik recovery → Evolve/Shatter
```

*POGLS V4 — Ratchaburi, Thailand | 2026-03*
