"""
╔══════════════════════════════════════════════════════════════════════╗
║     POGLS V3.1 — ComfyUI Integration Layer                          ║
║                                                                      ║
║  ไฟล์นี้คือสะพานระหว่าง POGLS V3.1 กับ ComfyUI                     ║
║                                                                      ║
║  V3.1: Visualize ผ่าน VisualFeed เท่านั้น — ไม่แตะ Core ตรง        ║
║                                                                      ║
║  สิ่งที่ทำ:                                                          ║
║    1. map_model()       — map AI model file เข้า vault              ║
║    2. slice_file()      — หั่น large file เป็น chunks พร้อม address ║
║    3. map_image_meta()  — map metadata ของรูปภาพเข้า angular space  ║
║    4. comfyui_node()    — output format ที่ ComfyUI รับได้เลย       ║
║    5. visual_status()   — Hydra/Audit status ผ่าน VisualFeed        ║
║    6. subscribe_feed()  — real-time frame feed สำหรับ ComfyUI node  ║
║                                                                      ║
║  ใช้งาน:                                                             ║
║    from comfyui_bridge import POGLSBridge                            ║
║    bridge = POGLSBridge()                                            ║
║    result = bridge.map_model("flux_dev.safetensors")                ║
║    status = bridge.visual_status()   # read-only, ไม่แตะ Core       ║
╚══════════════════════════════════════════════════════════════════════╝
"""

import math
import os
import json
import hashlib
import struct
import time
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import List, Optional, Dict, Any

# Import core controller
import sys
sys.path.insert(0, str(Path(__file__).parent))
from pogls_controller import (
    POGLS, AngularMapper, AngularAddress, GeoPoint,
    ANGULAR_FULL_CIRCLE, TOPO_VERTEX_TABLE, TOPO_BITS_TABLE,
    Mode, TopoLevel,
    # V3.1
    V31Adapter, VisualFrame, VF_HeadSnap, VF_TileSnap, VF_Event,
    SnapshotState, HeadStatus, AuditHealth, VFEventType,
    POGLS_VERSION_V31,
)

CHUNK_SIZE_DEFAULT = 100 * 1024 * 1024  # 100MB


# ═══════════════════════════════════════════════════════════════════════
# DATA CLASSES — ComfyUI-friendly output format
# ═══════════════════════════════════════════════════════════════════════

@dataclass
class ChunkInfo:
    """ข้อมูลของ 1 chunk จากการ slice file"""
    index:           int
    byte_offset:     int
    byte_size:       int
    size_mb:         float
    angular_address: int
    theta_rad:       float
    theta_deg:       float
    is_last:         bool
    sha256:          str    # hex string

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class ModelManifest:
    """
    Manifest ของ AI model ที่ผ่าน POGLS
    ComfyUI จะใช้ manifest นี้ในการ load/switch version
    """
    model_name:      str
    model_path:      str
    file_size_bytes: int
    file_size_gb:    float
    chunk_size_mb:   int
    total_chunks:    int
    n_bits:          int
    address_space:   int          # 2^n
    created_at:      str
    topo_level:      int
    chunks:          List[ChunkInfo] = field(default_factory=list)

    # Angular address ของ "identity" ของ model (chunk 0)
    model_address:   int = 0

    # Honeycomb slot status (reserved, always inactive now)
    honeycomb_active: bool = False
    honeycomb_stage:  str  = "pending_stage2"

    # V3.1: snapshot state ล่าสุด (จาก VisualFeed)
    latest_snap_state: str = "UNKNOWN"
    latest_head_id:    int = -1

    def to_dict(self) -> dict:
        d = asdict(self)
        return d

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent, ensure_ascii=False)

    def save(self, path: str):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(self.to_json())
        print(f"📄 Manifest saved: {path}")


