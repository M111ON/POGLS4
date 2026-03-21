"""
╔══════════════════════════════════════════════════════════════════════╗
║     POGLS V3.1 — Python Controller Layer (Evolutionary Interface)    ║
║                                                                      ║
║  Wraps the C core via ctypes for high-level workflow automation.     ║
║  Handles: Virtual Mount, Time Travel, topology switching,            ║
║           Delta snapshots, and geometry batch operations.            ║
║                                                                      ║
║  V3.1 additions:                                                     ║
║    • V31Adapter  — Python bridge to C Hydra/Audit/VisualFeed        ║
║    • VisualFrame — Python mirror of POGLS_VisualFrame (read-only)   ║
║    • HydraStatus — live head state from VisualFeed JSON feed        ║
║    • SnapshotState enum — mirrors C snap_state_t                    ║
║                                                                      ║
║  Usage:                                                              ║
║    from pogls_controller import POGLS, V31Adapter                   ║
║    p = POGLS(vault_path="my_geometry.pogls", n_bits=20)             ║
║    adapter = V31Adapter()                                            ║
║    frame = adapter.poll_frame()    # read-only visual snapshot      ║
╚══════════════════════════════════════════════════════════════════════╝
"""

import ctypes
import ctypes.util
import math
import os
import struct
import time
import json
import hashlib
import threading
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional, List, Tuple, Dict, Callable
from enum import IntEnum


# ═══════════════════════════════════════════════════════════════════════
# CONSTANTS — mirror of pogls_v3.h (single source of truth is the .h)
# ═══════════════════════════════════════════════════════════════════════

POGLS_MAGIC          = b"POGL"
SHIFT_SHADOW         = 4          # 1 << 4 = 16B
SHIFT_DEEP           = 8          # 1 << 8 = 256B
SHADOW_BLOCK_SIZE    = 1 << SHIFT_SHADOW    # 16
DEEP_BLOCK_SIZE      = 1 << SHIFT_DEEP     # 256
ANGULAR_FULL_CIRCLE  = 2 * math.pi

WARP_MAP_SIZE        = 72
PAYLOAD_SIZE         = 128
PARITY_SIZE          = 32

# Topology table (mirrors C TOPO_VERTEX_TABLE)
TOPO_VERTEX_TABLE = [12, 42, 162, 642, 2562]
TOPO_BITS_TABLE   = [12, 14,  16,  18,   20]

# ── V3.1 constants ──────────────────────────────────────────────────
POGLS_VERSION_V31    = "3.1"
VF_MAGIC             = "VFED"
VF_MAX_EVENTS        = 32
VF_MAX_TILES         = 256
VF_MAX_HEADS         = 16
VF_STALE_WARN_MS     = 500


# ═══════════════════════════════════════════════════════════════════════
# ENUMS
# ═══════════════════════════════════════════════════════════════════════

class Mode(IntEnum):
    TIME_TRAVEL = 0   # Shadow only, beam OFF
    REALTIME    = 1   # Shadow only, beam SPARSE
    DEEP_EDIT   = 2   # Hybrid 256B, beam ACTIVE
    WARP        = 3   # Deep only, beam FULL SCAN

class TopoLevel(IntEnum):
    SEED         = 0   # 12 verts  — 12-bit
    PREVIEW      = 1   # 42 verts  — 14-bit
    STANDARD     = 2   # 162 verts — 16-bit
    HIGH_FIDELITY = 3  # 642 verts — 18-bit
    ULTRA        = 4   # 2562 verts — 20-bit

# ── V3.1 enums ──────────────────────────────────────────────────────

class SnapshotState(IntEnum):
    """mirrors C snap_state_t"""
    PENDING              = 0
    CONFIRMED_CERTIFIED  = 1
    CONFIRMED_AUTO       = 2
    VOID                 = 3
    MIGRATED             = 4

class HeadStatus(IntEnum):
    """mirrors C head_status_t"""
    DORMANT    = 0
    SPAWNING   = 1
    ACTIVE     = 2
    SAFE       = 3
    MIGRATING  = 4
    RETRACTING = 5
    DEAD       = 6

class AuditHealth(IntEnum):
    """mirrors C audit_health_t"""
    OK       = 0
    DEGRADED = 1
    OFFLINE  = 2

class VFEventType(IntEnum):
    """mirrors C vf_event_type_t"""
    CERTIFY       = 0
    ANOMALY       = 1
    VOID          = 2
    HEALTH        = 3
    SPAWN         = 4
    RETRACT       = 5
    SAFE_MODE     = 6
    MIGRATE       = 7
    RECOVERY      = 8
    OVERLAP_DELTA = 9


# ═══════════════════════════════════════════════════════════════════════
# PURE-PYTHON ANGULAR MAPPER
# Implements A = floor( θ × 2^n ) without needing the C lib loaded.
# The C lib is used for high-throughput batch processing.
# ═══════════════════════════════════════════════════════════════════════

@dataclass
class AngularAddress:
    """Result of A = floor(θ × 2^n) — the fundamental POGLS address unit."""
    theta:        float     # Original angle [0, 2π)
    n:            int       # Resolution parameter
    address:      int       # A = floor(θ × 2^n)
    topo_level:   int = 2   # Default: STANDARD
    vertex_count: int = 162

    @property
    def bin_str(self) -> str:
        """Binary representation (n bits wide) — shows alignment clearly."""
        return f"{self.address:0{self.n}b}"

    @property
    def hex_str(self) -> str:
        return f"0x{self.address:0{(self.n+3)//4}X}"

    def __repr__(self):
        return (f"AngularAddress(θ={self.theta:.8f}rad, "
                f"n={self.n}, A={self.address}, "
                f"topo={self.topo_level}[{self.vertex_count}v])")


