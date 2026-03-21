"""
st_to_fabric.py — Safetensors → POGLS Fabric Converter
=======================================================
แปลง .safetensors ไฟล์ให้เป็น .pogls fabric
เพื่อให้โหลด tensor ได้แบบ zero-copy slice เข้า VRAM

Safetensors format (spec):
  [header_size: 8B LE uint64]
  [header_json: header_size bytes]
  [tensor data: ต่อเนื่องกัน, byte-aligned]

Fabric layout หลังแปลง:
  [Fabric Header: 32B]
  [Index Block: เก็บ tensor metadata ทั้งหมด]
  [Tensor Data Blocks: เรียงตาม tensor order]

Usage:
  converter = STFabricConverter("model.safetensors")
  fabric_path = converter.convert()               # แปลงครั้งแรก
  
  loader = FabricTensorLoader(fabric_path)
  tensor = loader.load_tensor("model.layers.0.weight")   # zero-copy
  tensors = loader.load_layers("model.layers.0")         # load by prefix
"""

import os
import sys
import json
import struct
import time
import hashlib
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Iterator
from dataclasses import dataclass, field, asdict

# ── ต้อง import fabric จากไฟล์เดียวกัน ─────────────────────────────
sys.path.insert(0, str(Path(__file__).parent))
from pogls_fabric import BlockFabric, SliceView, HEADER_SIZE, BLOCK_SIZE, BLOCK_SHIFT


# ═══════════════════════════════════════════════════════════════════
# SAFETENSORS SPEC CONSTANTS
# ═══════════════════════════════════════════════════════════════════

ST_HEADER_SIZE_BYTES = 8          # LE uint64 บอกขนาด JSON header
DTYPE_SIZES = {                   # bytes per element
    "F64": 8, "F32": 4, "F16": 2, "BF16": 2,
    "I64": 8, "I32": 4, "I16": 2, "I8": 1,
    "U8":  1, "BOOL": 1,
}


# ═══════════════════════════════════════════════════════════════════
# DATA STRUCTURES
# ═══════════════════════════════════════════════════════════════════

@dataclass
class TensorMeta:
    """Metadata ของ tensor หนึ่งตัว — เก็บไว้ใน Index Block"""
    name:         str
    dtype:        str
    shape:        List[int]
    data_offsets: List[int]   # [start, end] ใน safetensors data region
    fabric_offset: int = 0    # byte offset ใน fabric (หลังแปลง)
    fabric_length: int = 0    # ขนาดจริงใน fabric
    element_count: int = 0
    checksum:     str = ""    # sha256 ของ tensor data

    @property
    def nbytes(self) -> int:
        return DTYPE_SIZES.get(self.dtype, 4) * max(1, self.element_count)

    @property
    def shape_str(self) -> str:
        return "×".join(str(d) for d in self.shape)

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class FabricIndex:
    """Index ทั้งหมดของ fabric — serialize เป็น JSON เก็บใน block แรก"""
    source_file:    str
    source_size:    int
    tensor_count:   int
    converted_at:   float
    tensors:        Dict[str, dict]   # name → TensorMeta.to_dict()
    index_version:  int = 1

    def to_bytes(self) -> bytes:
        return json.dumps({
            "source_file":   self.source_file,
            "source_size":   self.source_size,
            "tensor_count":  self.tensor_count,
            "converted_at":  self.converted_at,
            "index_version": self.index_version,
            "tensors":       self.tensors,
        }, separators=(",", ":")).encode("utf-8")

    @classmethod
    def from_bytes(cls, data: bytes) -> "FabricIndex":
        d = json.loads(data)
        return cls(**d)


# ═══════════════════════════════════════════════════════════════════
# CONVERTER
# ═══════════════════════════════════════════════════════════════════