@dataclass
class VisualSnapshot:
    """
    Read-only status snapshot สำหรับ ComfyUI node.
    ดึงจาก VisualFeed เท่านั้น — ไม่มี write path ไปยัง Core

    ComfyUI node ใช้ object นี้เพื่อแสดง status panel
    โดยไม่ต้องรู้จัก internal C structures เลย
    """
    # Audit summary
    audit_health:        str       # "OK" | "DEGRADED" | "OFFLINE"
    total_scans:         int
    anomalous_tile_count:int
    total_tile_count:    int

    # Hydra summary
    active_heads:        int
    total_heads:         int
    heads_in_safe:       int       # จำนวน head ที่อยู่ใน SAFE mode
    heads_migrating:     int
    spawn_count:         int
    incident_count:      int

    # Frame metadata
    frame_seq:           int
    frame_age_ms:        int
    is_stale:            bool
    has_critical_event:  bool

    # Per-head detail (เฉพาะ heads ที่ active หรือ need attention)
    head_details:        List[dict] = field(default_factory=list)

    # Recent events (สูงสุด 10)
    recent_events:       List[dict] = field(default_factory=list)

    @classmethod
    def from_visual_frame(cls, frame: VisualFrame) -> "VisualSnapshot":
        """สร้าง VisualSnapshot จาก VisualFrame — read-only conversion"""
        heads_safe      = sum(1 for h in frame.heads if h.status == "SAFE")
        heads_migrating = sum(1 for h in frame.heads if h.status == "MIGRATING")

        return cls(
            audit_health=frame.audit_health,
            total_scans=frame.total_scans,
            anomalous_tile_count=len(frame.anomalous_tiles),
            total_tile_count=len(frame.tiles),
            active_heads=frame.active_heads,
            total_heads=len(frame.heads),
            heads_in_safe=heads_safe,
            heads_migrating=heads_migrating,
            spawn_count=frame.radar_spawn_count,
            incident_count=frame.radar_incident_count,
            frame_seq=frame.frame_seq,
            frame_age_ms=frame.frame_age_ms,
            is_stale=frame.is_stale,
            has_critical_event=frame.has_critical,
            head_details=[h.to_dict() for h in frame.heads],
            recent_events=[e.to_dict() for e in frame.events[-10:]],
        )

    def to_comfyui_node(self) -> dict:
        """Format ที่ ComfyUI custom node รับได้โดยตรง"""
        health_icon = {"OK": "✅", "DEGRADED": "⚠️",
                       "OFFLINE": "❌"}.get(self.audit_health, "❓")
        alert = []
        if self.heads_in_safe:    alert.append(f"{self.heads_in_safe} head(s) in SAFE mode")
        if self.heads_migrating:  alert.append(f"{self.heads_migrating} head(s) MIGRATING")
        if self.is_stale:         alert.append("feed is STALE")
        if self.has_critical_event: alert.append("CRITICAL event detected")

        return {
            "type":    "POGLS_VISUAL_STATUS",
            "version": POGLS_VERSION_V31,
            "health":  f"{health_icon} {self.audit_health}",
            "alert":   alert,
            "audit": {
                "scans":     self.total_scans,
                "anomalies": self.anomalous_tile_count,
                "tiles":     self.total_tile_count,
            },
            "hydra": {
                "active":   self.active_heads,
                "safe":     self.heads_in_safe,
                "migrate":  self.heads_migrating,
                "spawns":   self.spawn_count,
                "incidents":self.incident_count,
            },
            "frame": {
                "seq":     self.frame_seq,
                "age_ms":  self.frame_age_ms,
                "stale":   self.is_stale,
            },
            "heads":  self.head_details,
            "events": self.recent_events,
        }


@dataclass
class ImageMeta:
    """Metadata ของรูปภาพที่ถูก map เข้า angular space"""
    filename:        str
    width:           int
    height:          int
    channels:        int           # 3=RGB, 4=RGBA
    file_size_bytes: int

    # Angular addresses (3D: width, height, channels)
    addr_width:      int = 0
    addr_height:     int = 0
    addr_channels:   int = 0
    theta_width:     float = 0.0
    theta_height:    float = 0.0

    # Warp map hint (ส่วนไหนของรูปที่ "active")
    roi_density:     float = 1.0   # 0.0-1.0 (1.0 = full image)

    def to_dict(self) -> dict:
        return asdict(self)

    def to_comfyui_node(self) -> dict:
        """Format ที่ ComfyUI custom node รับได้โดยตรง"""
        return {
            "type":    "POGLS_IMAGE_META",
            "version": POGLS_VERSION_V31,
            "file":    self.filename,
            "dims":    {"w": self.width, "h": self.height, "c": self.channels},
            "pogls": {
                "addr_w": self.addr_width,
                "addr_h": self.addr_height,
                "addr_c": self.addr_channels,
                "roi":    self.roi_density,
            }
        }