@dataclass
class GeoPoint:
    """3D geometry point with three independent angular addresses."""
    addr_x:     AngularAddress
    addr_y:     AngularAddress
    addr_z:     AngularAddress
    dirty_bits: int = 0
    topo_level: int = 2
    timestamp:  float = field(default_factory=time.time)

    def mark_dirty(self, flags: int = 0xFF):
        self.dirty_bits |= flags

    def clear_dirty(self):
        self.dirty_bits = 0

    def encode_shadow(self) -> bytes:
        """
        Encode as Shadow Block (16 bytes = 2^4).
        Layout: [coord_scaled:4][vector_flags:4][deep_link:8]
        coord_scaled = addr_x.address (dominant axis for quick lookup)
        """
        coord_scaled = ctypes.c_int32(self.addr_x.address & 0x7FFFFFFF).value
        vector_flags = (self.topo_level & 0xF) | ((self.dirty_bits & 0xFF) << 4)
        deep_link    = 0   # Will be filled when Deep block is written
        return struct.pack(">iIQ", coord_scaled, vector_flags, deep_link)

    def encode_deep(self) -> bytes:
        """
        Encode as Deep Block (256 bytes = 2^8).
        Layout: [warp_map:72][payload:128][parity:32]
        Total: 232 bytes (+ 24B header written separately by vault)
        """
        # Warp Map: mark active axis regions
        warp = bytearray(WARP_MAP_SIZE)
        # X-axis occupies warp bytes 0-23, Y: 24-47, Z: 48-71
        x_flag = 0x01 if self.addr_x.address > 0 else 0x00
        y_flag = 0x02 if self.addr_y.address > 0 else 0x00
        z_flag = 0x04 if self.addr_z.address > 0 else 0x00
        for i in range(24):
            warp[i]      = x_flag
            warp[24 + i] = y_flag
            warp[48 + i] = z_flag

        # Payload: pack the three addresses + metadata
        payload = bytearray(PAYLOAD_SIZE)
        # Bytes 0-23: X address (8B) + theta (8B) + n (4B) + reserved (4B)
        struct.pack_into(">QdI4x", payload, 0,
                         self.addr_x.address, self.addr_x.theta, self.addr_x.n)
        struct.pack_into(">QdI4x", payload, 24,
                         self.addr_y.address, self.addr_y.theta, self.addr_y.n)
        struct.pack_into(">QdI4x", payload, 48,
                         self.addr_z.address, self.addr_z.theta, self.addr_z.n)
        struct.pack_into(">BBI14x", payload, 72,
                         self.topo_level, self.dirty_bits, TOPO_VERTEX_TABLE[self.topo_level])

        # Parity: SHA-256 of payload truncated to 32 bytes
        parity = hashlib.sha256(bytes(payload)).digest()

        return bytes(warp) + bytes(payload) + parity


class AngularMapper:
    """
    Pure Python implementation of POGLS angular addressing.
    THE LAW: A = floor(θ × 2^n)
    """

    def __init__(self, n: int = 20, topo_level: int = TopoLevel.STANDARD):
        self.n          = n
        self.topo_level = topo_level
        self._two_n     = 1 << n   # 2^n, computed ONCE (bit-shift)

    def angle_to_address(self, theta: float) -> AngularAddress:
        """Core computation: A = floor(θ × 2^n)"""
        # Normalize θ to [0, 2π)
        theta = theta % ANGULAR_FULL_CIRCLE
        if theta < 0:
            theta += ANGULAR_FULL_CIRCLE

        address = int(theta * self._two_n)  # floor via int truncation

        # Clamp to valid range [0, 2^n - 1]
        address = address & (self._two_n - 1)  # bit-mask, not modulo

        return AngularAddress(
            theta=theta,
            n=self.n,
            address=address,
            topo_level=self.topo_level,
            vertex_count=TOPO_VERTEX_TABLE[self.topo_level]
        )

    def address_to_angle(self, address: int) -> float:
        """Inverse: θ = (A + 0.5) / 2^n × 2π  (cell midpoint)"""
        address = address & (self._two_n - 1)
        return ((address + 0.5) / self._two_n) * ANGULAR_FULL_CIRCLE

    def map_xyz(self, x: float, y: float, z: float,
                range_min: float = -1.0,
                range_max: float = 1.0) -> GeoPoint:
        """
        Map a 3D geometry coordinate to angular addresses.

        1. Normalize each axis to [0, 1)
        2. Scale to [0, 2π)
        3. A = floor(θ × 2^n) per axis
        """
        def _axis_to_addr(val: float) -> AngularAddress:
            # Clamp
            val = max(range_min, min(range_max, val))
            # Normalize [0, 1)
            normalized = (val - range_min) / (range_max - range_min)
            # Prevent hitting exactly 1.0 (would alias to 2π → address 0)
            normalized = min(normalized, 1.0 - 1e-15)
            theta = normalized * ANGULAR_FULL_CIRCLE
            return self.angle_to_address(theta)

        return GeoPoint(
            addr_x=_axis_to_addr(x),
            addr_y=_axis_to_addr(y),
            addr_z=_axis_to_addr(z),
            topo_level=self.topo_level
        )

    def set_topo(self, level: int):
        """Switch topology level (marks all subsequent points as new topo)."""
        self.topo_level = max(0, min(4, level))

    def address_space_size(self) -> int:
        """Total number of addresses in this resolution: 2^n"""
        return self._two_n

    def resolution_per_radian(self) -> float:
        """How many addresses per radian of angular space."""
        return self._two_n / ANGULAR_FULL_CIRCLE

    def describe(self):
        print(f"╔══ POGLS Angular Mapper ══╗")
        print(f"  n          = {self.n}")
        print(f"  2^n        = {self._two_n:,} addresses")
        print(f"  Topo Level = {self.topo_level} "
              f"({TOPO_VERTEX_TABLE[self.topo_level]} vertices, "
              f"{TOPO_BITS_TABLE[self.topo_level]}-bit precision)")
        print(f"  Res/radian = {self.resolution_per_radian():,.0f} addrs/rad")
        print(f"╚══════════════════════════╝")


