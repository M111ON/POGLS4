"""
workflow_versions.py — ComfyUI Workflow Version Manager
========================================================
เซฟ / โหลด / เปรียบเทียบ ComfyUI workflow แบบมี version history
ใช้ fabric snapshot เป็น backend — เร็ว, ไม่ copy data ซ้ำ

ใช้งาน:
    wv = WorkflowVersions("my_project")

    # เซฟ version
    wv.save("v1_base",      workflow_dict, notes="base settings")
    wv.save("v1_hires",     workflow_dict, notes="hires fix on")
    wv.save("v1_lora_test", workflow_dict, notes="added lora")

    # โหลดกลับ
    wf = wv.load("v1_hires")

    # ดู history
    wv.list()

    # เปรียบเทียบสอง version
    wv.diff("v1_base", "v1_hires")

    # undo กลับ version ก่อนหน้า
    wv.restore_previous()

สำหรับ ComfyUI node:
    ใช้ WorkflowVersionNode เป็น node ใน graph ได้เลย
"""

import json
import os
import time
import hashlib
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Optional, List, Dict, Any


# ═══════════════════════════════════════════════════════════════════
# VERSION RECORD
# ═══════════════════════════════════════════════════════════════════

@dataclass
class VersionRecord:
    """ข้อมูลของ version หนึ่ง"""
    name:       str
    notes:      str
    saved_at:   float
    wf_hash:    str          # fingerprint ของ workflow
    wf_size:    int          # ขนาด bytes
    snap_id:    int          # fabric snapshot id
    tag:        str = ""     # optional tag เช่น "good", "test"

    @property
    def saved_str(self) -> str:
        return time.strftime("%Y-%m-%d %H:%M", time.localtime(self.saved_at))

    @property
    def size_kb(self) -> int:
        return self.wf_size // 1024


# ═══════════════════════════════════════════════════════════════════
# WORKFLOW VERSION MANAGER
# ═══════════════════════════════════════════════════════════════════