# ═══════════════════════════════════════════════════════════════════════
# LEGACY WRAPPER — simulate V2 read (Python side)
# ═══════════════════════════════════════════════════════════════════════

class LegacyWrapper:
    """
    จำลอง Smart Header immigration guard สำหรับ V2 compatibility
    V2 ระบบที่ขอข้อมูลจะได้รับแค่ coord_4byte กลับไป
    """

    @staticmethod
    def read_as_v2(geo_point: GeoPoint) -> dict:
        """
        V2 system อ่าน GeoPoint → ได้แค่ 4-byte coord
        payload ทั้งหมดถูกซ่อน
        """
        coord_4byte = geo_point.addr_x.address & 0xFFFFFFFF  # truncate to 4B
        return {
            "coord_4byte":  coord_4byte,
            "version_seen": "V3",
            "is_compatible": True,
            "hidden_payload_size": 128,   # V2 ไม่เห็นส่วนนี้
            "note": "V2 sees coord only. Payload hidden by Smart Header."
        }

    @staticmethod
    def read_as_v3(geo_point: GeoPoint) -> dict:
        """V3.1 system อ่านได้ทุกอย่าง"""
        return {
            "coord_4byte":   geo_point.addr_x.address & 0xFFFFFFFF,
            "addr_x":        geo_point.addr_x.address,
            "addr_y":        geo_point.addr_y.address,
            "addr_z":        geo_point.addr_z.address,
            "topo_level":    geo_point.topo_level,
            "vertex_count":  TOPO_VERTEX_TABLE[geo_point.topo_level],
            "version_seen":  POGLS_VERSION_V31,
            "is_compatible": True,
        }


# ═══════════════════════════════════════════════════════════════════════
# FILE CHUNKER — Large File Slicing
# ═══════════════════════════════════════════════════════════════════════