# ═══════════════════════════════════════════════════════════════════════
# VIRTUAL VAULT — Pure-Python file engine (no C lib needed)
# Manages Shadow and Deep blocks, Delta snapshots, Time Travel.
# ═══════════════════════════════════════════════════════════════════════

class POGLSVault:
    """
    Pure-Python POGLS V3 vault engine.

    File layout:
        [POGLS_Header: 24B]
        [Shadow blocks: N × 16B]
        [Deep blocks:   M × 256B]

    Snapshot (Time Travel):
        Creates a tiny delta file containing ONLY changed Shadow blocks.
        The base vault is never overwritten — read-only during delta.
    """

    HEADER_SIZE = 24   # Smart Header

    def __init__(self, vault_path: str, n: int = 20,
                 topo_level: int = TopoLevel.STANDARD,
                 mode: Mode = Mode.DEEP_EDIT):
        self.vault_path  = Path(vault_path)
        self.mapper      = AngularMapper(n=n, topo_level=topo_level)
        self.mode        = mode
        self.n           = n
        self._shadow_blocks: List[GeoPoint] = []
        self._snapshots: List[dict] = []   # Time Travel stack

        # Load or create vault
        if self.vault_path.exists():
            self._load()
        else:
            self._create()

    def _create(self):
        """Create new vault with Smart Header."""
        with open(self.vault_path, 'wb') as f:
            header = struct.pack(
                ">4sBBHQQ",
                POGLS_MAGIC,          # magic
                3,                    # version = V3
                self.mapper.topo_level,  # adaptive_level
                self.n,              # ratio_bits = n
                0,                   # total_blocks
                self.HEADER_SIZE     # root_offset (data starts after header)
            )
            f.write(header)
        print(f"✅ Created POGLS vault: {self.vault_path}")

    def _load(self):
        """Load existing vault header."""
        with open(self.vault_path, 'rb') as f:
            raw = f.read(self.HEADER_SIZE)
        magic = raw[0:4]
        if magic != POGLS_MAGIC:
            raise ValueError(f"Invalid POGLS vault (magic={magic})")
        print(f"📂 Loaded POGLS vault: {self.vault_path}")

    def write_point(self, x: float, y: float, z: float) -> GeoPoint:
        """
        Map a 3D point, encode to Shadow+Deep blocks, write to vault.
        Returns the GeoPoint with all three angular addresses.
        """
        point = self.mapper.map_xyz(x, y, z)

        if self.mode in (Mode.REALTIME, Mode.TIME_TRAVEL):
            # Shadow lane only — fast path
            self._write_shadow_block(point)
        else:
            # Hybrid: write both Shadow and Deep
            self._write_shadow_block(point)
            self._write_deep_block(point)

        self._shadow_blocks.append(point)
        self._update_header()
        return point

    def _write_shadow_block(self, point: GeoPoint):
        idx = len(self._shadow_blocks)
        offset = self.HEADER_SIZE + (idx << SHIFT_SHADOW)  # bit-shift, not multiply

        with open(self.vault_path, 'r+b') as f:
            f.seek(offset)
            f.write(point.encode_shadow())

    def _write_deep_block(self, point: GeoPoint):
        """
        Deep blocks are appended after all Shadow blocks.
        Offset: HEADER_SIZE + (shadow_count × 16) + (deep_index × 256)
        """
        shadow_total = max(len(self._shadow_blocks) + 1, 1)
        deep_idx     = len([p for p in self._shadow_blocks
                            if p.topo_level >= TopoLevel.STANDARD])

        shadow_region_size = shadow_total << SHIFT_SHADOW   # bit-shift
        deep_offset = self.HEADER_SIZE + shadow_region_size + (deep_idx << SHIFT_DEEP)

        with open(self.vault_path, 'r+b') as f:
            # Ensure file is large enough
            f.seek(0, 2)  # End of file
            eof = f.tell()
            if eof < deep_offset + DEEP_BLOCK_SIZE:
                f.write(b'\x00' * (deep_offset + DEEP_BLOCK_SIZE - eof))
            f.seek(deep_offset)
            f.write(point.encode_deep())

    def _update_header(self):
        """Update total_blocks count in Smart Header."""
        with open(self.vault_path, 'r+b') as f:
            f.seek(8)   # Offset of total_blocks field
            f.write(struct.pack(">Q", len(self._shadow_blocks)))

    # ─── TIME TRAVEL ───────────────────────────────────────────────────

    def snapshot(self, label: str = "") -> str:
        """
        Create a Time Travel snapshot (Delta file).
        Stores ONLY the Shadow block indices that changed since last snapshot.
        Base vault is NEVER modified.
        """
        snap_path = str(self.vault_path) + f".snap_{len(self._snapshots)}"

        # Write delta: only Shadow blocks since last snapshot
        last_snap_idx = self._snapshots[-1]['block_count'] if self._snapshots else 0
        new_blocks = self._shadow_blocks[last_snap_idx:]

        with open(snap_path, 'wb') as f:
            # Delta header: [magic:4][snap_id:4][base_count:8][delta_count:8]
            f.write(struct.pack(">4sIQQ",
                                b"PDLT",
                                len(self._snapshots),
                                last_snap_idx,
                                len(new_blocks)))
            # Write only changed Shadow blocks (16B each)
            for i, pt in enumerate(new_blocks):
                f.write(struct.pack(">Q", last_snap_idx + i))  # block index
                f.write(pt.encode_shadow())                      # 16B shadow

        meta = {
            'id':          len(self._snapshots),
            'label':       label or f"snap_{len(self._snapshots)}",
            'timestamp':   time.time(),
            'block_count': len(self._shadow_blocks),
            'delta_path':  snap_path,
            'delta_blocks': len(new_blocks),
        }
        self._snapshots.append(meta)

        snap_kb = Path(snap_path).stat().st_size / 1024
        print(f"📸 Snapshot: {meta['label']} | "
              f"{len(new_blocks)} delta blocks | {snap_kb:.1f}KB "
              f"(base={Path(self.vault_path).stat().st_size/1024:.0f}KB)")
        return snap_path

    def time_travel(self, snapshot_id: int) -> 'POGLSVault':
        """
        Return a view of the vault at snapshot_id.
        Reads: base blocks UP TO snap's base_count + delta file.
        Base vault file is untouched.
        """
        snap = self._snapshots[snapshot_id]
        print(f"⏳ Time Travel → {snap['label']} "
              f"(t={time.strftime('%H:%M:%S', time.localtime(snap['timestamp']))})")
        # In production: mount virtual overlay (base + delta)
        # Here: return a read-only view slice
        view = object.__new__(POGLSVault)
        view._shadow_blocks = self._shadow_blocks[:snap['block_count']]
        view._snapshots = self._snapshots[:snapshot_id]
        view.mapper = self.mapper
        view.mode = Mode.TIME_TRAVEL
        return view

    # ─── QUERY ─────────────────────────────────────────────────────────

    def lookup_by_address(self, axis: str, address: int) -> List[GeoPoint]:
        """Find all points matching an angular address on given axis."""
        results = []
        for pt in self._shadow_blocks:
            addr = getattr(pt, f'addr_{axis}')
            if addr.address == address:
                results.append(pt)
        return results

    def stats(self):
        total_bytes = Path(self.vault_path).stat().st_size if self.vault_path.exists() else 0
        print(f"╔══ POGLS Vault Stats ══╗")
        print(f"  Points:     {len(self._shadow_blocks):,}")
        print(f"  Snapshots:  {len(self._snapshots)}")
        print(f"  Vault size: {total_bytes/1024:.1f} KB")
        print(f"  Mode:       {Mode(self.mode).name}")
        self.mapper.describe()