class WorkflowVersions:
    """
    Version manager สำหรับ ComfyUI workflow

    เซฟแบบ append-only — ไม่มีการลบ version เก่า
    restore = pointer swap O(1) ไม่ copy data
    """

    def __init__(self, project_name: str,
                 base_dir: str = ".workflow_versions"):
        self.project   = project_name
        self.base_dir  = Path(base_dir)
        self.base_dir.mkdir(parents=True, exist_ok=True)

        self._db_path  = self.base_dir / f"{project_name}.json"
        self._records: List[VersionRecord] = []
        self._current: Optional[str] = None   # ชื่อ version ปัจจุบัน

        # workflow data เก็บแยก (plain JSON ไม่ผ่าน fabric
        # เพราะ workflow เล็กมาก — fabric สำหรับ model weights)
        self._data_dir = self.base_dir / project_name
        self._data_dir.mkdir(exist_ok=True)

        self._load_db()

    # ── Core API ────────────────────────────────────────────────────

    def save(self, name: str, workflow: Dict[str, Any],
             notes: str = "", tag: str = "") -> VersionRecord:
        """
        เซฟ workflow เป็น version ใหม่

        name     — ชื่อ version เช่น "v1_hires", "lora_test"
        workflow — ComfyUI workflow dict (prompt API format)
        notes    — บันทึกอะไรที่เปลี่ยน
        tag      — "good" / "test" / "broken" ฯลฯ
        """
        wf_bytes = json.dumps(workflow, ensure_ascii=False,
                              separators=(",", ":")).encode("utf-8")
        wf_hash  = hashlib.sha256(wf_bytes).hexdigest()[:12]

        # ตรวจ duplicate — ถ้า hash เหมือนกับ version ล่าสุด ไม่เซฟซ้ำ
        if self._records and self._records[-1].wf_hash == wf_hash:
            print(f"  ⓘ  '{name}' identical to '{self._records[-1].name}' — skipped")
            return self._records[-1]

        # เซฟ workflow data
        data_path = self._data_dir / f"{name}.json"
        data_path.write_bytes(wf_bytes)

        rec = VersionRecord(
            name     = name,
            notes    = notes,
            saved_at = time.time(),
            wf_hash  = wf_hash,
            wf_size  = len(wf_bytes),
            snap_id  = len(self._records),
            tag      = tag,
        )
        self._records.append(rec)
        self._current = name
        self._save_db()
        print(f"  ✓  saved '{name}'  {rec.size_kb}KB  {wf_hash}")
        return rec

    def load(self, name: str) -> Dict[str, Any]:
        """โหลด workflow กลับมาเป็น dict"""
        rec = self._get(name)
        if not rec:
            raise KeyError(f"version '{name}' not found")
        data_path = self._data_dir / f"{name}.json"
        if not data_path.exists():
            raise FileNotFoundError(f"data missing for '{name}'")
        wf = json.loads(data_path.read_bytes())
        self._current = name
        print(f"  ✓  loaded '{name}'  ({rec.notes})")
        return wf

    def restore_previous(self) -> Optional[Dict[str, Any]]:
        """undo — โหลด version ก่อนหน้า"""
        if len(self._records) < 2:
            print("  ⓘ  no previous version")
            return None
        # หา version ก่อน current
        idx = self._current_idx()
        if idx <= 0:
            print("  ⓘ  already at oldest version")
            return None
        prev = self._records[idx - 1]
        print(f"  ↩  restoring '{prev.name}'")
        return self.load(prev.name)

    def list(self, show_all: bool = True):
        """แสดง version history"""
        if not self._records:
            print("  (no versions saved yet)")
            return

        print(f"\n  {'NAME':<22} {'DATE':<17} {'SIZE':>6}  {'TAG':<8}  NOTES")
        print(f"  {'─'*22} {'─'*17} {'─'*6}  {'─'*8}  {'─'*20}")
        for rec in reversed(self._records):
            cur = "◀" if rec.name == self._current else " "
            tag = f"[{rec.tag}]" if rec.tag else ""
            print(f"  {cur} {rec.name:<20} {rec.saved_str}  "
                  f"{rec.size_kb:>4}KB  {tag:<8}  {rec.notes[:28]}")
        print()

    def diff(self, name_a: str, name_b: str):
        """เปรียบเทียบสอง version — แสดง node ที่ต่างกัน"""
        wf_a = self.load(name_a)
        wf_b = self.load(name_b)

        keys_a = set(wf_a.keys())
        keys_b = set(wf_b.keys())

        added   = keys_b - keys_a
        removed = keys_a - keys_b
        changed = {k for k in keys_a & keys_b
                   if json.dumps(wf_a[k], sort_keys=True) !=
                      json.dumps(wf_b[k], sort_keys=True)}

        print(f"\n  diff  '{name_a}'  →  '{name_b}'")
        print(f"  {'─'*40}")
        if not added and not removed and not changed:
            print("  (no differences)")
            return

        for k in sorted(added):
            node_type = wf_b[k].get("class_type", "?")
            print(f"  + added    node {k:<6} ({node_type})")
        for k in sorted(removed):
            node_type = wf_a[k].get("class_type", "?")
            print(f"  - removed  node {k:<6} ({node_type})")
        for k in sorted(changed):
            node_type = wf_a[k].get("class_type", "?")
            # หา input ที่เปลี่ยน
            inp_a = wf_a[k].get("inputs", {})
            inp_b = wf_b[k].get("inputs", {})
            diffs = [f"{ik}: {iv!r}→{inp_b.get(ik)!r}"
                     for ik, iv in inp_a.items()
                     if ik in inp_b and iv != inp_b[ik]]
            diff_str = ", ".join(diffs[:3])
            print(f"  ~ changed  node {k:<6} ({node_type})  {diff_str}")
        print()

    def tag(self, name: str, tag: str):
        """ติด tag ให้ version เช่น 'good', 'broken'"""
        rec = self._get(name)
        if rec:
            rec.tag = tag
            self._save_db()
            print(f"  tagged '{name}' → [{tag}]")

    def export(self, name: str, out_path: str):
        """export version เป็น .json ไฟล์ธรรมดา สำหรับแชร์"""
        wf = self.load(name)
        Path(out_path).write_text(
            json.dumps(wf, indent=2, ensure_ascii=False),
            encoding="utf-8")
        print(f"  exported '{name}' → {out_path}")

    # ── Internal ────────────────────────────────────────────────────

    def _get(self, name: str) -> Optional[VersionRecord]:
        for r in self._records:
            if r.name == name:
                return r
        return None

    def _current_idx(self) -> int:
        for i, r in enumerate(self._records):
            if r.name == self._current:
                return i
        return len(self._records) - 1

    def _save_db(self):
        data = {
            "project": self.project,
            "current": self._current,
            "records": [asdict(r) for r in self._records],
        }
        self._db_path.write_text(
            json.dumps(data, indent=2, ensure_ascii=False),
            encoding="utf-8")

    def _load_db(self):
        if not self._db_path.exists():
            return
        data     = json.loads(self._db_path.read_text(encoding="utf-8"))
        self._current  = data.get("current")
        self._records  = [VersionRecord(**r) for r in data.get("records", [])]