class FileChunker:
    """
    หั่นไฟล์ใหญ่เป็น chunks แต่ละ chunk ได้ Angular Address
    
    ตัวอย่าง: flux_dev.safetensors (10GB)
      → 100 chunks × 100MB
      → chunk[0].address = floor(0/100 × 2π / 2π × 2^20) = 0
      → chunk[50].address = floor(50/100 × 2π / 2π × 2^20) = 524288
      → chunk[99].address = floor(99/100 × 2π / 2π × 2^20) ≈ 1038090
    """

    def __init__(self, n_bits: int = 20, chunk_size: int = CHUNK_SIZE_DEFAULT):
        self.n_bits     = n_bits
        self.chunk_size = chunk_size
        self.mapper     = AngularMapper(n=n_bits)

    def slice(self, file_size: int, filename: str = "") -> List[ChunkInfo]:
        """
        คำนวณ chunk layout ทั้งหมดโดยไม่ต้องมีไฟล์จริง
        (ใช้สำหรับ planning และ manifest generation)
        """
        total_chunks = math.ceil(file_size / self.chunk_size)
        chunks = []

        for i in range(total_chunks):
            offset = i * self.chunk_size
            size   = min(self.chunk_size, file_size - offset)

            # θ = (i / total) × 2π → A = floor(θ/2π × 2^n) = floor(i/total × 2^n)
            theta = (i / total_chunks) * ANGULAR_FULL_CIRCLE
            addr  = self.mapper.angle_to_address(theta)

            # SHA256 placeholder (จะ fill จริงตอน read จาก disk)
            sha_placeholder = hashlib.sha256(
                f"{filename}:chunk:{i}:{offset}".encode()
            ).hexdigest()

            chunks.append(ChunkInfo(
                index=i,
                byte_offset=offset,
                byte_size=size,
                size_mb=round(size / (1024*1024), 2),
                angular_address=addr.address,
                theta_rad=round(theta, 8),
                theta_deg=round(math.degrees(theta), 4),
                is_last=(i == total_chunks - 1),
                sha256=sha_placeholder,
            ))

        return chunks

    def slice_real_file(self, filepath: str) -> List[ChunkInfo]:
        """Slice ไฟล์จริง พร้อม compute SHA256 ของแต่ละ chunk"""
        path      = Path(filepath)
        file_size = path.stat().st_size
        chunks    = self.slice(file_size, path.name)

        print(f"🔪 Slicing: {path.name} ({file_size/(1<<30):.2f} GB)")
        print(f"   Chunks: {len(chunks)} × {self.chunk_size>>20}MB")

        with open(filepath, 'rb') as f:
            for chunk in chunks:
                f.seek(chunk.byte_offset)
                data          = f.read(chunk.byte_size)
                chunk.sha256  = hashlib.sha256(data).hexdigest()
                if chunk.index % 100 == 0 or chunk.is_last:
                    print(f"   [{chunk.index:4d}] A={chunk.angular_address:>10}  "
                          f"sha={chunk.sha256[:8]}...")

        return chunks

    def print_map(self, file_size: int, label: str = ""):
        """แสดง chunk map (visual) — ไม่ต้องมีไฟล์จริง"""
        total = math.ceil(file_size / self.chunk_size)
        gb    = file_size / (1 << 30)
        mb    = self.chunk_size >> 20

        print(f"┌── Chunk Map{': ' + label if label else ''} {'─'*30}┐")
        print(f"│  Size:    {gb:.2f} GB  →  {total} chunks × {mb}MB")
        print(f"│  n_bits:  {self.n_bits}  (2^n = {1<<self.n_bits:,} addresses)")
        print(f"├{'─'*52}┤")

        chunks  = self.slice(file_size, label)
        preview = chunks[:3] + (chunks[-1:] if total > 4 else [])
        for c in preview:
            bar = "█" * int(c.size_mb / mb * 10)
            print(f"│  [{c.index:4d}]  θ={c.theta_deg:8.3f}°  "
                  f"A={c.angular_address:>10}  {bar}")
            if c.index == 2 and total > 4:
                print(f"│   ...  ({total-4} chunks)")

        print(f"└{'─'*52}┘")


# ═══════════════════════════════════════════════════════════════════════
# IMAGE METADATA MAPPER — รูปภาพ / วิดีโอ
# ═══════════════════════════════════════════════════════════════════════