# ═══════════════════════════════════════════════════════════════════════
# HIGH-LEVEL API (The "Evolutionary Interface")
# ═══════════════════════════════════════════════════════════════════════

class POGLS:
    """
    Top-level POGLS V3 controller.
    Wraps the mapper + vault in a clean, workflow-oriented API.

    Example:
        p = POGLS("geometry.pogls", n_bits=20)
        pt = p.map_point(1.0, -0.5, 2.3)
        p.snapshot("before_deform")
        pt2 = p.map_point(1.1, -0.4, 2.3)   # edit
        p.time_travel(0)                      # undo
    """

    def __init__(self, vault_path: str = "geometry.pogls",
                 n_bits: int = 20,
                 topo_level: int = TopoLevel.STANDARD,
                 mode: Mode = Mode.DEEP_EDIT):
        self.vault = POGLSVault(vault_path, n=n_bits,
                                topo_level=topo_level, mode=mode)
        self.mapper = self.vault.mapper

    def map_point(self, x: float, y: float, z: float) -> GeoPoint:
        """Map a 3D geometry point to angular addresses and persist."""
        return self.vault.write_point(x, y, z)

    def map_angle(self, theta: float) -> AngularAddress:
        """Direct angle → address conversion."""
        return self.mapper.angle_to_address(theta)

    def snapshot(self, label: str = "") -> str:
        """Create a Time Travel snapshot of current state."""
        return self.vault.snapshot(label)

    def time_travel(self, snapshot_id: int):
        """Return vault view at given snapshot."""
        return self.vault.time_travel(snapshot_id)

    def set_topo(self, level: int):
        """Change topology resolution level (triggers Checker Beam)."""
        self.mapper.set_topo(level)
        print(f"🔭 Topology → Level {level} "
              f"({TOPO_VERTEX_TABLE[level]} verts, "
              f"{TOPO_BITS_TABLE[level]}-bit)")

    def stats(self):
        self.vault.stats()


# ═══════════════════════════════════════════════════════════════════════
# V3.1 DATA CLASSES — Python mirror of C VisualFeed structs
# ═══════════════════════════════════════════════════════════════════════