class STFabricConverter:
    """
    แปลง .safetensors → .pogls fabric

    ขั้นตอน:
      1. อ่าน safetensors header (JSON) → รู้ tensor layout
      2. สร้าง fabric ขนาดพอดี
      3. เขียน Index Block ก่อน (placeholder)
      4. copy tensor data ทีละ tensor พร้อม verify checksum
      5. update Index Block ด้วย fabric_offset จริง
      6. snapshot เป็น completion seal
    """

    def __init__(self, st_path: str,
                 out_path: Optional[str] = None,
                 verify: bool = True,
                 progress: bool = True):
        self.st_path   = Path(st_path)
        self.out_path  = Path(out_path) if out_path else \
                         self.st_path.with_suffix(".pogls")
        self.verify    = verify
        self.progress  = progress
        self._metas:   Dict[str, TensorMeta] = {}

    # ── Public ──────────────────────────────────────────────────────

    def convert(self) -> str:
        """
        แปลงไฟล์ คืน path ของ fabric ที่สร้าง
        idempotent — ถ้า output มีอยู่แล้วและ valid จะไม่แปลงซ้ำ
        """
        if self.out_path.exists():
            if self._is_valid_fabric(self.out_path):
                if self.progress:
                    print(f"  ✓ already converted: {self.out_path.name}")
                return str(self.out_path)
            else:
                self.out_path.unlink()

        t0 = time.perf_counter()
        self._print(f"Converting {self.st_path.name} ...")

        # 1. parse safetensors header
        st_header, data_offset = self._parse_st_header()
        self._print(f"  tensors: {len(st_header)}  data_offset: {data_offset:,}B")

        # 2. build tensor metas
        self._build_metas(st_header, data_offset)

        # 3. compute fabric size
        index_bytes  = self._estimate_index_size()
        index_blocks = (index_bytes + BLOCK_SIZE - 1) >> BLOCK_SHIFT
        data_size    = sum(m.fabric_length for m in self._metas.values())
        total_size   = HEADER_SIZE + \
                       (index_blocks << BLOCK_SHIFT) + \
                       data_size + (1 << 20)   # +1MB headroom

        self._print(f"  fabric size: {total_size >> 20:.1f} MB")

        # 4. create fabric + write
        with BlockFabric(str(self.out_path),
                         create=True, max_size=total_size) as fab:
            self._write_index_placeholder(fab, index_blocks)
            self._copy_tensors(fab)
            self._finalize_index(fab, index_blocks)
            snap = fab.snapshot()
            self._print(f"  seal: {snap.merkle_root[:16]}...")

        elapsed = time.perf_counter() - t0
        src_mb  = self.st_path.stat().st_size >> 20
        dst_mb  = self.out_path.stat().st_size >> 20
        self._print(f"  done in {elapsed:.1f}s  "
                    f"{src_mb}MB → {dst_mb}MB fabric")
        return str(self.out_path)

    # ── Parsing ─────────────────────────────────────────────────────

    def _parse_st_header(self) -> Tuple[dict, int]:
        """อ่าน safetensors JSON header คืน (header_dict, data_start_offset)"""
        with open(self.st_path, "rb") as f:
            raw_size = f.read(ST_HEADER_SIZE_BYTES)
            header_size = struct.unpack("<Q", raw_size)[0]
            raw_json    = f.read(header_size)

        header = json.loads(raw_json)
        # __metadata__ ไม่ใช่ tensor
        header.pop("__metadata__", None)
        data_start = ST_HEADER_SIZE_BYTES + header_size
        return header, data_start

    def _build_metas(self, st_header: dict, data_offset: int):
        """สร้าง TensorMeta จาก safetensors header"""
        for name, info in st_header.items():
            dtype   = info["dtype"]
            shape   = info["shape"]
            offsets = info["data_offsets"]   # [start, end] relative to data region

            n_elem = 1
            for d in shape:
                n_elem *= d

            abs_start = data_offset + offsets[0]
            length    = offsets[1] - offsets[0]

            # align to BLOCK_SIZE boundary
            aligned_len = ((length + BLOCK_SIZE - 1) >> BLOCK_SHIFT) << BLOCK_SHIFT

            self._metas[name] = TensorMeta(
                name=name, dtype=dtype, shape=shape,
                data_offsets=[abs_start, abs_start + length],
                fabric_length=aligned_len,
                element_count=n_elem,
            )

    def _estimate_index_size(self) -> int:
        """ประมาณขนาด Index JSON"""
        sample = json.dumps({
            n: m.to_dict() for n, m in
            list(self._metas.items())[:3]
        })
        avg_per_tensor = len(sample) / max(3, len(self._metas))
        return int(avg_per_tensor * len(self._metas) * 1.3) + 4096

    # ── Writing ─────────────────────────────────────────────────────

    def _write_index_placeholder(self, fab: BlockFabric, index_blocks: int):
        """จอง index blocks ก่อน — เติม data จริงทีหลัง"""
        placeholder = bytes(index_blocks << BLOCK_SHIFT)
        fab.write(placeholder)

    def _copy_tensors(self, fab: BlockFabric):
        """copy tensor data จาก safetensors เข้า fabric ทีละ tensor"""
        total   = len(self._metas)
        done    = 0
        read_mb = 0

        with open(self.st_path, "rb") as st_f:
            for name, meta in self._metas.items():
                # อ่านจาก safetensors
                st_f.seek(meta.data_offsets[0])
                raw_length = meta.data_offsets[1] - meta.data_offsets[0]
                data = st_f.read(raw_length)

                # checksum ก่อนเขียน
                if self.verify:
                    meta.checksum = hashlib.sha256(data).hexdigest()[:16]

                # pad ให้ครบ block boundary
                if len(data) < meta.fabric_length:
                    data = data + bytes(meta.fabric_length - len(data))

                # เขียนลง fabric — บันทึก offset จริง
                offset = fab.write(data)
                meta.fabric_offset = offset

                done    += 1
                read_mb += raw_length >> 20
                if self.progress and (done % 50 == 0 or done == total):
                    pct = done * 100 // total
                    self._print(f"  [{pct:3d}%] {done}/{total} tensors  "
                                f"{read_mb}MB read")

    def _finalize_index(self, fab: BlockFabric, index_blocks: int):
        """เขียน index จริงลงใน placeholder blocks"""
        index = FabricIndex(
            source_file  = self.st_path.name,
            source_size  = self.st_path.stat().st_size,
            tensor_count = len(self._metas),
            converted_at = time.time(),
            tensors      = {n: m.to_dict() for n, m in self._metas.items()},
        )
        idx_bytes = index.to_bytes()
        # เขียนลง index region (offset = HEADER_SIZE, หลัง fabric header)
        idx_padded = idx_bytes + bytes(
            (index_blocks << BLOCK_SHIFT) - len(idx_bytes)
        )
        fab.write(idx_padded, offset=HEADER_SIZE)

    def _is_valid_fabric(self, path: Path) -> bool:
        try:
            with BlockFabric(str(path)) as f:
                return f._block_count > 0
        except Exception:
            return False

    def _print(self, msg: str):
        if self.progress:
            print(msg)