class ImageMapper:
    """
    Map metadata ของรูปภาพเข้า Angular Address Space
    
    3 axis:
      X → ความกว้าง (width)  : normalized ใน [0, max_dim)
      Y → ความสูง  (height) : normalized ใน [0, max_dim)
      Z → channels           : 1,2,3,4 → normalized
    
    ใช้สำหรับ: จัด index รูปที่ ComfyUI generate ออกมา
    เช่น รูป 1024×768 RGB จะได้ address ที่ unique ใน angular space
    """

    MAX_DIM      = 16384    # รองรับสูงสุด 16K resolution
    MAX_CHANNELS = 4

    def __init__(self, n_bits: int = 20):
        self.mapper = AngularMapper(n=n_bits)

    def map(self, filename: str, width: int, height: int,
            channels: int = 3, file_size: int = 0) -> ImageMeta:
        """Map รูปภาพเข้า angular space"""

        # normalize แต่ละ axis ไปยัง [0, 2π)
        theta_w  = (width    / self.MAX_DIM)      * ANGULAR_FULL_CIRCLE
        theta_h  = (height   / self.MAX_DIM)      * ANGULAR_FULL_CIRCLE
        theta_c  = (channels / self.MAX_CHANNELS) * ANGULAR_FULL_CIRCLE

        addr_w = self.mapper.angle_to_address(theta_w)
        addr_h = self.mapper.angle_to_address(theta_h)
        addr_c = self.mapper.angle_to_address(theta_c)

        # ROI density: estimate จาก resolution (สูงขึ้น = dense ขึ้น)
        roi = min(1.0, (width * height) / (1024 * 1024))  # normalize ต่อ 1MP

        return ImageMeta(
            filename=filename,
            width=width,
            height=height,
            channels=channels,
            file_size_bytes=file_size,
            addr_width=addr_w.address,
            addr_height=addr_h.address,
            addr_channels=addr_c.address,
            theta_width=round(theta_w, 6),
            theta_height=round(theta_h, 6),
            roi_density=round(roi, 4),
        )

    def map_from_file(self, filepath: str) -> ImageMeta:
        """
        Map รูปภาพจากไฟล์จริง
        รองรับ PNG, JPEG, WebP โดยดู header bytes (ไม่ต้อง install PIL)
        """
        path = Path(filepath)
        if not path.exists():
            raise FileNotFoundError(f"File not found: {filepath}")

        width, height, channels = self._read_image_dims(filepath)
        file_size = path.stat().st_size

        return self.map(path.name, width, height, channels, file_size)

    @staticmethod
    def _read_image_dims(filepath: str):
        """อ่าน width/height/channels จาก file header (no PIL needed)"""
        with open(filepath, 'rb') as f:
            header = f.read(24)

        # PNG: signature \x89PNG + IHDR chunk
        if header[:8] == b'\x89PNG\r\n\x1a\n':
            w = struct.unpack('>I', header[16:20])[0]
            h = struct.unpack('>I', header[20:24])[0]
            color_type = header[25] if len(header) > 25 else 2
            channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type, 3)
            return w, h, channels

        # JPEG: FF D8 FF
        if header[:3] == b'\xff\xd8\xff':
            # Scan for SOF marker
            try:
                with open(filepath, 'rb') as f:
                    data = f.read(65536)  # อ่านแค่ 64KB
                i = 2
                while i < len(data) - 8:
                    if data[i] == 0xFF and data[i+1] in (0xC0, 0xC2):
                        h = struct.unpack('>H', data[i+5:i+7])[0]
                        w = struct.unpack('>H', data[i+7:i+9])[0]
                        c = data[i+9]
                        return w, h, c
                    length = struct.unpack('>H', data[i+2:i+4])[0]
                    i += 2 + length
            except Exception:
                pass
            return 512, 512, 3  # fallback

        # WebP: RIFF????WEBP
        if header[:4] == b'RIFF' and header[8:12] == b'WEBP':
            return 512, 512, 3  # simplified

        # Unknown — return default
        return 512, 512, 3


# ═══════════════════════════════════════════════════════════════════════
# MAIN BRIDGE — ComfyUI Entry Point
# ═══════════════════════════════════════════════════════════════════════