@dataclass
class VF_TileSnap:
    """Python mirror of C VF_TileSnap — from Audit source"""
    index:            int
    state:            str          # tile_state_t name
    anomaly_flags:    int          # bitmask
    blocks_scanned:   int
    blocks_anomalous: int
    addr_start:       int
    addr_end:         int
    scanned_at_ms:    int
    overlap_hash:     str          # hex string

    @property
    def is_clean(self) -> bool:
        return self.anomaly_flags == 0 and self.state == "CLEAN"

    @property
    def anomaly_names(self) -> List[str]:
        names = []
        if self.anomaly_flags & 0x01: names.append("HASH_MISMATCH")
        if self.anomaly_flags & 0x02: names.append("COORD_DRIFT")
        if self.anomaly_flags & 0x04: names.append("OVERLAP_DELTA")
        if self.anomaly_flags & 0x08: names.append("WARP_CORRUPT")
        if self.anomaly_flags & 0x10: names.append("DEEP_UNREADABLE")
        if self.anomaly_flags & 0x20: names.append("SEQUENCE_BREAK")
        return names

    def to_dict(self) -> dict:
        return {
            "index":   self.index,
            "state":   self.state,
            "anomaly": f"0x{self.anomaly_flags:02X}",
            "anomaly_names": self.anomaly_names,
            "blocks":  f"{self.blocks_anomalous}/{self.blocks_scanned}",
            "addr":    f"{self.addr_start}-{self.addr_end}",
        }


@dataclass
class VF_HeadSnap:
    """Python mirror of C VF_HeadSnap — from Hydra source"""
    head_id:             int
    status:              str          # head_status_t name
    branch_id:           int
    zone_offset_start:   int
    zone_offset_end:     int
    last_active_ms:      int
    write_count:         int
    current_block_count: int
    peak_block_count:    int
    anomaly_count:       int

    @property
    def zone_mb(self) -> float:
        return (self.zone_offset_end - self.zone_offset_start) / (1 << 20)

    @property
    def is_healthy(self) -> bool:
        return self.status == "ACTIVE"

    @property
    def needs_attention(self) -> bool:
        return self.status in ("SAFE", "MIGRATING")

    def to_dict(self) -> dict:
        return {
            "head_id":   self.head_id,
            "status":    self.status,
            "branch":    f"0x{self.branch_id:016X}",
            "zone_mb":   round(self.zone_mb, 2),
            "writes":    self.write_count,
            "anomalies": self.anomaly_count,
        }


@dataclass
class VF_Event:
    """Python mirror of C VF_Event"""
    type:         str          # vf_event_type_t name
    severity:     int          # 0=info 1=warn 2=critical
    head_id:      int          # 0xFF = not head-specific
    tile_idx:     int          # 0xFF = not tile-specific
    branch_id:    int
    snapshot_id:  int
    event_at_ms:  int
    anomaly_flags: int

    @property
    def is_critical(self) -> bool:
        return self.severity >= 2

    def to_dict(self) -> dict:
        return {
            "type":     self.type,
            "severity": ["INFO", "WARN", "CRITICAL"][min(self.severity, 2)],
            "head":     self.head_id if self.head_id != 0xFF else None,
            "tile":     self.tile_idx if self.tile_idx != 0xFF else None,
            "at_ms":    self.event_at_ms,
        }


@dataclass
class VisualFrame:
    """
    Python mirror of C POGLS_VisualFrame.
    Produced by V31Adapter.poll_frame() — always read-only.
    GUI และ ComfyUI bridge ใช้ object นี้เท่านั้น ไม่แตะ Core ตรง
    """
    frame_seq:           int
    frame_at_ms:         int
    frame_age_ms:        int
    is_stale:            bool
    audit_health:        str          # "OK" | "DEGRADED" | "OFFLINE"
    active_heads:        int
    total_scans:         int
    total_anomalies:     int
    scan_duration_ms:    int
    radar_spawn_count:   int
    radar_retract_count: int
    radar_incident_count:int
    tiles:               List[VF_TileSnap]
    heads:               List[VF_HeadSnap]
    events:              List[VF_Event]

    # timestamp เมื่อ Python รับ frame นี้
    received_at:         float = field(default_factory=time.time)

    @property
    def has_critical(self) -> bool:
        return any(e.is_critical for e in self.events)

    @property
    def anomalous_tiles(self) -> List[VF_TileSnap]:
        return [t for t in self.tiles if not t.is_clean]

    @property
    def unhealthy_heads(self) -> List[VF_HeadSnap]:
        return [h for h in self.heads if h.needs_attention]

    def summary(self) -> str:
        health_icon = {"OK": "✅", "DEGRADED": "⚠️", "OFFLINE": "❌"}.get(
            self.audit_health, "?")
        stale = " [STALE]" if self.is_stale else ""
        return (f"Frame#{self.frame_seq}{stale} {health_icon} "
                f"Heads:{self.active_heads} "
                f"Tiles:{len(self.anomalous_tiles)}anom/{len(self.tiles)} "
                f"Events:{len(self.events)} "
                f"Age:{self.frame_age_ms}ms")

    def to_comfyui_node(self) -> dict:
        """Format สำหรับ ComfyUI custom node"""
        return {
            "type":    "POGLS_VISUAL_FRAME",
            "version": POGLS_VERSION_V31,
            "frame":   self.frame_seq,
            "health":  self.audit_health,
            "stale":   self.is_stale,
            "hydra": {
                "active_heads":   self.active_heads,
                "spawn_count":    self.radar_spawn_count,
                "retract_count":  self.radar_retract_count,
                "incident_count": self.radar_incident_count,
                "heads": [h.to_dict() for h in self.heads],
            },
            "audit": {
                "health":       self.audit_health,
                "total_scans":  self.total_scans,
                "anomalies":    self.total_anomalies,
                "scan_ms":      self.scan_duration_ms,
                "tiles": [t.to_dict() for t in self.anomalous_tiles],
            },
            "events": [e.to_dict() for e in self.events],
            "has_critical": self.has_critical,
        }