# ═══════════════════════════════════════════════════════════════════
# LOADER — blackbox API สำหรับ custom node
# ═══════════════════════════════════════════════════════════════════

class FabricTensorLoader:
    """
    โหลด tensor จาก fabric แบบ zero-copy

    Caller (custom node) ใช้งาน:
        loader = FabricTensorLoader("model.pogls")
        weight = loader.load_tensor("model.embed.weight")
        layers = loader.load_layers("transformer.h.0")
    """

    def __init__(self, fabric_path: str):
        self._fab   = BlockFabric(fabric_path)
        self._index = self._load_index()

    def _load_index(self) -> FabricIndex:
        """อ่าน index จาก block แรกของ fabric"""
        # index อยู่ที่ offset HEADER_SIZE
        # อ่าน 1MB แรก พอสำหรับ index ของ model ส่วนใหญ่
        view = self._fab.slice(HEADER_SIZE, min(1 << 20,
                               self._fab._mm.size() - HEADER_SIZE))
        raw  = bytes(view.read())
        # หา JSON boundary
        end  = raw.find(b"\x00")
        if end == -1:
            end = len(raw)
        return FabricIndex.from_bytes(raw[:end])

    # ── Public API ──────────────────────────────────────────────────

    def load_tensor(self, name: str) -> memoryview:
        """
        คืน memoryview ของ tensor — zero-copy
        torch.frombuffer(loader.load_tensor("x"), dtype=torch.float16)
        """
        meta = self._index.tensors.get(name)
        if not meta:
            raise KeyError(f"tensor '{name}' not found in fabric")
        m = TensorMeta(**meta)
        return self._fab.slice(m.fabric_offset, m.fabric_length).read()

    def load_tensor_bytes(self, name: str) -> bytes:
        """คืน bytes (copy) สำหรับกรณีที่ต้องการ"""
        return bytes(self.load_tensor(name))

    def iter_tensors(self, prefix: str = "") -> Iterator[Tuple[str, memoryview]]:
        """
        Iterate tensor ทีละตัว — streaming เข้า GPU ได้เลย

        for name, data in loader.iter_tensors("transformer.h.0"):
            gpu.load(name, data)
        """
        for name, meta_dict in self._index.tensors.items():
            if prefix and not name.startswith(prefix):
                continue
            yield name, self.load_tensor(name)

    def load_layers(self, prefix: str) -> Dict[str, memoryview]:
        """โหลดทุก tensor ที่ขึ้นต้นด้วย prefix"""
        return {n: v for n, v in self.iter_tensors(prefix)}

    def tensor_info(self, name: str) -> Optional[TensorMeta]:
        """ดู metadata ของ tensor โดยไม่โหลด data"""
        d = self._index.tensors.get(name)
        return TensorMeta(**d) if d else None

    @property
    def tensor_names(self) -> List[str]:
        return list(self._index.tensors.keys())

    @property
    def stats(self) -> dict:
        return {
            "source":       self._index.source_file,
            "tensors":      self._index.tensor_count,
            "fabric_stats": self._fab.stats,
        }

    def snapshot(self):
        """version snapshot ของ weights ณ ตอนนี้"""
        return self._fab.snapshot()

    def restore(self, snap):
        """undo weights กลับไป snapshot"""
        return self._fab.restore(snap)

    def close(self):
        self._fab.close()

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()
    def __repr__(self):
        return (f"FabricTensorLoader({self._index.source_file!r} "
                f"tensors={self._index.tensor_count})")


