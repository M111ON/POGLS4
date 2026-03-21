"""
pogls_ingest.py — POGLS V3.5 Ingest Layer
==========================================
Phase 3: เชื่อม POGLSVault + POGLS controller เข้ากับ DeltaFabric

อุดมการณ์:
  • ผู้ใช้เรียก POGLS API เหมือนเดิมทุกอย่าง
  • DeltaFabric ทำงานข้างหลัง — crash-safe โดยอัตโนมัติ
  • ไม่แตะไฟล์ต้นฉบับ

Usage:
    from pogls_ingest import POGLSIngest

    with POGLSIngest("my_vault.pogls") as engine:
        engine.map_point(1.0, -0.5, 2.3)
        engine.commit("after_edit")
"""

import os, sys, math, time, struct, threading
from pathlib import Path
from dataclasses import dataclass
from typing import Optional, List, Callable

sys.path.insert(0, str(Path(__file__).parent))
from pogls_controller import (
    POGLS, POGLSVault, AngularMapper, GeoPoint,
    TopoLevel, Mode, POGLS_MAGIC,
    ANGULAR_FULL_CIRCLE, DEEP_BLOCK_SIZE, SHADOW_BLOCK_SIZE,
)
from pogls_delta_bridge import (
    DeltaFabric, DeltaContext, delta_recover, scan_vault,
    RecoveryResult, AuditError,
    LANE_X, LANE_NX, LANE_Y, LANE_NY,
    PHI_UP, PHI_DOWN, PHI_SCALE, DELTA_MAX_PAYLOAD,
)
from pogls_fabric import BlockFabric, SnapshotPointer


@dataclass
class IngestRecord:
    point:     GeoPoint
    addr_x:    int
    addr_y:    int
    seq:       int
    timestamp: float


class DeltaVault:
    """
    Drop-in replacement สำหรับ POGLSVault
    เพิ่ม crash recovery ผ่าน DeltaFabric
    """

    def __init__(self, vault_path: str, n: int = 20,
                 topo_level: int = TopoLevel.STANDARD,
                 mode: Mode = Mode.DEEP_EDIT,
                 fabric_max_size: int = 0):

        self.vault_path = Path(vault_path).resolve()
        self.mapper     = AngularMapper(n=n, topo_level=topo_level)
        self.mode       = mode
        self.n          = n
        self._seq       = 0
        self._lock      = threading.RLock()
        self._records:  List[IngestRecord] = []
        self._snapshots: List[dict] = []

        # boot recovery
        result = delta_recover(str(self.vault_path))
        self._recovery = result
        icon = {"CLEAN": "✅", "TORN": "⚠️ ", "NEW": "🆕"}.get(result.name, "❓")
        print(f"{icon} DeltaVault [{result.name}]  {self.vault_path.name}")

        # open delta fabric
        self._delta = DeltaFabric(
            str(self.vault_path),
            auto_recover=False,
            fabric_max_size=fabric_max_size or (512 << 20),
        )

        # vault file
        if not self.vault_path.exists():
            self._create_vault()
        else:
            self._load_vault()

    def _create_vault(self):
        with open(self.vault_path, 'wb') as f:
            f.write(struct.pack(">4sBBHQQ",
                POGLS_MAGIC, 3, self.mapper.topo_level,
                self.n, 0, POGLSVault.HEADER_SIZE))

    def _load_vault(self):
        with open(self.vault_path, 'rb') as f:
            raw = f.read(POGLSVault.HEADER_SIZE)
        if raw[:4] != POGLS_MAGIC:
            raise ValueError(f"Invalid POGLS vault: {self.vault_path}")

    # ── WRITE ──────────────────────────────────────────────────────────

    def write_point(self, x: float, y: float, z: float) -> GeoPoint:
        """Map 3D point → delta lanes"""
        point  = self.mapper.map_xyz(x, y, z)
        addr_x = point.addr_x.address
        addr_y = point.addr_y.address

        with self._lock:
            self._seq += 1
            shadow = point.encode_shadow()     # 16B

            # X pair
            self._delta.write(shadow, lane=LANE_X,  addr=addr_x)
            self._delta.write(shadow, lane=LANE_NX, addr=addr_x)
            # Y pair
            self._delta.write(shadow, lane=LANE_Y,  addr=addr_y)
            self._delta.write(shadow, lane=LANE_NY, addr=addr_y)

            # Deep block สำหรับ DEEP_EDIT / WARP
            if self.mode in (Mode.DEEP_EDIT, Mode.WARP):
                self._append_deep(point.encode_deep(), addr_x)

            self._records.append(IngestRecord(
                point=point, addr_x=addr_x, addr_y=addr_y,
                seq=self._seq, timestamp=time.time()))

        return point

    def _append_deep(self, data: bytes, addr: int):
        CHUNK = 200   # < DELTA_MAX_PAYLOAD (224B)
        for i in range(0, len(data), CHUNK):
            chunk = data[i:i + CHUNK]
            self._delta.write(chunk, lane=LANE_X,  addr=addr + i)
            self._delta.write(chunk, lane=LANE_NX, addr=addr + i)

    # ── COMMIT ─────────────────────────────────────────────────────────

    def snapshot(self, label: str = "") -> dict:
        """Crash-safe commit: audit → merkle → fsync → atomic rename"""
        with self._lock:
            try:
                snap_ptr = self._delta.commit()
            except AuditError as e:
                print(f"❌ Commit blocked: {e}")
                raise

            meta = {
                'id':          len(self._snapshots),
                'label':       label or f"snap_{len(self._snapshots)}",
                'timestamp':   time.time(),
                'block_count': self._seq,
                'delta_epoch': self._delta._delta.epoch,
                'snap_ptr':    snap_ptr,
            }
            self._snapshots.append(meta)
            print(f"📸 [{meta['label']}]  epoch={meta['delta_epoch']}  "
                  f"records={self._seq}")
            return meta

    # ── TIME TRAVEL ────────────────────────────────────────────────────

    def time_travel(self, snapshot_id: int) -> "DeltaVault":
        snap = self._snapshots[snapshot_id]
        view = object.__new__(DeltaVault)
        view._records   = self._records[:snap['block_count']]
        view._snapshots = self._snapshots[:snapshot_id]
        view.mapper     = self.mapper
        view.mode       = Mode.TIME_TRAVEL
        view._lock      = threading.RLock()
        print(f"⏳ Time Travel → [{snap['label']}]  epoch={snap['delta_epoch']}")
        return view

    def lookup_by_address(self, axis: str, address: int) -> List[GeoPoint]:
        return [r.point for r in self._records
                if getattr(r.point, f'addr_{axis}').address == address]

    def stats(self) -> dict:
        ds = self._delta.stats
        s = {"records": len(self._records), "snapshots": len(self._snapshots),
             "recovery": self._recovery.name, **ds}
        print(f"╔══ DeltaVault Stats ══╗")
        print(f"  Records:     {s['records']:,}")
        print(f"  Snapshots:   {s['snapshots']}")
        print(f"  Delta epoch: {s['delta_epoch']}")
        print(f"  Recovery:    {s['recovery']}")
        print(f"  Lane seqs:   {s['delta_lane_seq']}")
        print(f"╚═════════════════════╝")
        return s

    def close(self):
        self._delta.close()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