# ═══════════════════════════════════════════════════════════════════════
# V31Adapter — Python bridge to C VisualFeed JSON feed
#
# สองโหมด:
#   1. JSON pipe mode  — อ่าน JSON lines จาก C VisualFeed ผ่าน pipe/file
#                        ใช้ในการ deploy จริงกับ C binary
#   2. Simulation mode — สร้าง VisualFrame จำลองโดยไม่ต้องมี C process
#                        ใช้สำหรับ GUI development และ ComfyUI bridge dev
#
# PRINCIPLE: V31Adapter ไม่มี write path ไปยัง Core เลย
#   ทุก method เป็น read หรือ subscribe เท่านั้น
# ═══════════════════════════════════════════════════════════════════════

class V31Adapter:
    """
    Python bridge to POGLS_VisualFeed (C layer).

    ใช้งาน:
        adapter = V31Adapter()                  # simulation mode
        frame = adapter.poll_frame()            # get latest VisualFrame
        adapter.subscribe(callback)             # auto-notify on new frame
        adapter.stop()                          # stop background thread
    """

    def __init__(self, feed_path: Optional[str] = None,
                 poll_interval_ms: int = 200):
        """
        feed_path=None  → simulation mode (ไม่ต้องมี C process)
        feed_path=str   → read JSON lines from file/pipe (C VisualFeed output)
        """
        self._feed_path      = feed_path
        self._poll_interval  = poll_interval_ms / 1000.0
        self._latest_frame:  Optional[VisualFrame] = None
        self._frame_seq:     int = 0
        self._callbacks:     List[Callable[[VisualFrame], None]] = []
        self._lock           = threading.Lock()
        self._running        = False
        self._thread:        Optional[threading.Thread] = None
        self._sim_state      = self._init_sim_state()

        # Start background poller if feed_path given
        if feed_path:
            self.start()

    # ── Simulation state ─────────────────────────────────────────────

    def _init_sim_state(self) -> dict:
        """Initial simulation state — mirrors what C Hydra would report"""
        return {
            "heads": [
                {"id": 0, "status": "ACTIVE",   "branch": 0xDEADBEEF0001,
                 "zone_start": 0, "zone_end": 2<<20, "writes": 0, "anom": 0},
                {"id": 1, "status": "ACTIVE",   "branch": 0xDEADBEEF0002,
                 "zone_start": 6<<20, "zone_end": 8<<20, "writes": 0, "anom": 0},
            ],
            "audit_health": "OK",
            "total_scans":  0,
            "total_anomalies": 0,
            "spawn_count":  2,
            "retract_count":0,
            "incident_count":0,
            "tile_count":   8,
            "event_log":    [],    # pending events for next frame
        }

    # ── Public API ────────────────────────────────────────────────────

    def poll_frame(self) -> VisualFrame:
        """
        Get latest VisualFrame.
        Simulation mode: generates fresh frame each call.
        Pipe mode: returns last received frame (non-blocking).
        """
        if self._feed_path:
            with self._lock:
                return self._latest_frame or self._build_sim_frame()
        else:
            return self._build_sim_frame()

    def subscribe(self, callback: Callable[[VisualFrame], None]):
        """Register callback — called on every new frame (background thread)"""
        self._callbacks.append(callback)
        if not self._running:
            self.start()

    def inject_event(self, event_type: str, severity: int = 0,
                     head_id: int = 0xFF, branch_id: int = 0):
        """
        Inject a synthetic event into simulation.
        ใช้สำหรับ GUI testing / ComfyUI bridge testing
        """
        with self._lock:
            self._sim_state["event_log"].append({
                "type": event_type, "severity": severity,
                "head_id": head_id, "branch_id": branch_id,
                "at_ms": int(time.time() * 1000),
            })

    def simulate_anomaly(self, head_id: int = 0, critical: bool = False):
        """Simulate anomaly on a specific head — for GUI testing"""
        sev = 2 if critical else 1
        evt = "MIGRATE" if critical else "SAFE_MODE"
        with self._lock:
            for h in self._sim_state["heads"]:
                if h["id"] == head_id:
                    h["status"] = "MIGRATING" if critical else "SAFE"
                    h["anom"] += 1
                    break
            self._sim_state["incident_count"] += 1
            self._sim_state["event_log"].append({
                "type": evt, "severity": sev,
                "head_id": head_id, "branch_id": 0,
                "at_ms": int(time.time() * 1000),
            })

    def simulate_spawn(self, zone_start_mb: int = 10, zone_end_mb: int = 12):
        """Simulate Hydra spawning a new head"""
        with self._lock:
            new_id = max((h["id"] for h in self._sim_state["heads"]),
                         default=-1) + 1
            self._sim_state["heads"].append({
                "id": new_id, "status": "SPAWNING",
                "branch": int(time.time() * 1000) ^ (new_id << 48),
                "zone_start": zone_start_mb << 20,
                "zone_end":   zone_end_mb << 20,
                "writes": 0, "anom": 0,
            })
            self._sim_state["spawn_count"] += 1
            self._sim_state["event_log"].append({
                "type": "SPAWN", "severity": 0,
                "head_id": new_id, "branch_id": 0,
                "at_ms": int(time.time() * 1000),
            })

    def stop(self):
        """Stop background polling thread"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=1.0)
            self._thread = None

    def start(self):
        """Start background polling thread"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(
            target=self._poll_loop, daemon=True, name="V31Adapter-poll")
        self._thread.start()

    # ── Internal frame builder ────────────────────────────────────────

    def _build_sim_frame(self) -> VisualFrame:
        """Build a VisualFrame from simulation state"""
        with self._lock:
            state = self._sim_state
            self._frame_seq += 1
            now_ms = int(time.time() * 1000)

            # Tiles — simulate CLEAN tiles (no C scan in simulation)
            tiles = []
            tile_space = 1 << 20   # 2^20 addresses
            tile_size  = tile_space // state["tile_count"]
            for i in range(state["tile_count"]):
                tiles.append(VF_TileSnap(
                    index=i,
                    state="CLEAN",
                    anomaly_flags=0,
                    blocks_scanned=64,
                    blocks_anomalous=0,
                    addr_start=i * tile_size,
                    addr_end=(i + 1) * tile_size,
                    scanned_at_ms=now_ms - 100,
                    overlap_hash="00" * 8,
                ))

            # Heads
            heads = []
            for h in state["heads"]:
                if h["status"] in ("DORMANT", "DEAD"):
                    continue
                heads.append(VF_HeadSnap(
                    head_id=h["id"],
                    status=h["status"],
                    branch_id=h["branch"],
                    zone_offset_start=h["zone_start"],
                    zone_offset_end=h["zone_end"],
                    last_active_ms=now_ms - 50,
                    write_count=h["writes"],
                    current_block_count=0,
                    peak_block_count=0,
                    anomaly_count=h["anom"],
                ))

            # Events — drain pending log
            events = []
            for ev in state["event_log"]:
                events.append(VF_Event(
                    type=ev["type"],
                    severity=ev["severity"],
                    head_id=ev.get("head_id", 0xFF),
                    tile_idx=0xFF,
                    branch_id=ev.get("branch_id", 0),
                    snapshot_id=0,
                    event_at_ms=ev["at_ms"],
                    anomaly_flags=0,
                ))
            state["event_log"] = []   # consumed

            active = sum(1 for h in heads if h.status == "ACTIVE")
            frame = VisualFrame(
                frame_seq=self._frame_seq,
                frame_at_ms=now_ms,
                frame_age_ms=100,
                is_stale=False,
                audit_health=state["audit_health"],
                active_heads=active,
                total_scans=state["total_scans"],
                total_anomalies=state["total_anomalies"],
                scan_duration_ms=12,
                radar_spawn_count=state["spawn_count"],
                radar_retract_count=state["retract_count"],
                radar_incident_count=state["incident_count"],
                tiles=tiles,
                heads=heads,
                events=events,
            )
            state["total_scans"] += 1
            return frame

    def _build_frame_from_json(self, line: str) -> Optional[VisualFrame]:
        """Parse a JSON line from C VisualFeed output → VisualFrame"""
        try:
            d = json.loads(line)
            now_ms = int(time.time() * 1000)

            tiles = []
            for i, t in enumerate(d.get("tiles", [])):
                # Parse anomaly hex "0x04" → int
                anom_str = t.get("anom", "0x00")
                anom = int(anom_str, 16) if isinstance(anom_str, str) else anom_str
                tiles.append(VF_TileSnap(
                    index=t.get("i", i),
                    state=t.get("s", "IDLE"),
                    anomaly_flags=anom,
                    blocks_scanned=0,
                    blocks_anomalous=0,
                    addr_start=0, addr_end=0,
                    scanned_at_ms=now_ms,
                    overlap_hash="00" * 8,
                ))

            heads = []
            for h in d.get("heads", []):
                heads.append(VF_HeadSnap(
                    head_id=h.get("id", 0),
                    status=h.get("status", "UNKNOWN"),
                    branch_id=h.get("branch", 0),
                    zone_offset_start=0,
                    zone_offset_end=int(h.get("zone_mb", 0)) << 20,
                    last_active_ms=now_ms,
                    write_count=h.get("writes", 0),
                    current_block_count=0,
                    peak_block_count=0,
                    anomaly_count=h.get("anom", 0),
                ))

            events = []
            for ev in d.get("events", []):
                events.append(VF_Event(
                    type=ev.get("type", "UNKNOWN"),
                    severity=ev.get("sev", 0),
                    head_id=ev.get("head", 0xFF),
                    tile_idx=ev.get("tile", 0xFF),
                    branch_id=0, snapshot_id=0,
                    event_at_ms=ev.get("at", now_ms),
                    anomaly_flags=0,
                ))

            audit = d.get("audit", {})
            hydra = d.get("hydra", {})
            return VisualFrame(
                frame_seq=d.get("frame", 0),
                frame_at_ms=d.get("at", now_ms),
                frame_age_ms=d.get("age", 0),
                is_stale=d.get("stale", False),
                audit_health=audit.get("health", "OK"),
                active_heads=hydra.get("active", 0),
                total_scans=audit.get("scans", 0),
                total_anomalies=audit.get("anomalies", 0),
                scan_duration_ms=audit.get("scan_ms", 0),
                radar_spawn_count=hydra.get("spawn", 0),
                radar_retract_count=hydra.get("retract", 0),
                radar_incident_count=hydra.get("incident", 0),
                tiles=tiles, heads=heads, events=events,
            )
        except (json.JSONDecodeError, KeyError, ValueError):
            return None

    def _poll_loop(self):
        """Background thread — reads JSON lines from feed_path"""
        if not self._feed_path:
            # Simulation mode polling
            while self._running:
                frame = self._build_sim_frame()
                with self._lock:
                    self._latest_frame = frame
                for cb in self._callbacks:
                    try:
                        cb(frame)
                    except Exception:
                        pass
                time.sleep(self._poll_interval)
            return

        # Pipe/file mode
        try:
            with open(self._feed_path, 'r', encoding='utf-8') as f:
                while self._running:
                    line = f.readline()
                    if not line:
                        time.sleep(0.05)
                        continue
                    frame = self._build_frame_from_json(line.strip())
                    if frame:
                        with self._lock:
                            self._latest_frame = frame
                        for cb in self._callbacks:
                            try:
                                cb(frame)
                            except Exception:
                                pass
        except (FileNotFoundError, PermissionError) as e:
            print(f"[V31Adapter] feed error: {e} — switching to simulation")
            self._feed_path = None
            self._poll_loop()   # recurse into sim mode