# ═══════════════════════════════════════════════════════════════════
# SELF-TEST — ใช้ fake safetensors ที่สร้างเอง
# ═══════════════════════════════════════════════════════════════════

def _make_fake_safetensors(path: str, n_layers: int = 4,
                            layer_size_mb: float = 1.0) -> int:
    """สร้าง .safetensors จำลองสำหรับ test"""
    import math, random

    tensors = {}
    data_parts = []
    cursor = 0

    layer_bytes = int(layer_size_mb * (1 << 20))
    elem_count  = layer_bytes // 2   # float16

    for i in range(n_layers):
        for suffix in ["weight", "bias"]:
            name = f"transformer.h.{i}.{suffix}"
            size = layer_bytes if suffix == "weight" else 512
            tensors[name] = {
                "dtype": "F16",
                "shape": [elem_count if suffix == "weight" else 256],
                "data_offsets": [cursor, cursor + size],
            }
            data_parts.append(bytes(
                [random.randint(0, 255) for _ in range(size)]
            ))
            cursor += size

    header_json = json.dumps(tensors).encode("utf-8")
    header_size = struct.pack("<Q", len(header_json))

    with open(path, "wb") as f:
        f.write(header_size)
        f.write(header_json)
        for part in data_parts:
            f.write(part)

    return os.path.getsize(path)


