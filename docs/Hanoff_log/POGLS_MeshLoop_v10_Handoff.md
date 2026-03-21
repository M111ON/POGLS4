# POGLS V4 — Mesh Loop Session Handoff
**Date:** 2026-03  |  Ratchaburi, Thailand  |  Po Panthakhan

---

## สิ่งที่ทำเสร็จ session นี้

### Architecture Decision (LOCKED)
```
V4 (fast execution)     ←──── ไม่แตะ DetachEntry
       ↓ anomaly
DetachEntry (frozen 32B)
       ↓ flush_pass + is_anomaly guard
       ↓ mesh_translate()
MeshEntry (24B — language กลาง)
       ↓ mesh_cb callback (async)
ReflexBias (instant feedback)
       ↓ decay 7/8 bit-shift
V4 route_final: demote MAIN → GHOST
       ↓ (future)
POGLS38 cluster/temporal
```

**Key rule:** DetachEntry FROZEN ตลอดไป. MeshEntry คือ gateway เดียว.

---

## Files Delivered

| File | Role | Tests |
|------|------|-------|
| `pogls_mesh_entry.h` | MeshEntry + translate + MeshEntryBuf + ReflexBias | 34+22+23 |
| `pogls_pipeline_reflex_patch.h` | Diff instructions สำหรับ pipeline_wire.h | doc only |
| `tests/test_mesh_entry.c` | MeshEntry struct + translate + buf | 34/34 ✅ |
| `tests/test_mesh_wire.c` | ReflexBias penalty/decay/isolation | 22/22 ✅ |
| `tests/test_reflex_loop.c` | End-to-end standalone loop | 23/23 ✅ |

**Total: 79/79 PASS**

---

## MeshEntry Spec (FROZEN)

```c
typedef struct {
    uint64_t addr;      // angular address (PHI space)
    uint64_t value;     // raw data value
    uint32_t sig;       // fingerprint: addr ^ value ^ phase
    uint8_t  type;      // GHOST=0 BURST=1 SEQ=2 ANOMALY=3
    uint8_t  phase18;   // gate heartbeat (sacred)
    int16_t  delta;     // phase288 - phase306 (world crossing)
} MeshEntry;            // 24B exact
```

**Type priority (highest first):** SEQ > BURST > GHOST > ANOMALY

---

## ReflexBias Spec

```
256 buckets × int8_t bias (keyed by addr >> 12, 4KB granularity)
Penalty: BURST=-3  ANOMALY=-2  GHOST/SEQ=-1
Decay:   7/8 bit-shift every 64 pushes (zero-float, same as FaceState)
Demote:  bias <= -4 AND route==MAIN → demote to GHOST
```

---

## 4 Changes ที่ต้อง Apply ใน POGLS4 repo

### [A] pogls_pipeline_wire.h — เพิ่ม include
```c
#include "pogls_mesh_entry.h"
```

### [B] PipelineWire struct — เพิ่ม field
```c
ReflexBias  reflex;   // หลัง DetachLane detach;
```

### [C] เพิ่ม callback function ก่อน pipeline_wire_init()
```c
static void _pw_mesh_reflex_cb(const MeshEntry *e, void *ctx) {
    PipelineWire *pw = (PipelineWire *)ctx;
    if (!pw || !e) return;
    reflex_update(&pw->reflex, e);
}
```

### [D] pipeline_wire_init() — หลัง detach_lane_start()
```c
reflex_init(&pw->reflex);
pw->detach.mesh_cb  = _pw_mesh_reflex_cb;
pw->detach.mesh_ctx = pw;
```

### [E] route_final: section — ก่อน switch(l3_route)
```c
if (reflex_should_demote(&pw->reflex, angular_addr) &&
    l3_route == ROUTE_MAIN) {
    l3_route = ROUTE_GHOST;
    pw->anchor_ghost++;
}
```

### [F] pogls_detach_lane.h — เพิ่ม fields ใน DetachLane struct
```c
// หลัง uint32_t magic;
void   (*mesh_cb)(const MeshEntry *e, void *ctx);
void    *mesh_ctx;
```

### [G] detach_flush_pass() — หลัง delta_append call
```c
if (dl->mesh_cb) {
    for (uint32_t _mi = 0; _mi < avail; _mi++) {
        DetachEntry *_de = &dl->ring[(t + _mi) & DETACH_RING_MASK];
        if (!is_mesh_anomaly(_de)) continue;
        MeshEntry _me = mesh_translate(_de);
        dl->mesh_cb(&_me, dl->mesh_ctx);
    }
}
```

---

## Next Steps (เรียงตาม impact)

### Priority 1 — Port Snapshot to V4 (BLOCKING, ยังค้างจาก handoff เดิม)
ดู POGLS_Handoff.docx Section 1
```c
typedef enum { SNAP_PENDING=0, SNAP_CERTIFIED=1, SNAP_VOID=2 } snap_state_t;
typedef struct {
    snap_state_t  state;
    uint64_t      merkle_root;
    uint64_t      certified_at;
    uint32_t      lane_mask;
} V4Snapshot;
```

### Priority 2 — POGLS38 consumer ของ MeshEntryBuf
MeshEntryBuf พร้อมแล้ว — POGLS38 เพิ่ม consumer loop:
```c
MeshEntry batch[64];
uint32_t n = mesh_entry_drain(&buf, batch, 64);
// cluster + temporal analysis
```

### Priority 3 — DHC tail callback (30 min)
Wire `dhc_cb` ใน `detach_flush_pass()` ควบคู่กับ `mesh_cb`

### Priority 4 — EngineSlice wire into pipeline_wire
Tag WireBlock ด้วย slice_id จาก EngineSliceSet

---

## Loop Status

```
observe → think → influence

V4 anomaly        ✅ DetachEntry.push() (frozen)
Mesh translate    ✅ mesh_translate() (this session)
Reflex update     ✅ reflex_update() (this session)
Route influence   ✅ reflex_should_demote() (this session)
Decay/recovery    ✅ 7/8 bit-shift (this session)
POGLS38 consumer  ⏳ MeshEntryBuf ready, consumer not yet
Snapshot/Audit    ⏳ BLOCKING (see Priority 1)
```

**System มีชีวิตแล้วในระดับ reflex** — routing ปรับตัวตาม anomaly history
โดยไม่รอ POGLS38 และไม่แตะ frozen layers

---

*POGLS V4 — Ratchaburi, Thailand | 2026-03*