class POGLSBridge:
    """
    Main interface สำหรับ ComfyUI integration (V3.1)

    V3.1: ทุก status/visual ดึงผ่าน VisualFeed — ไม่แตะ Core ตรง

    ใช้งาน:
        bridge = POGLSBridge(vault_path="models.pogls")

        # Map model file (ขนาดใดก็ได้)
        manifest = bridge.map_model("flux_dev.safetensors")

        # Map รูปที่ ComfyUI generate
        img_meta = bridge.map_image("output_001.png")

        # ดู visual status (read-only, ไม่แตะ Core)
        status = bridge.visual_status()
        print(status.audit_health)   # "OK" | "DEGRADED" | "OFFLINE"

        # Subscribe real-time feed
        bridge.subscribe_feed(lambda frame: print(frame.summary()))
    """

    def __init__(self, vault_path: str = "pogls_models.vault",
                 n_bits: int = 20,
                 chunk_size_mb: int = 100,
                 visual_feed_path: Optional[str] = None):
        self.vault_path    = vault_path
        self.n_bits        = n_bits
        self.chunk_size    = chunk_size_mb * 1024 * 1024
        self.chunker       = FileChunker(n_bits=n_bits, chunk_size=self.chunk_size)
        self.img_mapper    = ImageMapper(n_bits=n_bits)
        self.legacy        = LegacyWrapper()
        self._manifests: Dict[str, ModelManifest] = {}

        # V3.1: VisualFeed adapter — read-only observer
        # visual_feed_path=None → simulation mode (สำหรับ dev/testing)
        # visual_feed_path=str  → pipe จาก C VisualFeed process
        self._adapter = V31Adapter(
            feed_path=visual_feed_path,
            poll_interval_ms=200,
        )

    def map_model(self, filepath: str,
                  real_file: bool = False) -> ModelManifest:
        """
        Map AI model file เข้า POGLS
        
        real_file=False → ใช้ file size เท่านั้น (เร็ว, ไม่อ่านทั้งไฟล์)
        real_file=True  → อ่านจริง + compute SHA256 ต่อ chunk (ช้ากว่า)
        """
        path = Path(filepath)

        if not path.exists():
            # Simulation mode — ใช้ชื่อไฟล์เพื่อประมาณขนาด
            file_size = self._estimate_size(path.name)
            print(f"⚠️  File not found — simulation mode: {path.name} "
                  f"({file_size/(1<<30):.1f} GB estimated)")
        else:
            file_size = path.stat().st_size

        # Slice file
        if real_file and path.exists():
            chunks = self.chunker.slice_real_file(str(path))
        else:
            chunks = self.chunker.slice(file_size, path.name)

        manifest = ModelManifest(
            model_name=path.stem,
            model_path=str(path),
            file_size_bytes=file_size,
            file_size_gb=round(file_size / (1<<30), 3),
            chunk_size_mb=self.chunk_size >> 20,
            total_chunks=len(chunks),
            n_bits=self.n_bits,
            address_space=1 << self.n_bits,
            created_at=time.strftime("%Y-%m-%dT%H:%M:%S"),
            topo_level=int(TopoLevel.STANDARD),
            chunks=chunks,
            model_address=chunks[0].angular_address if chunks else 0,
            honeycomb_active=False,
            honeycomb_stage="pending_stage2",
        )

        self._manifests[path.name] = manifest

        print(f"✅ Model mapped: {path.name}")
        print(f"   Size: {manifest.file_size_gb} GB")
        print(f"   Chunks: {manifest.total_chunks} × {manifest.chunk_size_mb}MB")
        print(f"   Model address: A={manifest.model_address:,}")
        print(f"   Honeycomb: {manifest.honeycomb_stage} (inactive)")

        return manifest

    def map_image(self, filepath: str,
                  width: int = 0, height: int = 0,
                  channels: int = 3) -> ImageMeta:
        """
        Map รูปภาพเข้า angular space
        
        ถ้ามีไฟล์จริง → อ่าน dims อัตโนมัติ
        ถ้าไม่มีไฟล์ → ใช้ width/height ที่ระบุ
        """
        path = Path(filepath)
        if path.exists():
            try:
                meta = self.img_mapper.map_from_file(str(path))
            except Exception as e:
                print(f"⚠️  Could not read image dims: {e}")
                meta = self.img_mapper.map(
                    path.name, width or 512, height or 512, channels
                )
        else:
            meta = self.img_mapper.map(
                path.name, width or 512, height or 512, channels
            )

        print(f"🖼️  Image mapped: {meta.filename}")
        print(f"   Dims: {meta.width}×{meta.height} ch={meta.channels}")
        print(f"   A(w)={meta.addr_width:,}  A(h)={meta.addr_height:,}")
        print(f"   ROI density: {meta.roi_density:.2%}")

        return meta

    def read_legacy(self, geo_point: GeoPoint, version: str = "v2") -> dict:
        """อ่านข้อมูลผ่าน Legacy Wrapper"""
        if version == "v2":
            result = self.legacy.read_as_v2(geo_point)
        else:
            result = self.legacy.read_as_v3(geo_point)
        return result

    def show_chunk_map(self, file_size_gb: float, label: str = ""):
        """แสดง chunk map แบบ visual"""
        self.chunker.print_map(int(file_size_gb * (1<<30)), label)

    def export_manifest(self, model_name: str, output_path: str = ""):
        """Export manifest เป็น JSON สำหรับ ComfyUI"""
        manifest = self._manifests.get(model_name)
        if not manifest:
            print(f"❌ Manifest not found: {model_name}")
            return None
        if not output_path:
            output_path = model_name.replace(".safetensors", "") + "_manifest.json"
        manifest.save(output_path)
        return output_path

    # ── V3.1: VisualFeed methods ─────────────────────────────────────

    def visual_status(self) -> VisualSnapshot:
        """
        Get current system status — read-only, ไม่แตะ Core ตรง
        Source: VisualFeed → Audit tiles + Hydra heads + signal events

        Returns VisualSnapshot ที่ ComfyUI node ใช้แสดง status panel ได้เลย
        """
        frame = self._adapter.poll_frame()
        return VisualSnapshot.from_visual_frame(frame)

    def poll_visual_frame(self) -> VisualFrame:
        """
        Get raw VisualFrame — สำหรับ advanced usage ที่ต้องการ tile-level detail
        GUI ใช้ method นี้ตรง
        """
        return self._adapter.poll_frame()

    def subscribe_feed(self, callback):
        """
        Subscribe real-time VisualFrame feed.
        callback(frame: VisualFrame) จะถูกเรียกทุก poll_interval_ms

        ใช้สำหรับ:
        - ComfyUI node ที่ต้องการ live status
        - GUI panel ที่ auto-refresh
        """
        self._adapter.subscribe(callback)

    def inject_test_anomaly(self, head_id: int = 0, critical: bool = False):
        """
        Inject test anomaly เพื่อ test GUI/ComfyUI response.
        ใช้ใน development เท่านั้น — ไม่มีผลต่อ Core จริง
        """
        self._adapter.simulate_anomaly(head_id=head_id, critical=critical)
        severity = "CRITICAL" if critical else "SAFE_MODE"
        print(f"⚠️  Test anomaly injected: head={head_id} severity={severity}")

    def inject_test_spawn(self, zone_start_mb: int = 20, zone_end_mb: int = 22):
        """Inject simulated head spawn — สำหรับ GUI testing"""
        self._adapter.simulate_spawn(zone_start_mb, zone_end_mb)
        print(f"🐉 Test spawn injected: zone={zone_start_mb}-{zone_end_mb}MB")

    def stop(self):
        """Stop background VisualFeed polling thread"""
        self._adapter.stop()

    @staticmethod
    def _estimate_size(filename: str) -> int:
        """ประมาณขนาดไฟล์จากชื่อ (สำหรับ simulation)"""
        name = filename.lower()
        if "flux" in name:        return 10 * (1<<30)
        if "sdxl" in name:        return 6  * (1<<30)
        if "sd15" in name:        return 2  * (1<<30)
        if "deepseek" in name:    return 600* (1<<30)
        if "llama" in name:       return 40 * (1<<30)
        return 4 * (1<<30)