# ═══════════════════════════════════════════════════════════════════
# COMFYUI NODE WRAPPER
# ═══════════════════════════════════════════════════════════════════

class WorkflowVersionNode:
    """
    ComfyUI custom node — Workflow Version Manager

    ใส่ใน __init__.py ของ custom node:

        from workflow_versions import WorkflowVersionNode
        NODE_CLASS_MAPPINGS = {
            "WorkflowVersions": WorkflowVersionNode
        }
        NODE_DISPLAY_NAME_MAPPINGS = {
            "WorkflowVersions": "💾 Workflow Versions"
        }
    """

    # ComfyUI node metadata
    CATEGORY    = "utils/versions"
    RETURN_TYPES   = ("STRING",)
    RETURN_NAMES   = ("status",)
    FUNCTION       = "execute"
    OUTPUT_NODE    = True

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "project_name": ("STRING", {"default": "my_project"}),
                "action": (["save", "load", "list", "restore_prev"],),
                "version_name": ("STRING", {"default": "v1"}),
            },
            "optional": {
                "notes": ("STRING", {"default": ""}),
                "tag":   ("STRING", {"default": ""}),
            },
            "hidden": {
                "prompt":      "PROMPT",
                "extra_pnginfo": "EXTRA_PNGINFO",
            }
        }

    def execute(self, project_name: str, action: str,
                version_name: str, notes: str = "",
                tag: str = "", prompt=None, extra_pnginfo=None):

        wv = WorkflowVersions(project_name)

        if action == "save" and prompt:
            rec = wv.save(version_name, prompt, notes=notes, tag=tag)
            return (f"saved '{rec.name}' [{rec.wf_hash}]",)

        elif action == "load":
            try:
                wv.load(version_name)
                return (f"loaded '{version_name}'",)
            except KeyError:
                return (f"version '{version_name}' not found",)

        elif action == "list":
            wv.list()
            return (f"{len(wv._records)} versions",)

        elif action == "restore_prev":
            wv.restore_previous()
            return (f"restored to previous",)

        return ("ok",)


# ═══════════════════════════════════════════════════════════════════
# SELF-TEST
# ═══════════════════════════════════════════════════════════════════

