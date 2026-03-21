"""
pogls_fabric.py — POGLS Acceleration Fabric
============================================
Blackbox backend สำหรับ tool ที่ต้องการ:
  • Zero-copy memory-mapped slicing
  • อ่าน model ใหญ่เหมือนอยู่ใน RAM เต็มก้อน
  • Version snapshot ราคาถูก (root pointer swap)
  • Audit read-only แยกจาก data หลัก
  • Scale จากเครื่องเดียวไป multi-volume ได้

API (blackbox — caller ไม่ต้องรู้ข้างใน):
  fabric = BlockFabric("model.pogls")
  view   = fabric.slice(offset=0, length=4<<20)   # 4MB window
  data   = view.read()                             # zero-copy
  snap   = fabric.snapshot()                       # O(1)
  fabric.restore(snap)

ไม่มี:
  • migration engine
  • A/B world
  • phase orchestration
  • distributed layer

สิ่งที่มีแค่:
  • mmap + fixed-width block
  • deterministic offset (bit-shift)
  • immutable append + snapshot pointer
  • read-only audit lane
"""

import os
import mmap
import struct
import hashlib
import threading
import time
from dataclasses import dataclass, field
from typing import Optional, List, Iterator
from pathlib import Path

def _page_flush(mm, offset: int, length: int):
    """mmap.flush บน Linux ต้องการ page-aligned offset+length"""
    page = mmap.PAGESIZE
    start = (offset // page) * page
    end   = (((offset + length - 1) // page) + 1) * page
    mm.flush(start, end - start)


# ═══════════════════════════════════════════════════════════════════
# CONSTANTS — กำหนดครั้งเดียว ไม่แก้ logic
# ═══════════════════════════════════════════════════════════════════

BLOCK_SHIFT      = 8                    # 1 << 8 = 256 bytes per block
BLOCK_SIZE       = 1 << BLOCK_SHIFT     # 256B — Deep Block size จาก POGLS spec
HEADER_SIZE      = 32                   # Smart Header (extended)
MAGIC            = b"PGFB"              # POGLS Fabric
VERSION          = 0x01

# Header layout (32 bytes):
#  [0-3]   magic       4B
#  [4]     version     1B
#  [5]     flags       1B   bit0=immutable, bit1=compressed
#  [6-7]   reserved    2B
#  [8-15]  block_count 8B   จำนวน block ที่เขียนแล้ว
#  [16-23] root_snap   8B   block index ของ root snapshot
#  [24-31] epoch       8B   monotonic counter กัน stale restore

HEADER_FMT = ">4sBBH Q Q Q"   # = 32 bytes


# ═══════════════════════════════════════════════════════════════════
# DATA STRUCTURES
# ═══════════════════════════════════════════════════════════════════

@dataclass(frozen=True)
class SnapshotPointer:
    """
    Snapshot = root pointer เท่านั้น
    ไม่มี copy ไม่มี duplicate data
    O(1) สร้าง O(1) restore
    """
    snap_id:     int        # monotonic id
    root_block:  int        # block index ของ snapshot record
    block_count: int        # block count ณ เวลา snapshot
    merkle_root: str        # hash ของ block range ณ เวลานั้น
    epoch:       int        # epoch ป้องกัน stale restore
    created_at:  float = field(default_factory=time.time)


@dataclass
class SliceView:
    """
    Zero-copy window เข้า fabric
    อ่านได้ ห้ามเขียน — read-only lane
    """
    _fabric:     "BlockFabric"
    offset:      int        # byte offset ใน fabric
    length:      int        # ขนาดที่ต้องการอ่าน

    def read(self) -> memoryview:
        """
        คืน memoryview — zero-copy, ไม่ allocate buffer ใหม่
        caller ใช้ได้เลยโดยไม่ copy
        """
        return self._fabric._mmap_read(self.offset, self.length)

    def read_bytes(self) -> bytes:
        """คืน bytes สำหรับกรณีที่ต้องการ copy จริงๆ"""
        return bytes(self.read())

    def iter_blocks(self, batch_blocks: int = 256) -> Iterator[memoryview]:
        """
        Streaming เข้า VRAM — yield ทีละ batch (default 256 blocks = 64KB)
        batch_blocks ปรับตาม VRAM transfer unit ได้

        ตัวอย่าง:
          for chunk in view.iter_blocks(batch_blocks=4096):  # 1MB per yield
              gpu.load(chunk)
        """
        batch_size = BLOCK_SIZE * batch_blocks
        pos = self.offset
        end = self.offset + self.length
        while pos < end:
            chunk = min(batch_size, end - pos)
            yield self._fabric._mmap_read(pos, chunk)
            pos += chunk

    def prefetch(self, mode: str = "sequential"):
        """
        บอก OS ล่วงหน้าว่าจะอ่าน region นี้ — ลด page fault
        mode: "sequential"  → MADV_SEQUENTIAL (streaming)
              "random"      → MADV_RANDOM      (random access)
              "willneed"    → MADV_WILLNEED     (โหลดล่วงหน้าทันที)

        เรียกก่อน read() หรือ iter_blocks() เพื่อ warm cache
        """
        try:
            self._fabric._mm.madvise(
                {"sequential": mmap.MADV_SEQUENTIAL,
                 "random":     mmap.MADV_RANDOM,
                 "willneed":   mmap.MADV_WILLNEED,
                }.get(mode, mmap.MADV_SEQUENTIAL),
                self.offset, self.length
            )
        except (AttributeError, OSError):
            pass   # Windows ไม่มี madvise — silent fallback
        return self   # chain ได้: view.prefetch().read()

    @property
    def block_count(self) -> int:
        return (self.length + BLOCK_SIZE - 1) >> BLOCK_SHIFT


# ═══════════════════════════════════════════════════════════════════
# BLOCK FABRIC — core engine
# ═══════════════════════════════════════════════════════════════════

class BlockFabric:
    """
    Pure block fabric — blackbox backend

    ใช้งาน:
        fabric = BlockFabric("model.pogls")
        view   = fabric.slice(0, 4 * 1024 * 1024)
        data   = view.read()                # zero-copy memoryview
        snap   = fabric.snapshot()          # O(1)
        fabric.restore(snap)               # O(1)
    """

    def __init__(self, path: str, create: bool = False,
                 max_size: int = 0):
        self.path      = Path(path)
        self._lock     = threading.RLock()
        self._mm:      Optional[mmap.mmap] = None
        self._fd:      Optional[int]       = None
        self._snaps:   List[SnapshotPointer] = []
        self._snap_seq = 0

        # Stats (audit lane — read-only, ไม่แตะ data)
        self._stats = {
            "reads":      0,
            "writes":     0,
            "snapshots":  0,
            "bytes_read": 0,
        }

        if create or not self.path.exists():
            self._create(max_size or (1 << 30))  # default 1GB pre-alloc
        else:
            self._open()

    # ─── Init ──────────────────────────────────────────────────────

    def _create(self, max_size: int):
        """สร้าง fabric ใหม่ พร้อม Smart Header"""
        alloc = max(HEADER_SIZE + BLOCK_SIZE,
                    ((max_size + BLOCK_SIZE - 1) >> BLOCK_SHIFT) << BLOCK_SHIFT)

        # เขียน header ก่อน open mmap
        hdr = struct.pack(
            HEADER_FMT,
            MAGIC, VERSION, 0x01, 0,
            0, 0, 1,   # block_count=0, root_snap=0, epoch=1
        )
        with open(self.path, "w+b") as f:
            f.write(hdr)
            f.seek(alloc - 1)
            f.write(b"\x00")

        self._open()   # mmap จะอ่าน header ที่เขียนแล้ว

    def _open(self):
        """เปิด file + mmap"""
        self._fd = os.open(str(self.path), os.O_RDWR)
        size     = os.fstat(self._fd).st_size
        self._mm = mmap.mmap(self._fd, size,
                             access=mmap.ACCESS_WRITE)
        self._load_header()

    def _write_header(self, block_count: int, root_snap: int, epoch: int):
        hdr = struct.pack(
            HEADER_FMT,
            MAGIC, VERSION, 0x01, 0,   # magic, version, flags(immutable), reserved
            block_count,
            root_snap,
            epoch,
        )
        with self._lock:
            self._mm[0:HEADER_SIZE] = hdr
            _page_flush(self._mm, 0, HEADER_SIZE)

        self._block_count = block_count
        self._root_snap   = root_snap
        self._epoch       = epoch

    def _load_header(self):
        raw = bytes(self._mm[0:HEADER_SIZE])
        magic, ver, flags, _res, bc, rs, ep = struct.unpack(HEADER_FMT, raw)
        if magic != MAGIC:
            raise ValueError(f"Invalid fabric magic: {magic}")
        self._block_count = bc
        self._root_snap   = rs
        self._epoch       = ep

    # ─── Blackbox API ──────────────────────────────────────────────

    def slice(self, offset: int, length: int) -> SliceView:
        """
        คืน zero-copy window เข้า fabric
        offset และ length หน่วยเป็น bytes

        ไม่ copy ไม่ allocate ไม่แตะ data
        """
        if offset < 0 or length <= 0:
            raise ValueError("offset >= 0, length > 0")
        end = offset + length
        if end > self._mm.size():
            raise ValueError(f"slice [{offset}:{end}] out of bounds")
        return SliceView(_fabric=self, offset=offset, length=length)

    def write(self, data: bytes, offset: Optional[int] = None) -> int:
        """
        เขียน data ลง fabric
        offset=None → append ต่อท้าย (immutable append mode)
        คืน byte offset ที่เขียนไป
        """
        with self._lock:
            if offset is None:
                # append — deterministic: block-aligned
                write_at = HEADER_SIZE + (self._block_count << BLOCK_SHIFT)
            else:
                write_at = offset

            end = write_at + len(data)
            if end > self._mm.size():
                raise IOError(f"Fabric full — resize ก่อน")

            self._mm[write_at:end] = data
            _page_flush(self._mm, write_at, len(data))

            if offset is None:
                n_blocks = (len(data) + BLOCK_SIZE - 1) >> BLOCK_SHIFT
                self._block_count += n_blocks
                self._write_header(self._block_count,
                                   self._root_snap,
                                   self._epoch)

            self._stats["writes"]     += 1
            self._stats["bytes_read"] += len(data)  # track write volume too
            return write_at

    def snapshot(self) -> SnapshotPointer:
        """
        สร้าง snapshot — O(delta) ไม่ใช่ O(n)

        hash เฉพาะ block ที่เพิ่มมาตั้งแต่ snapshot ก่อนหน้า
        chain กับ root เก่า: root_new = H(root_prev || delta_hash)
        ถ้าไม่มีอะไรใหม่ → O(1) จริงๆ
        """
        with self._lock:
            prev_count = 0
            prev_root  = b""
            if self._snaps:
                last       = self._snaps[-1]
                prev_count = last.block_count
                prev_root  = bytes.fromhex(last.merkle_root)

            if self._block_count == prev_count:
                # ไม่มี delta — reuse root เดิม
                delta_root = (self._snaps[-1].merkle_root if self._snaps
                              else hashlib.sha256(b"").hexdigest())
            else:
                # hash เฉพาะ delta ที่เพิ่มมา
                delta_hash = self._compute_range_hash(prev_count, self._block_count)
                # chain: ผูก history ไว้ใน root
                delta_root = hashlib.sha256(
                    prev_root + bytes.fromhex(delta_hash)
                ).hexdigest()

            snap = SnapshotPointer(
                snap_id    = self._snap_seq,
                root_block = self._block_count,
                block_count= self._block_count,
                merkle_root= delta_root,
                epoch      = self._epoch,
            )
            self._snaps.append(snap)
            self._snap_seq += 1
            self._stats["snapshots"] += 1
            return snap

    def restore(self, snap: SnapshotPointer) -> bool:
        """
        Restore ไปยัง snapshot — O(1)
        เปลี่ยน root pointer เท่านั้น ไม่แตะ block data
        """
        with self._lock:
            if snap.epoch != self._epoch:
                return False   # stale snapshot — epoch mismatch
            self._block_count = snap.block_count
            self._root_snap   = snap.root_block
            self._write_header(self._block_count,
                               self._root_snap,
                               self._epoch)
            return True

    def verify(self, snap: SnapshotPointer) -> bool:
        """
        Audit lane — ตรวจว่า data ณ snap ยังครบถ้วนไหม
        read-only, ไม่แตะ data หลัก

        rebuild chained root ตั้งแต่ต้นจนถึง snap นี้
        เพื่อให้ตรงกับ root ที่ snapshot() สร้างไว้
        """
        # หา chain ของ snaps ทั้งหมดจนถึง snap_id นี้
        chain = [s for s in self._snaps if s.snap_id <= snap.snap_id]
        if not chain:
            return False

        prev_count = 0
        prev_root  = b""
        result_root = ""
        for s in chain:
            if s.block_count == prev_count:
                result_root = (self._snaps[self._snaps.index(s)-1].merkle_root
                               if self._snaps.index(s) > 0
                               else hashlib.sha256(b"").hexdigest())
            else:
                dh = self._compute_range_hash(prev_count, s.block_count)
                result_root = hashlib.sha256(
                    prev_root + bytes.fromhex(dh)
                ).hexdigest()
            prev_count = s.block_count
            prev_root  = bytes.fromhex(result_root)

        return result_root == snap.merkle_root

    # ─── Zero-copy internal ────────────────────────────────────────

    def _mmap_read(self, offset: int, length: int) -> memoryview:
        """
        คืน memoryview — zero allocation
        เป็น core ของ zero-copy path ทั้งหมด
        """
        self._stats["reads"]      += 1
        self._stats["bytes_read"] += length
        return memoryview(self._mm)[offset : offset + length]

    def _compute_range_hash(self, start_block: int,
                             end_block: int) -> str:
        """
        Hash block range — incremental sha256
        อ่านครั้งละ 1MB (4096 blocks) เพื่อลด Python loop overhead
        """
        h    = hashlib.sha256()
        pos  = HEADER_SIZE + (start_block << BLOCK_SHIFT)
        end  = HEADER_SIZE + (end_block   << BLOCK_SHIFT)
        step = BLOCK_SIZE << 12   # 4096 blocks = 1MB per read
        while pos < end:
            chunk = min(step, end - pos)
            h.update(self._mm[pos : pos + chunk])
            pos += chunk
        return h.hexdigest()

    # ─── Stats (audit lane) ────────────────────────────────────────

    @property
    def stats(self) -> dict:
        """Read-only audit data — ไม่แตะ fabric state"""
        with self._lock:
            return {
                **self._stats,
                "block_count":  self._block_count,
                "snapshot_count": len(self._snaps),
                "size_mb":      round(self._mm.size() / (1 << 20), 2),
                "used_mb":      round(
                    (HEADER_SIZE + (self._block_count << BLOCK_SHIFT))
                    / (1 << 20), 2),
            }

    # ─── Lifecycle ─────────────────────────────────────────────────

    def close(self):
        with self._lock:
            if self._mm:
                try:
                    self._mm.flush()
                except Exception:
                    pass
                try:
                    self._mm.close()
                except BufferError:
                    pass  # memoryviews ยังอยู่ — GC จัดการเอง
                self._mm = None
            if self._fd is not None:
                try:
                    os.close(self._fd)
                except OSError:
                    pass
                self._fd = None

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()

    def __repr__(self):
        return (f"BlockFabric({self.path.name!r} "
                f"blocks={self._block_count} "
                f"snaps={len(self._snaps)})")


# ═══════════════════════════════════════════════════════════════════
# VRAM LOADER — ตัวอย่างการใช้ blackbox API
# สำหรับ custom node ที่โหลด model ใหญ่แบบ slice
# ═══════════════════════════════════════════════════════════════════

class VRAMLoader:
    """
    ตัวอย่าง: โหลด model ใหญ่เข้า VRAM ทีละ slice
    Caller ไม่รู้ว่ามี BlockFabric อยู่เบื้องหลัง
    """

    def __init__(self, fabric: BlockFabric,
                 vram_budget_mb: int = 4096):
        self._fabric     = fabric
        self._budget     = vram_budget_mb << 20
        self._window_start = 0

    def load_next_window(self) -> Optional[memoryview]:
        """
        คืน memoryview ของ window ถัดไป
        zero-copy — GPU อ่านได้ตรงจาก pointer นี้
        """
        fabric_size = (self._fabric._block_count << BLOCK_SHIFT)
        if self._window_start >= fabric_size:
            return None   # โหลดครบแล้ว

        length = min(self._budget,
                     fabric_size - self._window_start)
        view = self._fabric.slice(
            HEADER_SIZE + self._window_start, length)

        self._window_start += length
        return view.read()

    def reset(self):
        """เริ่มโหลดใหม่จากต้น"""
        self._window_start = 0


# ═══════════════════════════════════════════════════════════════════
# SELF-TEST
# ═══════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    import tempfile, os

    print("═" * 56)
    print("  pogls_fabric.py — Blackbox Backend Self-Test")
    print("═" * 56)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "test.pogls")

        with BlockFabric(path, create=True, max_size=16<<20) as fab:
            print(f"\n▶ Created: {fab}")

            # 1. Write model data
            chunk_a = bytes(range(256)) * 16   # 4KB
            chunk_b = bytes([0xFF] * 4096)     # 4KB

            off_a = fab.write(chunk_a)
            off_b = fab.write(chunk_b)
            print(f"  wrote chunk_a @ offset {off_a}")
            print(f"  wrote chunk_b @ offset {off_b}")

            # 2. Zero-copy slice
            view = fab.slice(off_a, len(chunk_a))
            data = view.read_bytes()
            assert data == chunk_a, "slice mismatch"
            print(f"  ✓ zero-copy slice: {len(data)}B match")

            # 3. iter_blocks (streaming)
            blocks = list(view.iter_blocks())
            total = sum(len(bytes(b)) for b in blocks)
            assert total == len(chunk_a), f"iter total {total} != {len(chunk_a)}"
            print(f"  ✓ iter_blocks: {len(blocks)} blocks")

            # 4. Snapshot O(1)
            snap1 = fab.snapshot()
            print(f"  ✓ snapshot #{snap1.snap_id} "
                  f"blocks={snap1.block_count} "
                  f"root={snap1.merkle_root[:12]}...")

            # 5. Write more after snapshot
            fab.write(bytes([0xAB] * 2048))
            assert fab._block_count > snap1.block_count
            print(f"  wrote more: blocks now {fab._block_count}")

            # 6. Audit verify
            assert fab.verify(snap1), "verify failed"
            print(f"  ✓ audit verify: snapshot still valid")

            # 7. Restore O(1)
            ok = fab.restore(snap1)
            assert ok
            assert fab._block_count == snap1.block_count
            print(f"  ✓ restore: blocks back to {fab._block_count}")

            # 8. VRAMLoader
            fab.write(bytes(range(256)) * 64)  # 16KB more
            loader = VRAMLoader(fab, vram_budget_mb=1)
            windows = 0
            while True:
                w = loader.load_next_window()
                if w is None:
                    break
                windows += 1
            print(f"  ✓ VRAMLoader: {windows} window(s)")

            # 9. Stats (audit lane)
            s = fab.stats
            print(f"\n  Stats: {s}")

            print(f"\n✅ All tests passed — blackbox API ready")
            print("═" * 56)