# ═══════════════════════════════════════════════════════════════════════
# SAMPLE INTEGRATION — ตัวอย่างการใช้งาน
# ═══════════════════════════════════════════════════════════════════════

def run_sample_integration():
    """
    ตัวอย่าง Integration V3.1 ครบวงจร:
    1. Map AI models (simulation)
    2. Visual status ผ่าน VisualFeed — ไม่แตะ Core ตรง
    3. Simulate anomaly → ดู status เปลี่ยน
    4. Subscribe real-time feed
    5. Legacy wrapper + Honeycomb slot
    6. ComfyUI node output
    """
    print("═" * 62)
    print("  POGLS V3.1 × ComfyUI — Integration Demo")
    print("═" * 62)

    bridge = POGLSBridge(n_bits=20, chunk_size_mb=100)

    # ─── 1. Map AI Models ────────────────────────────────────────────
    print("\n▶ [1] Mapping AI Models (simulation mode)\n")
    models = [
        "flux_dev_fp8.safetensors",
        "sdxl_base_1.0.safetensors",
        "deepseek_v3.safetensors",
    ]
    manifests = {}
    for model in models:
        m = bridge.map_model(model)
        manifests[model] = m
        print()

    # ─── 2. Visual Status (read-only, VisualFeed) ─────────────────────
    print("\n▶ [2] Visual Status — ผ่าน VisualFeed (ไม่แตะ Core ตรง)\n")
    status = bridge.visual_status()
    node   = status.to_comfyui_node()

    print(f"  Audit health  : {node['health']}")
    print(f"  Active heads  : {status.active_heads}/{status.total_heads}")
    print(f"  Tiles         : {status.anomalous_tile_count} anomalous / {status.total_tile_count}")
    print(f"  Frame seq     : #{status.frame_seq}  age={status.frame_age_ms}ms")
    print(f"  Alert(s)      : {node['alert'] or ['none']}")

    # ─── 3. Simulate anomaly + watch status change ────────────────────
    print("\n▶ [3] Simulate Anomaly → Watch Status Change\n")

    status_before = bridge.visual_status()
    print(f"  Before: health={status_before.audit_health} "
          f"safe={status_before.heads_in_safe} migrate={status_before.heads_migrating}")

    bridge.inject_test_anomaly(head_id=0, critical=False)   # SAFE mode
    bridge.inject_test_anomaly(head_id=1, critical=True)    # MIGRATING
    bridge.inject_test_spawn(zone_start_mb=20, zone_end_mb=22)

    status_after = bridge.visual_status()
    print(f"  After:  health={status_after.audit_health} "
          f"safe={status_after.heads_in_safe} migrate={status_after.heads_migrating}")
    print(f"  Events in frame: {len(status_after.recent_events)}")
    for ev in status_after.recent_events:
        icon = "❌" if ev["severity"] == "CRITICAL" else \
               "⚠️" if ev["severity"] == "WARN"     else "ℹ️"
        print(f"    {icon} {ev['type']}  sev={ev['severity']}")

    # ─── 4. Subscribe feed ────────────────────────────────────────────
    print("\n▶ [4] Subscribe Real-Time Feed\n")
    received = []

    def on_frame(frame: VisualFrame):
        received.append(frame.frame_seq)

    bridge.subscribe_feed(on_frame)
    import time; time.sleep(0.35)   # รอ 1-2 frames
    bridge.stop()
    print(f"  Received {len(received)} frame(s) via callback: seq={received}")

    # ─── 5. Chunk Map + Image Mapping ─────────────────────────────────
    print("\n▶ [5] Chunk Map + Image Metadata\n")
    bridge.show_chunk_map(10.0, "flux_dev_fp8")
    print()
    img_mapper = ImageMapper(n_bits=20)
    for fname, w, h, c in [("output_001.png", 1024, 1024, 3),
                            ("portrait.png",    512,  768, 4)]:
        meta = img_mapper.map(fname, w, h, c)
        print(f"  {fname}: {w}×{h}ch{c}  A_w={meta.addr_width:>8,} "
              f"A_h={meta.addr_height:>8,}  ROI={meta.roi_density:.1%}")

    # ─── 6. Legacy Wrapper ────────────────────────────────────────────
    print("\n▶ [6] Legacy Wrapper (V2 Compatibility)\n")
    mapper  = AngularMapper(n=20)
    sample  = mapper.map_xyz(0.5, 0.3, 0.7)
    v2_view = bridge.read_legacy(sample, "v2")
    v3_view = bridge.read_legacy(sample, "v3")
    print(f"  V2 sees: coord={v2_view['coord_4byte']}  "
          f"(payload hidden: {v2_view['hidden_payload_size']}B)")
    print(f"  V3 sees: X={v3_view['addr_x']}  Y={v3_view['addr_y']}  "
          f"Z={v3_view['addr_z']}  ver={v3_view['version_seen']}")

    # ─── 7. ComfyUI Node Output ───────────────────────────────────────
    print("\n▶ [7] ComfyUI Node Output\n")
    frame     = bridge.poll_visual_frame()
    node_full = frame.to_comfyui_node()
    print("  VisualFrame node (truncated):")
    summary = {
        "type":    node_full["type"],
        "version": node_full["version"],
        "health":  node_full["health"],
        "hydra":   node_full["hydra"],
        "has_critical": node_full["has_critical"],
    }
    print("  " + json.dumps(summary, indent=4).replace("\n", "\n  "))

    print("\n" + "═" * 62)
    print("  ✅ POGLS V3.1 × ComfyUI Integration complete.")
    print("  📌 Visualize ผ่าน VisualFeed เท่านั้น — Core ไม่ถูกแตะ")
    print("═" * 62)


if __name__ == "__main__":
    run_sample_integration()