# ═══════════════════════════════════════════════════════════════════════
# QUICK SELF-TEST
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    import tempfile, os

    print("═" * 60)
    print("  POGLS V3.1 — Python Controller Self-Test")
    print("═" * 60)

    with tempfile.TemporaryDirectory() as tmp:
        vault_path = os.path.join(tmp, "test.pogls")
        p = POGLS(vault_path=vault_path, n_bits=20, topo_level=TopoLevel.STANDARD)
        p.mapper.describe()

        print("\n▶ Mapping angles:")
        for theta in [0, math.pi/4, math.pi/2, math.pi, 3*math.pi/2]:
            a = p.map_angle(theta)
            print(f"  θ={theta:.4f}  A={a.address:>8}  bin={a.bin_str[:16]}...")

        print("\n▶ Mapping 3D geometry points:")
        test_points = [
            (0.0,  0.0,  0.0),
            (0.5,  0.5,  0.5),
            (-1.0, 0.0,  1.0),
            (0.75, -0.25, 0.1),
        ]
        for x, y, z in test_points:
            pt = p.map_point(x, y, z)
            print(f"  ({x:5.2f},{y:5.2f},{z:5.2f}) → "
                  f"X:{pt.addr_x.address:>8} "
                  f"Y:{pt.addr_y.address:>8} "
                  f"Z:{pt.addr_z.address:>8}")

        print("\n▶ Time Travel:")
        p.snapshot("initial_state")
        p.map_point(0.9, 0.9, 0.9)
        p.snapshot("after_edit")
        p.time_travel(0)

        print("\n▶ Topology switch:")
        p.set_topo(TopoLevel.ULTRA)
        pt = p.map_point(0.1, 0.2, 0.3)
        print(f"  Ultra-detail: A_x={pt.addr_x.address}")

        print("\n▶ Stats:")
        p.stats()

    # ─── V3.1 Adapter Tests ──────────────────────────────────────────
    print("\n" + "─" * 60)
    print("  V3.1 Adapter — VisualFeed (simulation mode)")
    print("─" * 60)

    adapter = V31Adapter(poll_interval_ms=100)

    # poll_frame
    frame = adapter.poll_frame()
    assert frame.frame_seq >= 1, "frame_seq should be >= 1"
    assert frame.audit_health == "OK", "default health should be OK"
    assert len(frame.tiles) == 8, "default sim has 8 tiles"
    assert len(frame.heads) == 2, "default sim has 2 heads"
    assert not frame.is_stale, "fresh frame should not be stale"
    print(f"  poll_frame()     → {frame.summary()}")

    # inject anomaly
    adapter.simulate_anomaly(head_id=0, critical=False)
    frame2 = adapter.poll_frame()
    warn_heads = frame2.unhealthy_heads
    assert any(h.head_id == 0 for h in warn_heads), "head 0 should be SAFE"
    print(f"  simulate_anomaly → {frame2.summary()}")

    # inject critical
    adapter.simulate_anomaly(head_id=1, critical=True)
    frame3 = adapter.poll_frame()
    assert frame3.has_critical, "critical anomaly should set has_critical"
    print(f"  critical anomaly → {frame3.summary()}")

    # spawn simulation
    adapter.simulate_spawn(zone_start_mb=20, zone_end_mb=22)
    frame4 = adapter.poll_frame()
    spawn_events = [e for e in frame4.events if e.type == "SPAWN"]
    assert spawn_events, "spawn event should appear in frame"
    print(f"  simulate_spawn   → {frame4.summary()}")

    # to_comfyui_node
    node = frame4.to_comfyui_node()
    assert node["type"] == "POGLS_VISUAL_FRAME", "wrong node type"
    assert node["version"] == POGLS_VERSION_V31, "wrong version"
    print(f"  comfyui_node     → type={node['type']} v={node['version']}")

    # JSON round-trip via _build_frame_from_json
    sample_json = json.dumps({
        "frame": 99, "at": 1234567890, "age": 50, "stale": False,
        "audit": {"health": "DEGRADED", "scans": 10,
                  "anomalies": 2, "scan_ms": 8},
        "hydra": {"active": 1, "spawn": 3, "retract": 1, "incident": 2},
        "heads": [{"id": 0, "status": "SAFE", "branch": 123,
                   "zone_mb": 4, "writes": 100, "anom": 1}],
        "tiles": [{"i": 0, "s": "ANOMALY", "anom": "0x01",
                   "blk": "1/64"}],
        "events":[{"type": "ANOMALY", "sev": 1,
                   "head": 0, "tile": 0, "at": 1234567890}],
    })
    parsed = adapter._build_frame_from_json(sample_json)
    assert parsed is not None, "JSON parse should succeed"
    assert parsed.audit_health == "DEGRADED", "health should be DEGRADED"
    assert parsed.tiles[0].state == "ANOMALY", "tile state should be ANOMALY"
    print(f"  JSON round-trip  → frame#{parsed.frame_seq} "
          f"health={parsed.audit_health}")

    adapter.stop()
    print("\n✅ All V3.1 tests passed.")
    print("═" * 60)