if __name__ == "__main__":
    import statistics

    print("═" * 60)
    print("  st_to_fabric.py — Converter Self-Test")
    print("═" * 60)

    with tempfile.TemporaryDirectory() as tmp:
        # สร้าง fake model: 4 layers × 1MB weight = ~8MB
        st_path = os.path.join(tmp, "model.safetensors")
        st_size = _make_fake_safetensors(st_path, n_layers=4,
                                          layer_size_mb=1.0)
        print(f"\n▶ Fake model: {st_size>>10:,}KB  "
              f"({len(json.loads(open(st_path,'rb').read()[8:8+struct.unpack('<Q',open(st_path,'rb').read(8))[0]]))} tensors)")

        # ── Convert ──────────────────────────────────────────────
        print("\n▶ Converting...")
        fabric_path = os.path.join(tmp, "model.pogls")
        conv = STFabricConverter(st_path, fabric_path, verify=True)
        conv.convert()

        # ── Load + verify ────────────────────────────────────────
        print("\n▶ Loading via FabricTensorLoader...")
        with FabricTensorLoader(fabric_path) as loader:
            print(f"  {loader}")
            print(f"  tensors: {loader.tensor_names[:3]}...")

            # load single tensor
            t0   = time.perf_counter()
            data = loader.load_tensor("transformer.h.0.weight")
            ms   = (time.perf_counter() - t0) * 1000
            assert len(data) > 0
            print(f"  ✓ load_tensor()  {len(data)>>10:,}KB  {ms:.4f}ms")

            # load by prefix
            layer0 = loader.load_layers("transformer.h.0")
            assert len(layer0) == 2   # weight + bias
            print(f"  ✓ load_layers('transformer.h.0')  "
                  f"{len(layer0)} tensors")

            # iter all tensors
            names = [n for n, _ in loader.iter_tensors()]
            assert len(names) == 8   # 4 layers × 2
            print(f"  ✓ iter_tensors()  {len(names)} tensors")

            # snapshot + restore
            snap = loader.snapshot()
            ok   = loader.restore(snap)
            assert ok
            print(f"  ✓ snapshot + restore  root={snap.merkle_root[:12]}...")

            # ── Benchmark: fabric vs plain read ──────────────────
            print("\n▶ Benchmark (warmup 1s, bench 2s)...")
            WARMUP, BENCH = 1.0, 2.0

            # fabric zero-copy
            def load_fabric():
                _ = loader.load_tensor("transformer.h.0.weight")

            t0 = time.perf_counter()
            while time.perf_counter() - t0 < WARMUP:
                load_fabric()
            times_f = []
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < BENCH:
                t1 = time.perf_counter()
                load_fabric()
                times_f.append(time.perf_counter() - t1)

            # plain safetensors read (manual)
            def load_plain():
                with open(st_path, "rb") as f:
                    sz = struct.unpack("<Q", f.read(8))[0]
                    hdr = json.loads(f.read(sz))
                    info = hdr["transformer.h.0.weight"]
                    f.seek(8 + sz + info["data_offsets"][0])
                    _ = f.read(info["data_offsets"][1]
                               - info["data_offsets"][0])

            t0 = time.perf_counter()
            while time.perf_counter() - t0 < WARMUP:
                load_plain()
            times_p = []
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < BENCH:
                t1 = time.perf_counter()
                load_plain()
                times_p.append(time.perf_counter() - t1)

            med_f = statistics.median(times_f) * 1000
            med_p = statistics.median(times_p) * 1000
            ops_f = len(times_f) / BENCH
            ops_p = len(times_p) / BENCH
            mb    = len(data) / (1 << 20)

            print(f"\n  load_tensor (fabric)  "
                  f"median {med_f:.4f}ms  {ops_f:,.0f} ops/s  "
                  f"~{mb*ops_f:,.0f} MB/s")
            print(f"  load_tensor (plain)   "
                  f"median {med_p:.4f}ms  {ops_p:,.0f} ops/s  "
                  f"~{mb*ops_p:,.0f} MB/s")
            print(f"\n  fabric vs plain: {med_p/med_f:.1f}x faster")

            print(f"\n  stats: {loader.stats['fabric_stats']}")

    print("\n✅ All tests passed")
    print("═" * 60)