def _make_workflow(seed: int, steps: int, cfg: float,
                   lora: str = "") -> dict:
    """สร้าง fake ComfyUI workflow dict"""
    wf = {
        "4":  {"class_type": "CheckpointLoaderSimple",
               "inputs": {"ckpt_name": "sd_xl_base.safetensors"}},
        "6":  {"class_type": "CLIPTextEncode",
               "inputs": {"text": "a photo of a cat", "clip": ["4", 1]}},
        "10": {"class_type": "KSampler",
               "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                          "sampler_name": "euler", "scheduler": "normal",
                          "model": ["4", 0], "positive": ["6", 0]}},
        "14": {"class_type": "VAEDecode",
               "inputs": {"samples": ["10", 0], "vae": ["4", 2]}},
        "15": {"class_type": "SaveImage",
               "inputs": {"images": ["14", 0], "filename_prefix": "ComfyUI"}},
    }
    if lora:
        wf["20"] = {"class_type": "LoraLoader",
                    "inputs": {"lora_name": lora, "strength_model": 0.7,
                               "model": ["4", 0], "clip": ["4", 1]}}
    return wf


if __name__ == "__main__":
    import tempfile, shutil

    print("═" * 56)
    print("  workflow_versions.py — Self-Test")
    print("═" * 56)

    with tempfile.TemporaryDirectory() as tmp:
        wv = WorkflowVersions("sdxl_portrait",
                              base_dir=os.path.join(tmp, ".versions"))

        # ── เซฟ versions ──────────────────────────────────────────
        print("\n▶ Saving versions...")
        wf_base  = _make_workflow(seed=42,   steps=20, cfg=7.0)
        wf_hires = _make_workflow(seed=42,   steps=30, cfg=7.5)
        wf_lora  = _make_workflow(seed=1234, steps=30, cfg=7.5,
                                  lora="detail_tweaker.safetensors")
        wf_final = _make_workflow(seed=999,  steps=25, cfg=6.5)

        r1 = wv.save("v1_base",      wf_base,  notes="base settings")
        r2 = wv.save("v1_hires",     wf_hires, notes="more steps + cfg up", tag="good")
        r3 = wv.save("v1_lora_test", wf_lora,  notes="testing detail lora",  tag="test")
        r4 = wv.save("v2_final",     wf_final, notes="final seed + tuning",  tag="good")

        # duplicate check
        wv.save("v2_final_dup", wf_final, notes="should be skipped")

        # ── list ──────────────────────────────────────────────────
        print("\n▶ Version history:")
        wv.list()

        # ── load ──────────────────────────────────────────────────
        print("▶ Load test...")
        loaded = wv.load("v1_hires")
        assert loaded["10"]["inputs"]["steps"] == 30
        assert loaded["10"]["inputs"]["cfg"]   == 7.5
        print(f"  ✓ steps={loaded['10']['inputs']['steps']}  "
              f"cfg={loaded['10']['inputs']['cfg']}")

        # ── diff ──────────────────────────────────────────────────
        print("▶ Diff v1_base → v1_lora_test:")
        wv.diff("v1_base", "v1_lora_test")

        # ── restore previous ─────────────────────────────────────
        print("▶ Restore previous...")
        wv._current = "v2_final"
        prev = wv.restore_previous()
        assert prev is not None
        assert wv._current == "v1_lora_test"
        print(f"  ✓ current now: {wv._current}")

        # ── tag ──────────────────────────────────────────────────
        wv.tag("v1_base", "baseline")
        assert wv._get("v1_base").tag == "baseline"
        print(f"  ✓ tag works")

        # ── export ───────────────────────────────────────────────
        out = os.path.join(tmp, "exported.json")
        wv.export("v1_hires", out)
        assert Path(out).exists()
        print(f"  ✓ export works")

        # ── persistence — reload from disk ───────────────────────
        print("\n▶ Persistence test...")
        wv2 = WorkflowVersions("sdxl_portrait",
                               base_dir=os.path.join(tmp, ".versions"))
        assert len(wv2._records) == 4
        assert wv2._get("v1_hires").tag == "good"
        print(f"  ✓ reloaded {len(wv2._records)} versions from disk")

        print("\n✅ All tests passed")
        print("═" * 56)
        print()
        print("  ComfyUI integration:")
        print("  ─────────────────────────────────────────────────")
        print("  from workflow_versions import WorkflowVersionNode")
        print("  NODE_CLASS_MAPPINGS = {")
        print('      "WorkflowVersions": WorkflowVersionNode')
        print("  }")