class POGLSIngest:
    """
    Drop-in replacement สำหรับ POGLS
    เพิ่ม crash recovery โดยไม่เปลี่ยน interface

    Before:  p = POGLS("geometry.pogls")
    After:   p = POGLSIngest("geometry.pogls")   ← API เหมือนกัน
    """

    def __init__(self, vault_path: str = "geometry.pogls",
                 n_bits: int = 20,
                 topo_level: int = TopoLevel.STANDARD,
                 mode: Mode = Mode.DEEP_EDIT,
                 fabric_max_size: int = 0):

        self.vault  = DeltaVault(vault_path, n=n_bits,
                                 topo_level=topo_level, mode=mode,
                                 fabric_max_size=fabric_max_size)
        self.mapper = self.vault.mapper

    def map_point(self, x: float, y: float, z: float) -> GeoPoint:
        return self.vault.write_point(x, y, z)

    def map_angle(self, theta: float):
        return self.mapper.angle_to_address(theta)

    def commit(self, label: str = "") -> dict:
        return self.vault.snapshot(label)

    def snapshot(self, label: str = "") -> dict:
        """alias — backward compatible"""
        return self.commit(label)

    def time_travel(self, snapshot_id: int) -> DeltaVault:
        return self.vault.time_travel(snapshot_id)

    def set_topo(self, level: int):
        self.mapper.set_topo(level)
        from pogls_controller import TOPO_VERTEX_TABLE, TOPO_BITS_TABLE
        print(f"🔭 Topology → Level {level} "
              f"({TOPO_VERTEX_TABLE[level]} verts, {TOPO_BITS_TABLE[level]}-bit)")

    def stats(self) -> dict:
        return self.vault.stats()

    @property
    def recovery(self) -> RecoveryResult:
        return self.vault._recovery

    def close(self): self.vault.close()
    def __enter__(self): return self
    def __exit__(self, *_): self.close()


def scan_and_report(vault_dir: str, callback: Optional[Callable] = None) -> dict:
    """Boot scanner — รายงาน recovery status ของทุกไฟล์ใน vault"""
    report = {"clean": [], "torn": [], "new": []}

    def _cb(path, result):
        report[result.name.lower()].append(path)
        if callback:
            callback(path, result)

    scan_vault(vault_dir, callback=_cb)
    print(f"\n{'═'*50}")
    print(f"  Boot Scan: {vault_dir}")
    print(f"  ✅ CLEAN : {len(report['clean'])}")
    print(f"  ⚠️  TORN  : {len(report['torn'])} (recovered)")
    print(f"  🆕 NEW   : {len(report['new'])}")
    print(f"{'═'*50}\n")
    return report
