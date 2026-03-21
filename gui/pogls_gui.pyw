#!/usr/bin/env python3
"""
POGLS V3.5  ·  Desktop Manager
================================
Tabbed UI — 4 tabs:
  1. Files     — file list + detail dock
  2. Hydra     — live VisualFeed (Hydra heads + Audit tiles)
  3. Simulate  — simulation controls (แยกออกมา, ทำงานได้จริง)
  4. Ingest    — import chat history / files into POGLS WAL

Config: pogls_config.json
Optional:
  pip install tkinterdnd2   → Drag & Drop
  pip install safetensors   → DNA Scan
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, colorchooser
import os, sys, json, math, hashlib, zipfile, threading, copy, time
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass, field, asdict
from typing import List, Optional, Dict, Tuple

# ── Windows DPI ───────────────────────────────────────────────────────
try:
    from ctypes import windll
    windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

# ── Optional DnD ─────────────────────────────────────────────────────
try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    _DND = True
    _Base = TkinterDnD.Tk
except ImportError:
    _DND = False
    _Base = tk.Tk


# ═══════════════════════════════════════════════════════════════════════
#  CONFIG
# ═══════════════════════════════════════════════════════════════════════

DEFAULT_CONFIG = {
    "window": {
        "width": 1300, "height": 800,
        "min_width": 1000, "min_height": 640,
        "sidebar_width": 200, "dock_width": 290,
    },
    "font": {
        "family": "Consolas",
        "size_logo": 15, "size_head": 12,
        "size_ui": 11,   "size_sm": 10,
        "size_xs": 9,    "size_tree": 10,
        "tree_row_height": 28,
    },
    "theme": {
        "mode": "dark",
        "bg": "#0b0d11",   "panel": "#0f1219",
        "card": "#141820", "border": "#1e2535",
        "amber": "#f5a623","cyan": "#19d4d4",
        "green": "#3de08a","red": "#e05555",
        "blue": "#4fa8ff", "text": "#d6dff0",
        "muted": "#49597a","hover": "#18202e",
        "sel": "#0d2245",  "selfg": "#4fa8ff",
    },
    "behavior": {
        "confirm_delete": False,
        "chunk_size_mb": 100,
        "max_recent_snaps": 50,
        "show_chunks_col": True,
        "status_timeout_ms": 7000,
    },
    "paths": {
        "database_file": "pogls_db.json",
        "dna_output_dir": "",
    },
}

CONFIG_PATH = Path("pogls_config.json")

def load_config() -> dict:
    cfg = copy.deepcopy(DEFAULT_CONFIG)
    if CONFIG_PATH.exists():
        try:
            user = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            for section, vals in user.items():
                if section.startswith("_"):
                    continue
                if section in cfg and isinstance(vals, dict):
                    cfg[section].update(
                        {k: v for k, v in vals.items()
                         if not k.startswith("_")})
        except Exception as e:
            print(f"[Config] load error: {e}")
    return cfg

def save_config(cfg: dict):
    out = {"_comment": "POGLS V3.5 Config", **cfg}
    CONFIG_PATH.write_text(
        json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")


# ═══════════════════════════════════════════════════════════════════════
#  DATA MODEL
# ═══════════════════════════════════════════════════════════════════════

TOPO_NAMES = [
    "Seed · 12v", "Preview · 42v", "Std · 162v",
    "Hi-Fi · 642v", "Ultra · 2562v",
]
EXT_LIST = ["all", ".safetensors", ".ckpt", ".pt", ".json", ".py", ".zip", ".png", ".txt"]

@dataclass
class Entry:
    eid:    str
    name:   str
    path:   str
    size:   int
    ext:    str
    addr:   int   = 0
    theta:  float = 0.0
    topo:   int   = 2
    added:  str   = ""
    tags:   List[str]  = field(default_factory=list)
    snaps:  List[dict] = field(default_factory=list)
    chunks: int   = 0
    mapped: bool  = False

    @property
    def size_hr(self) -> str:
        b = float(self.size)
        for u in ("B","KB","MB","GB","TB"):
            if b < 1024: return f"{b:.1f} {u}"
            b /= 1024
        return f"{b:.1f} PB"

    @property
    def addr_str(self) -> str:
        return f"{self.addr:,}"

    @property
    def topo_str(self) -> str:
        return TOPO_NAMES[min(self.topo, 4)]


class Database:
    def __init__(self, path: str = "pogls_db.json"):
        self.path = Path(path)
        self.data: Dict[str, Entry] = {}
        self._load()

    def _load(self):
        if not self.path.exists(): return
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            for k, v in raw.items():
                v.setdefault("chunks", 0); v.setdefault("tags", [])
                v.setdefault("snaps", []); v.setdefault("mapped", False)
                self.data[k] = Entry(**v)
        except Exception as e:
            print(f"[DB] load error: {e}")

    def save(self):
        self.path.write_text(
            json.dumps({k: asdict(v) for k, v in self.data.items()},
                       indent=2, ensure_ascii=False), encoding="utf-8")

    def add(self, e: Entry): self.data[e.eid] = e; self.save()
    def remove(self, eid: str): self.data.pop(eid, None); self.save()

    def search(self, q="", ext="all", topo=-1, mapped_only=False) -> List[Entry]:
        ql = q.lower().strip()
        out = []
        for e in self.data.values():
            if ext not in ("all","") and e.ext.lower() != ext.lower(): continue
            if topo >= 0 and e.topo != topo: continue
            if mapped_only and not e.mapped: continue
            if ql:
                hay = (e.name + e.path + " ".join(e.tags)).lower()
                if ql not in hay: continue
            out.append(e)
        return sorted(out, key=lambda x: x.added, reverse=True)

    def snap(self, eid: str, label: str) -> Optional[dict]:
        e = self.data.get(eid)
        if not e: return None
        s = {"sid": len(e.snaps), "label": label,
             "ts": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
             "addr": e.addr, "topo": e.topo, "state": "PENDING"}
        e.snaps.append(s); self.save(); return s

    def stats(self) -> dict:
        entries = list(self.data.values())
        total  = len(entries)
        mapped = sum(1 for e in entries if e.mapped)
        snapped= sum(1 for e in entries if e.snaps)
        size_b = sum(e.size for e in entries)
        by_ext: Dict[str, int] = {}
        for e in entries:
            x = e.ext.lower() or "other"
            by_ext[x] = by_ext.get(x, 0) + 1
        top_ext = sorted(by_ext.items(), key=lambda x: x[1], reverse=True)[:4]
        return {"total": total, "mapped": mapped, "unmapped": total-mapped,
                "snapped": snapped, "size_b": size_b, "top_ext": top_ext}


# ═══════════════════════════════════════════════════════════════════════
#  HELPERS
# ═══════════════════════════════════════════════════════════════════════

def file_addr(path: str, n: int = 20) -> Tuple[int, float]:
    h   = hashlib.sha256(Path(path).name.encode()).hexdigest()
    val = int(h[:8], 16) / 0xFFFFFFFF
    return int(val * (1 << n)), round(math.degrees(val * 2 * math.pi), 4)

def make_eid(path: str) -> str:
    return hashlib.md5(str(Path(path).resolve()).encode()).hexdigest()[:14]

def win_open(path: str):
    target = Path(path)
    os.startfile(str(target.parent if target.is_file() else target))

def size_str(b: int) -> str:
    for u in ("B","KB","MB","GB","TB"):
        if b < 1024: return f"{b:.1f} {u}"
        b //= 1024
    return f"{b} PB"


# ═══════════════════════════════════════════════════════════════════════
#  SIMULATION ENGINE  — ทำงานได้โดยไม่ต้องมี C process
#  แยกออกมาเป็น class เดียว เพื่อให้ Simulate tab เรียกได้ตรงๆ
# ═══════════════════════════════════════════════════════════════════════

class SimEngine:
    """
    Pure-Python simulation of POGLS Hydra + Audit state.
    ไม่ต้องมี pogls_controller.py — ทำงานได้ standalone.

    มี 2 callback channels:
      subscribe(cb)      → cb(frame_dict)     ทุก frame update  → Hydra tab
      subscribe_log(cb)  → cb(level, msg)     ทุก step detail   → Simulate tab log
    """

    # Log levels
    INFO     = "INFO"
    WARN     = "WARN"
    ERROR    = "ERROR"
    STEP     = "STEP"    # state transition
    RECOVER  = "RECOVER"
    OK       = "OK"

    def __init__(self):
        self._lock = threading.Lock()
        self._heads: List[dict] = []
        self._tiles: List[dict] = []
        self._audit_health = "OK"
        self._event_log: List[dict] = []
        self._frame_seq       = 0
        self._spawn_count     = 0
        self._retract_count   = 0
        self._incident_count  = 0
        self._total_scans     = 0
        self._total_anomalies = 0
        self._frame_callbacks: List = []
        self._log_callbacks:   List = []   # ← ใหม่: step-level log

        self._reset_to_default()
        self._running = True
        self._tick_thread = threading.Thread(target=self._tick_loop, daemon=True)
        self._tick_thread.start()

    # ── subscribe ────────────────────────────────────────────────────
    def subscribe(self, cb):      self._frame_callbacks.append(cb)
    def subscribe_log(self, cb):  self._log_callbacks.append(cb)

    def _push_log(self, level: str, msg: str):
        """Push step log to all log subscribers (thread-safe, non-blocking)"""
        entry = {"level": level, "msg": msg, "ts": time.time()}
        for cb in self._log_callbacks:
            try: cb(entry)
            except Exception: pass

    def _notify(self):
        frame = self.get_frame()
        for cb in self._frame_callbacks:
            try: cb(frame)
            except Exception: pass

    # ── state ────────────────────────────────────────────────────────
    def _reset_to_default(self):
        with self._lock:
            self._heads = [
                {"id": 0, "status": "ACTIVE",  "branch": 0xDEADBEEF0001,
                 "zone_start": 0,     "zone_end": 2<<20, "writes": 0, "anom": 0},
                {"id": 1, "status": "ACTIVE",  "branch": 0xDEADBEEF0002,
                 "zone_start": 6<<20, "zone_end": 8<<20, "writes": 0, "anom": 0},
            ]
            self._tiles = [
                {"index": i, "state": "CLEAN", "anomaly_flags": 0,
                 "blocks_scanned": 0, "blocks_anomalous": 0}
                for i in range(8)
            ]
            self._audit_health = "OK"
            self._event_log    = []

    def _tick_loop(self):
        import random
        while self._running:
            time.sleep(2.0)
            with self._lock:
                self._frame_seq  += 1
                self._total_scans += 1
                for tile in self._tiles:
                    tile["blocks_scanned"] += random.randint(0, 8)
                    if tile["state"] in ("CLEAN","IDLE"):
                        if random.random() < 0.05:
                            tile["state"] = "SCANNING"
                    elif tile["state"] == "SCANNING":
                        tile["state"] = random.choice(["CLEAN","CLEAN","CERTIFIED"])
                for h in self._heads:
                    if h["status"] == "ACTIVE":
                        h["writes"] += random.randint(1, 12)
            self._notify()

    def get_frame(self) -> dict:
        with self._lock:
            return {
                "frame_seq":      self._frame_seq,
                "audit_health":   self._audit_health,
                "active_heads":   sum(1 for h in self._heads if h["status"] == "ACTIVE"),
                "total_scans":    self._total_scans,
                "total_anomalies":self._total_anomalies,
                "spawn_count":    self._spawn_count,
                "retract_count":  self._retract_count,
                "incident_count": self._incident_count,
                "heads":          copy.deepcopy(self._heads),
                "tiles":          copy.deepcopy(self._tiles),
                "events":         list(self._event_log[-8:]),
                "is_stale":       False,
                "frame_age_ms":   0,
            }

    # ═══════════════════════════════════════════════════════════════════
    #  SIMULATION ACTIONS — แต่ละ action log ทุก step
    # ═══════════════════════════════════════════════════════════════════

    def sim_spawn(self, zone_start_mb: int = None, zone_end_mb: int = None):
        import random
        start = zone_start_mb if zone_start_mb is not None else random.randint(2, 28)
        end   = zone_end_mb   if zone_end_mb   is not None else start + 2

        with self._lock:
            if len(self._heads) >= 16:
                self._push_log(self.ERROR, "Spawn rejected — MAX_HEADS (16) reached")
                return "MAX HEADS (16) reached"
            new_id = max((h["id"] for h in self._heads), default=-1) + 1
            self._heads.append({
                "id": new_id, "status": "SPAWNING",
                "branch": int(time.time() * 1000) & 0xFFFFFFFFFFFF,
                "zone_start": start << 20, "zone_end": end << 20,
                "writes": 0, "anom": 0,
            })
            self._spawn_count += 1
            self._event_log.append({
                "type": "SPAWN", "severity": 0,
                "head_id": new_id, "ts": time.time(),
                "msg": f"Head[{new_id}] spawned zone {start}-{end}MB"
            })

        self._push_log(self.STEP,  f"Head[{new_id}]  DORMANT → SPAWNING")
        self._push_log(self.INFO,  f"  zone: {start}MB – {end}MB  ({end-start}MB)")
        self._push_log(self.INFO,  f"  trigger: zone density > 4MB threshold")
        self._push_log(self.INFO,  f"  genesis snapshot: PENDING")
        self._notify()

        def promote():
            time.sleep(0.4)
            self._push_log(self.INFO, f"  certifying genesis snapshot…")
            time.sleep(0.6)
            with self._lock:
                for h in self._heads:
                    if h["id"] == new_id and h["status"] == "SPAWNING":
                        h["status"] = "ACTIVE"
                self._event_log.append({
                    "type": "SPAWN_COMPLETE", "severity": 0,
                    "head_id": new_id, "ts": time.time(),
                    "msg": f"Head[{new_id}] ACTIVE"
                })
            self._push_log(self.STEP, f"Head[{new_id}]  SPAWNING → ACTIVE  ✓")
            self._push_log(self.OK,   f"  Head[{new_id}] ready to accept writes")
            self._notify()
        threading.Thread(target=promote, daemon=True).start()
        return f"Head[{new_id}] spawning → zone {start}-{end}MB"

    def sim_anomaly(self, head_id: int = None, critical: bool = False):
        import random
        with self._lock:
            active = [h for h in self._heads if h["status"] == "ACTIVE"]
            if not active:
                self._push_log(self.ERROR, "Anomaly skipped — no ACTIVE heads")
                return "No active heads"
            h = next((x for x in active if x["id"] == head_id), None) or random.choice(active)
            new_status = "MIGRATING" if critical else "SAFE"
            old_status = h["status"]
            h["status"] = new_status
            h["anom"]  += 1
            self._total_anomalies += 1
            self._incident_count  += 1
            if critical:
                self._audit_health = "DEGRADED"
            # Affect random tile
            affected_tile = None
            if self._tiles:
                affected_tile = random.choice(self._tiles)
                affected_tile["state"] = "ANOMALY"
                affected_tile["anomaly_flags"] |= 0x01
                affected_tile["blocks_anomalous"] += random.randint(1, 6)
            self._event_log.append({
                "type": "ANOMALY" if not critical else "CRITICAL",
                "severity": 2 if critical else 1,
                "head_id": h["id"], "ts": time.time(),
                "msg": f"Head[{h['id']}] {old_status} → {new_status}"
            })
            hid = h["id"]
            ti  = affected_tile["index"] if affected_tile else "?"

        flag_name = "HASH_MISMATCH" if not critical else "DEEP_UNREADABLE"
        self._push_log(self.WARN if not critical else self.ERROR,
                       f"{'Anomaly' if not critical else 'CRITICAL anomaly'} detected on Head[{hid}]")
        self._push_log(self.STEP, f"Head[{hid}]  {old_status} → {new_status}")
        self._push_log(self.INFO, f"  anomaly flag: {flag_name} (0x{'01' if not critical else '10'})")
        self._push_log(self.INFO, f"  Tile[{ti}] → ANOMALY  ({affected_tile['blocks_anomalous'] if affected_tile else 0} blocks)")
        if critical:
            self._push_log(self.ERROR, f"  Audit health: OK → DEGRADED")
            self._push_log(self.WARN,  f"  auto-promote BLOCKED while DEGRADED")
            self._push_log(self.INFO,  f"  action required: manual Recovery")
        else:
            self._push_log(self.INFO,  f"  SAFE mode: writes paused, reads OK")
            self._push_log(self.INFO,  f"  auto-recover scheduled in 3s…")
        self._notify()

        if not critical:
            def recover():
                time.sleep(1.5)
                self._push_log(self.INFO, f"  Audit re-scanning Tile[{ti}]…")
                time.sleep(1.0)
                with self._lock:
                    for hh in self._heads:
                        if hh["id"] == hid and hh["status"] == "SAFE":
                            hh["status"] = "ACTIVE"
                    # heal tile
                    for tile in self._tiles:
                        if tile.get("index") == ti:
                            tile["state"] = "CLEAN"
                            tile["anomaly_flags"] = 0
                    self._event_log.append({
                        "type": "RECOVERY", "severity": 0,
                        "head_id": hid, "ts": time.time(),
                        "msg": f"Head[{hid}] auto-recovered"
                    })
                self._push_log(self.STEP,    f"Head[{hid}]  SAFE → ACTIVE  (auto-recover)")
                self._push_log(self.RECOVER, f"  Tile[{ti}] → CLEAN")
                self._push_log(self.OK,      f"  Head[{hid}] back to normal operation")
                self._notify()
            threading.Thread(target=recover, daemon=True).start()

        sev = "CRITICAL → MIGRATING (manual recovery needed)" if critical else "→ SAFE (auto-recover in 3s)"
        return f"Head[{hid}] {sev}"

    def sim_retract(self, head_id: int = None):
        import random
        with self._lock:
            candidates = [h for h in self._heads
                          if h["status"] in ("ACTIVE","SAFE") and h["id"] != 0]
            if not candidates:
                self._push_log(self.ERROR, "Retract skipped — no retractable heads (Head[0] protected)")
                return "Cannot retract — need at least 1 other active head"
            h = next((x for x in candidates if x["id"] == head_id), None) or random.choice(candidates)
            h["status"] = "RETRACTING"
            self._retract_count += 1
            hid = h["id"]
            zone_mb = (h["zone_end"] - h["zone_start"]) >> 20
            self._event_log.append({
                "type": "RETRACT", "severity": 0,
                "head_id": hid, "ts": time.time(),
                "msg": f"Head[{hid}] retracting"
            })

        self._push_log(self.STEP, f"Head[{hid}]  ACTIVE → RETRACTING")
        self._push_log(self.INFO, f"  trigger: zone size {zone_mb}MB < 512KB threshold")
        self._push_log(self.INFO, f"  migrating remaining blocks to Core…")
        self._notify()

        def finish():
            time.sleep(0.8)
            self._push_log(self.INFO, f"  flushing write buffer…")
            time.sleep(0.7)
            with self._lock:
                self._heads = [hh for hh in self._heads if hh["id"] != hid]
                self._event_log.append({
                    "type": "RETRACT_DONE", "severity": 0,
                    "head_id": hid, "ts": time.time(),
                    "msg": f"Head[{hid}] removed"
                })
            self._push_log(self.STEP, f"Head[{hid}]  RETRACTING → DEAD  (removed)")
            self._push_log(self.OK,   f"  zone reclaimed, Core updated")
            self._notify()
        threading.Thread(target=finish, daemon=True).start()
        return f"Head[{hid}] retracting → removed in ~1.5s"

    def sim_certify(self):
        import random
        with self._lock:
            pending = [t for t in self._tiles if t["state"] in ("CLEAN","SCANNING")]
            if not pending:
                self._push_log(self.WARN, "Certify skipped — no CLEAN/SCANNING tiles")
                return "No tiles to certify"
            t = random.choice(pending)
            old_state = t["state"]
            t["state"] = "CERTIFIED"
            ti = t["index"]
            self._event_log.append({
                "type": "CERTIFY", "severity": 0,
                "head_id": 0xFF, "ts": time.time(),
                "msg": f"Tile[{ti}] certified"
            })

        self._push_log(self.STEP, f"Tile[{ti}]  {old_state} → CERTIFIED")
        self._push_log(self.INFO, f"  snapshot state: PENDING → CONFIRMED_CERTIFIED")
        self._push_log(self.INFO, f"  certified = immutable checkpoint (one-shot)")
        self._push_log(self.OK,   f"  Tile[{ti}] is now checkpoint-immune")
        self._notify()
        return f"Tile[{ti}] → CERTIFIED"

    def sim_recovery(self):
        self._push_log(self.INFO, "Manual recovery initiated…")
        with self._lock:
            healed_heads = []
            healed_tiles = []
            for h in self._heads:
                if h["status"] in ("MIGRATING","SAFE","RETRACTING"):
                    old = h["status"]
                    h["status"] = "ACTIVE"
                    healed_heads.append((h["id"], old))
            for tile in self._tiles:
                if tile["state"] == "ANOMALY":
                    tile["state"] = "CLEAN"
                    tile["anomaly_flags"] = 0
                    healed_tiles.append(tile["index"])
            old_health = self._audit_health
            self._audit_health = "OK"
            self._event_log.append({
                "type": "RECOVERY", "severity": 0,
                "head_id": 0xFF, "ts": time.time(),
                "msg": "Manual recovery complete"
            })

        for hid, old in healed_heads:
            self._push_log(self.STEP,    f"Head[{hid}]  {old} → ACTIVE")
        for ti in healed_tiles:
            self._push_log(self.RECOVER, f"Tile[{ti}]  ANOMALY → CLEAN")
        if old_health != "OK":
            self._push_log(self.STEP, f"Audit health  {old_health} → OK")
        if not healed_heads and not healed_tiles:
            self._push_log(self.INFO, "Nothing to recover — system already healthy")
        else:
            self._push_log(self.OK, f"Recovery done  ({len(healed_heads)} heads, {len(healed_tiles)} tiles)")
        self._notify()
        return f"Recovery complete — {len(healed_heads)} heads, {len(healed_tiles)} tiles healed"

    def sim_reset(self):
        self._push_log(self.INFO, "─" * 36)
        self._push_log(self.INFO, "State reset to default")
        self._reset_to_default()
        self._push_log(self.OK,   "Head[0] ACTIVE  zone 0–2MB")
        self._push_log(self.OK,   "Head[1] ACTIVE  zone 6–8MB")
        self._push_log(self.OK,   "8 tiles CLEAN  |  Audit OK")
        self._push_log(self.INFO, "─" * 36)
        self._notify()
        return "State reset to default"

    def stop(self):
        self._running = False


# ═══════════════════════════════════════════════════════════════════════
#  THEME + WIDGET FACTORY
# ═══════════════════════════════════════════════════════════════════════

class Theme:
    def __init__(self, cfg: dict):
        t = cfg["theme"]; f = cfg["font"]
        self.bg     = t["bg"];    self.panel  = t["panel"]
        self.card   = t["card"];  self.border = t["border"]
        self.amber  = t["amber"]; self.cyan   = t["cyan"]
        self.green  = t["green"]; self.red    = t["red"]
        self.blue   = t["blue"];  self.text   = t["text"]
        self.muted  = t["muted"]; self.hover  = t["hover"]
        self.sel    = t["sel"];   self.selfg  = t["selfg"]
        self.dim    = self._darken(t["border"])
        self.b2     = self._lighten(t["border"])
        fam = f["family"]
        self.logo = (fam, f["size_logo"], "bold")
        self.head = (fam, f["size_head"], "bold")
        self.ui   = (fam, f["size_ui"])
        self.sm   = (fam, f["size_sm"])
        self.xs   = (fam, f["size_xs"])
        self.tree = (fam, f["size_tree"])
        self.row_h = f["tree_row_height"]

    @staticmethod
    def _darken(c):
        try:
            r=max(0,int(c[1:3],16)-20); g=max(0,int(c[3:5],16)-20); b=max(0,int(c[5:7],16)-20)
            return f"#{r:02x}{g:02x}{b:02x}"
        except: return c

    @staticmethod
    def _lighten(c):
        try:
            r=min(255,int(c[1:3],16)+20); g=min(255,int(c[3:5],16)+20); b=min(255,int(c[5:7],16)+20)
            return f"#{r:02x}{g:02x}{b:02x}"
        except: return c


class W:
    def __init__(self, t: Theme): self.t = t

    def btn(self, parent, text, cmd, fg=None, font=None, px=10, py=5) -> tk.Button:
        t = self.t; fg_ = fg or t.amber
        b = tk.Button(parent, text=text, command=cmd,
                      bg=t.card, fg=fg_, font=font or t.sm,
                      relief="flat", bd=0, padx=px, pady=py,
                      activebackground=t.hover, activeforeground=fg_,
                      cursor="hand2")
        b.bind("<Enter>", lambda _: b.config(bg=t.hover))
        b.bind("<Leave>", lambda _: b.config(bg=t.card))
        return b

    def sep(self, parent, padx=8, pady=3):
        tk.Frame(parent, bg=self.t.border, height=1).pack(fill="x", padx=padx, pady=pady)

    def sec(self, parent, text):
        tk.Label(parent, text=text, bg=self.t.panel, fg=self.t.muted,
                 font=self.t.xs, padx=12).pack(anchor="w", pady=(9, 1))

    def label(self, parent, text, fg=None, font=None, **kw) -> tk.Label:
        t = self.t
        return tk.Label(parent, text=text, bg=t.bg,
                        fg=fg or t.text, font=font or t.sm, **kw)

    def card_frame(self, parent, **kw) -> tk.Frame:
        return tk.Frame(parent, bg=self.t.card,
                        highlightbackground=self.t.border,
                        highlightthickness=1, **kw)


# ═══════════════════════════════════════════════════════════════════════
#  SETTINGS DIALOG
# ═══════════════════════════════════════════════════════════════════════

class SettingsDialog(tk.Toplevel):
    def __init__(self, parent: "App"):
        super().__init__(parent)
        self.app = parent; self.cfg = copy.deepcopy(parent.cfg)
        t = parent.theme
        self.title("⚙  Settings"); self.geometry("560x560")
        self.configure(bg=t.bg); self.resizable(False, True); self.grab_set()
        self._vars: Dict[str, tk.Variable] = {}
        self._build(t)

    def _build(self, t):
        tk.Label(self, text="⚙  Settings", bg=t.bg, fg=t.amber,
                 font=t.head).pack(anchor="w", padx=18, pady=12)
        canvas = tk.Canvas(self, bg=t.bg, highlightthickness=0)
        sb = tk.Scrollbar(self, orient="vertical", command=canvas.yview,
                          bg=t.panel, troughcolor=t.panel)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y"); canvas.pack(fill="both", expand=True)
        body = tk.Frame(canvas, bg=t.bg)
        win_id = canvas.create_window((0, 0), window=body, anchor="nw")
        body.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(win_id, width=e.width))
        self._body(body, t)
        bf = tk.Frame(self, bg=t.panel, height=48); bf.pack(fill="x", side="bottom")
        bf.pack_propagate(False)
        self.app.w.btn(bf, "  ✓  Apply  ", self._apply, t.amber, t.ui).pack(side="right", padx=10, pady=10)
        self.app.w.btn(bf, "  Reset  ", self._reset, t.muted, t.sm).pack(side="right", padx=4, pady=10)
        self.app.w.btn(bf, "  Cancel  ", self.destroy, t.muted, t.sm).pack(side="right", padx=4, pady=10)

    def _body(self, parent, t):
        def section(txt):
            tk.Label(parent, text=txt, bg=t.bg, fg=t.amber,
                     font=t.sm).pack(anchor="w", padx=16, pady=(14, 2))
            tk.Frame(parent, bg=t.border, height=1).pack(fill="x", padx=16, pady=2)

        def row(label, var, widget_fn):
            f = tk.Frame(parent, bg=t.bg); f.pack(fill="x", padx=16, pady=4)
            tk.Label(f, text=label, bg=t.bg, fg=t.text, font=t.sm,
                     width=22, anchor="w").pack(side="left")
            widget_fn(f, var)

        def entry(f, var):
            tk.Entry(f, textvariable=var, bg=t.card, fg=t.text,
                     font=t.ui, relief="flat", bd=0, width=14,
                     insertbackground=t.amber).pack(side="left", padx=4)

        def spin(f, var, lo, hi):
            tk.Spinbox(f, textvariable=var, from_=lo, to=hi,
                       bg=t.card, fg=t.text, font=t.ui,
                       relief="flat", bd=0, width=8,
                       insertbackground=t.amber,
                       buttonbackground=t.card).pack(side="left", padx=4)

        def check(f, var):
            tk.Checkbutton(f, variable=var, bg=t.bg,
                           fg=t.text, selectcolor=t.card,
                           activebackground=t.bg).pack(side="left")

        def color_pick(f, var):
            def pick():
                c = colorchooser.askcolor(color=var.get(), parent=self)
                if c and c[1]: var.set(c[1])
            preview = tk.Label(f, bg=var.get(), width=4, relief="flat")
            preview.pack(side="left", padx=4, pady=2)
            var.trace_add("write", lambda *_: (lambda v=var, p=preview: p.config(bg=v.get()))())
            self.app.w.btn(f, " pick ", pick, t.muted, t.xs).pack(side="left")

        section("🖥  Window")
        for label, key in [("Width","width"),("Height","height")]:
            sv = tk.IntVar(value=self.cfg["window"][key])
            self._vars[f"window.{key}"] = sv
            row(label, sv, lambda f, v, a=400, b=2560: spin(f, v, a, b))

        section("🎨  Colors")
        for label, key in [("Background","bg"),("Panel","panel"),
                            ("Card","card"),("Accent amber","amber"),
                            ("Accent cyan","cyan"),("Text","text"),("Muted","muted")]:
            sv = tk.StringVar(value=self.cfg["theme"][key])
            self._vars[f"theme.{key}"] = sv
            row(label, sv, color_pick)

        section("⚙  Behavior")
        sv = tk.BooleanVar(value=self.cfg["behavior"]["confirm_delete"])
        self._vars["behavior.confirm_delete"] = sv
        row("Confirm delete", sv, check)
        for label, key, lo, hi in [
            ("Chunk size MB","chunk_size_mb",10,1000),
            ("Status timeout ms","status_timeout_ms",1000,30000)]:
            sv = tk.IntVar(value=self.cfg["behavior"][key])
            self._vars[f"behavior.{key}"] = sv
            row(label, sv, lambda f, v, a=lo, b=hi: spin(f, v, a, b))

    def _apply(self):
        for dotkey, var in self._vars.items():
            section, key = dotkey.split(".", 1)
            try: self.cfg[section][key] = var.get()
            except Exception: pass
        save_config(self.cfg); self.app.cfg = self.cfg
        self.app._reload_theme(); self.destroy()
        self.app.set_status("✅  Settings saved")

    def _reset(self):
        if messagebox.askyesno("Reset", "Reset ทุก setting กลับ default?"):
            self.cfg = copy.deepcopy(DEFAULT_CONFIG); save_config(self.cfg)
            self.app.cfg = self.cfg; self.app._reload_theme()
            self.destroy(); self.app.set_status("Settings reset")


# ═══════════════════════════════════════════════════════════════════════
#  MAIN APPLICATION
# ═══════════════════════════════════════════════════════════════════════

class App(_Base):

    def __init__(self):
        super().__init__()
        self.cfg   = load_config()
        self.theme = Theme(self.cfg)
        self.w     = W(self.theme)
        self.db    = Database(self.cfg["paths"]["database_file"])

        self.sel_eid    = None
        self._sort_col  = "added"
        self._folder_filter = None
        self._sort_asc  = False
        self._sb_rows:  List[Tuple] = []
        self._view_var  = tk.StringVar(value="all")
        self._view_mapped = False
        self._last_frame = None

        # Simulation engine (standalone, ไม่ต้องพึ่ง C)
        self._sim = SimEngine()
        self._vf_enabled = True   # เสมอ — ใช้ SimEngine

        # ลอง connect V31Adapter จริงถ้ามี
        self._adapter = None
        try:
            from pogls_controller import V31Adapter
            self._adapter = V31Adapter(poll_interval_ms=500)
            self._adapter.subscribe(self._on_visual_frame)
        except ImportError:
            self._sim.subscribe(self._on_visual_frame_dict)

        # SimEngine step-log → Simulate tab (เสมอ ไม่ว่าจะมี adapter หรือเปล่า)
        self._sim.subscribe_log(self._on_sim_log)

        t = self.theme
        self.title("◈  POGLS  V3.5  ·  Desktop Manager")
        self.geometry(f"{self.cfg['window']['width']}x{self.cfg['window']['height']}")
        self.minsize(self.cfg["window"]["min_width"], self.cfg["window"]["min_height"])
        self.configure(bg=t.bg)

        self._build_ui()
        self._setup_dnd()
        self.refresh()
        self._poll_visual()

    # ── helpers ─────────────────────────────────────────────────────────
    def _on_visual_frame(self, frame):    self._last_frame = ("vf", frame)
    def _on_visual_frame_dict(self, d):   self._last_frame = ("dict", d)

    def _on_sim_log(self, entry: dict):
        """Called from SimEngine background thread — schedule to main thread"""
        self.after(0, lambda e=entry: self._sim_log_push(e))

    def _get_frame_dict(self) -> Optional[dict]:
        """Always return a frame dict — from adapter or sim"""
        if self._adapter:
            try:
                f = self._adapter.poll_frame()
                return {
                    "frame_seq":      f.frame_seq,
                    "audit_health":   f.audit_health,
                    "active_heads":   f.active_heads,
                    "total_scans":    f.total_scans,
                    "total_anomalies":f.total_anomalies,
                    "spawn_count":    f.radar_spawn_count,
                    "retract_count":  f.radar_retract_count,
                    "incident_count": f.radar_incident_count,
                    "heads":          [{"id": h.head_id, "status": h.status,
                                        "branch": h.branch_id,
                                        "zone_start": h.zone_offset_start,
                                        "zone_end": h.zone_offset_end,
                                        "writes": h.write_count,
                                        "anom": h.anomaly_count}
                                       for h in f.heads],
                    "tiles":          [{"index": t.index, "state": t.state,
                                        "anomaly_flags": t.anomaly_flags,
                                        "blocks_scanned": t.blocks_scanned,
                                        "blocks_anomalous": t.blocks_anomalous}
                                       for t in f.tiles],
                    "events":         [{"type": e.type, "severity": e.severity,
                                        "head_id": e.head_id, "ts": 0, "msg": e.type}
                                       for e in f.events],
                    "is_stale":       f.is_stale,
                    "frame_age_ms":   f.frame_age_ms,
                }
            except Exception:
                pass
        return self._sim.get_frame()

    # ── theme ────────────────────────────────────────────────────────────
    def _reload_theme(self):
        self.theme = Theme(self.cfg); self.w = W(self.theme)
        self._apply_ttk_style()
        self.set_status("Theme updated")

    def _apply_ttk_style(self):
        t = self.theme; s = ttk.Style(self)
        s.configure("P.Treeview", background=t.card, foreground=t.text,
                    fieldbackground=t.card, borderwidth=0,
                    rowheight=t.row_h, font=t.tree)
        s.configure("P.Treeview.Heading", background=t.panel, foreground=t.muted,
                    font=(t.tree[0], t.tree[1], "bold"), relief="flat", borderwidth=0)
        s.map("P.Treeview", background=[("selected", t.sel)],
              foreground=[("selected", t.selfg)])
        # Tab style
        s.configure("App.TNotebook", background=t.bg, borderwidth=0, tabmargins=0)
        s.configure("App.TNotebook.Tab", background=t.panel, foreground=t.muted,
                    font=t.sm, padding=(16, 8), borderwidth=0)
        s.map("App.TNotebook.Tab",
              background=[("selected", t.bg), ("active", t.hover)],
              foreground=[("selected", t.amber), ("active", t.text)])

    # ═══════════════════════════════════════════════════════════════════
    #  BUILD UI
    # ═══════════════════════════════════════════════════════════════════

    def _build_ui(self):
        self._apply_ttk_style()
        self._build_topbar()
        # Tab container
        self._notebook = ttk.Notebook(self, style="App.TNotebook")
        self._notebook.pack(fill="both", expand=True, padx=0, pady=0)
        # Build 4 tabs
        self._tab_files   = self._make_tab("  ◈ Files  ")
        self._tab_hydra   = self._make_tab("  ⬡ Hydra  ")
        self._tab_sim     = self._make_tab("  🐉 Simulate  ")
        self._tab_ingest  = self._make_tab("  ⬇ Ingest  ")
        # Populate
        self._build_files_tab(self._tab_files)
        self._build_hydra_tab(self._tab_hydra)
        self._build_sim_tab(self._tab_sim)
        self._build_ingest_tab(self._tab_ingest)
        self._build_statusbar()

    def _make_tab(self, label: str) -> tk.Frame:
        t = self.theme
        frame = tk.Frame(self._notebook, bg=t.bg)
        self._notebook.add(frame, text=label)
        return frame

    # ── TOP BAR ─────────────────────────────────────────────────────────

    def _build_topbar(self):
        t   = self.theme
        bar = tk.Frame(self, bg=t.panel, height=52)
        bar.pack(fill="x"); bar.pack_propagate(False)
        tk.Label(bar, text="◈ POGLS  V3.5",
                 bg=t.panel, fg=t.amber, font=t.logo, padx=18).pack(side="left", pady=8)
        tk.Frame(bar, bg=t.border, width=1).pack(side="left", fill="y", pady=8)
        # Search
        sf = tk.Frame(bar, bg=t.card, padx=8)
        sf.pack(side="left", padx=14, pady=10, ipady=3)
        tk.Label(sf, text="⌕", bg=t.card, fg=t.muted, font=(t.ui[0], 13)).pack(side="left")
        self.q_var = tk.StringVar()
        self.q_var.trace_add("write", lambda *_: self.refresh())
        tk.Entry(sf, textvariable=self.q_var, bg=t.card, fg=t.text,
                 font=t.ui, insertbackground=t.amber, relief="flat", bd=0,
                 width=24).pack(side="left", padx=4)
        tk.Button(sf, text="✕", command=lambda: self.q_var.set(""),
                  bg=t.card, fg=t.muted, font=t.xs, relief="flat", bd=0,
                  padx=4, activebackground=t.card, activeforeground=t.amber,
                  cursor="hand2").pack(side="left")
        # Type filter
        self.ext_var = tk.StringVar(value="all")
        om = tk.OptionMenu(sf, self.ext_var, *EXT_LIST, command=lambda _: self.refresh())
        om.config(bg=t.card, fg=t.cyan, relief="flat", font=t.xs, bd=0,
                  highlightthickness=0, activebackground=t.hover)
        om["menu"].config(bg=t.card, fg=t.text, font=t.xs)
        om.pack(side="left")
        # Right buttons
        for txt, cmd, fg in [
            ("⚙", self.open_settings, t.muted),
            ("+ Folder", self.add_folder, t.cyan),
            ("+ Files",  self.add_files,  t.amber),
        ]:
            self.w.btn(bar, txt, cmd, fg=fg, font=t.sm, py=6).pack(
                side="right", padx=4, pady=10)
        # Live status pill (top-right)
        self._top_health = tk.Label(bar, text="● OK", bg=t.panel, fg=t.green,
                                    font=t.xs, padx=10)
        self._top_health.pack(side="right", pady=10)

    # ═══════════════════════════════════════════════════════════════════
    #  TAB 1 — FILES
    # ═══════════════════════════════════════════════════════════════════

    def _build_files_tab(self, parent):
        t = self.theme
        body = tk.Frame(parent, bg=t.bg)
        body.pack(fill="both", expand=True)
        self._build_sidebar(body)
        self._build_center(body)
        self._build_dock(body)

    # ── SIDEBAR ─────────────────────────────────────────────────────────
    def _build_sidebar(self, parent):
        t  = self.theme
        sb = tk.Frame(parent, bg=t.panel,
                      width=self.cfg["window"]["sidebar_width"])
        sb.pack(side="left", fill="y"); sb.pack_propagate(False)
        self.w.sec(sb, "VIEWS")
        for label, key in [
            ("◈  All Files",      "all"),
            ("◷  Has Snapshots",  "__snaps__"),
            ("◉  Mapped (DNA)",   "__mapped__"),
            ("◫  Safetensors",    ".safetensors"),
            ("◻  Checkpoints",    ".ckpt"),
            ("⬡  JSON / Config", ".json"),
            ("◱  Python",         ".py"),
            ("◲  Zip",            ".zip"),
        ]:
            self._sb_row(sb, label, key)
        self.w.sep(sb)
        self.w.sec(sb, "TOPO FILTER")
        self.topo_var = tk.IntVar(value=-1)
        for val, txt in [(-1, "All levels")] + list(enumerate(TOPO_NAMES)):
            tk.Radiobutton(sb, text=txt, variable=self.topo_var, value=val,
                           bg=t.panel, fg=t.muted, font=t.xs,
                           selectcolor=t.panel, activebackground=t.panel,
                           activeforeground=t.amber,
                           command=self.refresh).pack(anchor="w", padx=14, pady=1)
        self.w.sep(sb)
        self.w.sec(sb, "TOOLS")
        for txt, cmd in [
            ("🧬  DNA Scan",       self.dna_scan),
            ("⚡  Batch Import",   self.batch_import),
            ("⏱  Version History",self.show_history),
            ("🗑  Clear List",     self.clear_db),
            ("⬇  Export All",     self.export_all),
        ]:
            self.w.btn(sb, txt, cmd, font=t.xs, px=10, py=5).pack(
                fill="x", padx=8, pady=2)

        self.w.sep(sb)
        self.w.sec(sb, "FOLDERS")

        # Folder tree — 2 modes: DB-grouped (top) + filesystem browse (bottom)
        ftop = tk.Frame(sb, bg=t.panel); ftop.pack(fill="x", padx=8, pady=(0, 2))
        self.w.btn(ftop, "⬡  Browse Disk", self._folder_browse,
                   t.cyan, t.xs, px=8, py=3).pack(side="left")
        self.w.btn(ftop, "↺", self._folder_refresh_db,
                   t.muted, t.xs, px=6, py=3).pack(side="right")

        ft_frame = tk.Frame(sb, bg=t.panel)
        ft_frame.pack(fill="both", expand=True, padx=4)
        ft_sb = tk.Scrollbar(ft_frame, orient="vertical", bg=t.panel,
                             troughcolor=t.panel, width=8)
        self._folder_tree = ttk.Treeview(ft_frame, show="tree",
                                         style="P.Treeview",
                                         selectmode="browse",
                                         yscrollcommand=ft_sb.set)
        ft_sb.config(command=self._folder_tree.yview)
        ft_sb.pack(side="right", fill="y")
        self._folder_tree.pack(fill="both", expand=True)
        self._folder_tree.bind("<<TreeviewSelect>>", self._on_folder_select)
        self._folder_tree.tag_configure("folder", foreground=t.amber)
        self._folder_tree.tag_configure("fs",     foreground=t.cyan)
        self._folder_tree.tag_configure("empty",  foreground=t.dim)

        self.w.sep(sb)
        self.w.sec(sb, "DATABASE")
        self.db_stat = tk.StringVar(value="0 files")
        tk.Label(sb, textvariable=self.db_stat, bg=t.panel,
                 fg=t.cyan, font=t.xs, padx=14, justify="left").pack(anchor="w", pady=2)

        # Init folder tree from DB
        self._folder_refresh_db()

    def _sb_row(self, parent, label, key):
        t   = self.theme
        f   = tk.Frame(parent, bg=t.panel, cursor="hand2")
        lbl = tk.Label(f, text=label, bg=t.panel, fg=t.text,
                       font=t.sm, anchor="w", padx=14, pady=6)
        f.pack(fill="x"); lbl.pack(fill="x")
        self._sb_rows.append((f, lbl, key))
        def activate(*_):
            self._view_var.set(key)
            for fr, lb, _ in self._sb_rows:
                fr.config(bg=t.panel); lb.config(bg=t.panel)
            f.config(bg=t.sel); lbl.config(bg=t.sel)
            self.ext_var.set(key if key.startswith(".") else "all")
            self._view_mapped = (key == "__mapped__")
            self.refresh()
        def on_enter(_):
            if self._view_var.get() != key:
                f.config(bg=t.hover); lbl.config(bg=t.hover)
        def on_leave(_):
            bg = t.sel if self._view_var.get() == key else t.panel
            f.config(bg=bg); lbl.config(bg=bg)
        for w in (f, lbl):
            w.bind("<Button-1>", activate)
            w.bind("<Enter>", on_enter); w.bind("<Leave>", on_leave)

    # ── CENTER ───────────────────────────────────────────────────────────
    def _build_center(self, parent):
        t      = self.theme
        center = tk.Frame(parent, bg=t.bg)
        center.pack(side="left", fill="both", expand=True)
        # Drop banner
        self.drop_banner = tk.Label(
            center, text="  ⬇  ลาก files / folders มาวางที่นี่  ·  หรือกด  + Files  /  + Folder",
            bg=t.border, fg=t.muted, font=t.xs, anchor="w", padx=12, pady=6)
        self.drop_banner.pack(fill="x", padx=6, pady=(4, 0))
        # Tree
        fr = tk.Frame(center, bg=t.bg)
        fr.pack(fill="both", expand=True, padx=6, pady=4)
        cols = ("name","size","addr","topo","snaps","chunks","added","mapped")
        cfg  = {"name":("File Name",260,"w"),"size":("Size",72,"e"),
                "addr":("Angular Address",130,"e"),"topo":("Topology",104,"center"),
                "snaps":("Snaps",46,"center"),"chunks":("Chunks",54,"center"),
                "added":("Added",130,"center"),"mapped":("Mapped",56,"center")}
        self.tree = ttk.Treeview(fr, columns=cols, show="headings",
                                 style="P.Treeview", selectmode="extended")
        for col in cols:
            hdr, w, anc = cfg[col]
            self.tree.heading(col, text=hdr, anchor=anc,
                              command=lambda c=col: self._sort(c))
            self.tree.column(col, width=w, anchor=anc, minwidth=32)
        vsb = tk.Scrollbar(fr, orient="vertical", command=self.tree.yview,
                           bg=t.panel, troughcolor=t.panel,
                           activebackground=t.amber, width=12)
        self.tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y"); self.tree.pack(fill="both", expand=True)
        self.tree.tag_configure("mapped",  foreground=t.green)
        self.tree.tag_configure("snapped", foreground=t.cyan)
        self.tree.tag_configure("normal",  foreground=t.text)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        self.tree.bind("<Double-1>",          self._on_double)
        self.tree.bind("<Delete>",            lambda _: self.remove_sel())
        self.tree.bind("<Button-3>",          self._ctx_menu)
        self.tree.bind("<Control-a>",         self._select_all)
        # Action bar
        ab = tk.Frame(center, bg=t.panel, height=40)
        ab.pack(fill="x"); ab.pack_propagate(False)
        for txt, cmd, fg in [
            ("📸  Snapshot",  self.snap_sel,   t.amber),
            ("⏪  Restore",   self.restore_sel, t.cyan),
            ("🗜  Zip",       self.zip_sel,     t.muted),
            ("📂  Explorer",  self.open_sel,    t.muted),
            ("✕  Remove",    self.remove_sel,  t.red),
        ]:
            self.w.btn(ab, txt, cmd, fg=fg, font=t.xs, px=9, py=4).pack(
                side="left", padx=3, pady=5)
        self.sel_info = tk.Label(ab, text="", bg=t.panel, fg=t.muted, font=t.xs)
        self.sel_info.pack(side="right", padx=12)
        # Stats bar
        self.stats_bar = tk.Frame(center, bg=t.panel, height=26)
        self.stats_bar.pack(fill="x"); self.stats_bar.pack_propagate(False)
        self.stats_var  = tk.StringVar(value="")
        self.mapped_var = tk.StringVar(value="")
        self.heads_var  = tk.StringVar(value="")
        self.audit_var  = tk.StringVar(value="")
        for var, fg, side in [
            (self.stats_var,  t.muted, "left"),
            (self.mapped_var, t.green, "right"),
            (self.heads_var,  t.cyan,  "right"),
        ]:
            tk.Label(self.stats_bar, textvariable=var, bg=t.panel,
                     fg=fg, font=t.xs, padx=8).pack(side=side, pady=3)
        self.audit_lbl = tk.Label(self.stats_bar, textvariable=self.audit_var,
                                  bg=t.panel, fg=t.green, font=t.xs, padx=8)
        self.audit_lbl.pack(side="right", pady=3)

    # ── DOCK ─────────────────────────────────────────────────────────────
    def _build_dock(self, parent):
        t    = self.theme
        dock = tk.Frame(parent, bg=t.panel,
                        width=self.cfg["window"]["dock_width"])
        dock.pack(side="right", fill="y"); dock.pack_propagate(False)
        tk.Label(dock, text="FILE DETAIL", bg=t.panel, fg=t.muted,
                 font=t.xs, padx=14).pack(anchor="w", pady=(12, 2))
        self.w.sep(dock)
        self._dv: Dict[str, tk.StringVar] = {}
        for label, key in [
            ("Name",   "name"),   ("Folder", "folder"),
            ("Size",   "size"),   ("Ext",    "ext"),
            ("Chunks", "chunks"), ("Addr",   "addr"),
            ("θ",      "theta"),  ("Topo",   "topo"),
            ("Added",  "added"),  ("Snaps",  "snaps"),
            ("Mapped", "mapped"),
            ("Head",   "head_id"),("Branch", "branch_id"),
            ("State",  "snap_state"),("Mode","write_mode"),
        ]:
            row = tk.Frame(dock, bg=t.panel); row.pack(fill="x", padx=12, pady=2)
            tk.Label(row, text=f"{label}:", bg=t.panel, fg=t.muted,
                     font=t.xs, width=7, anchor="w").pack(side="left")
            sv = tk.StringVar(value="—"); self._dv[key] = sv
            color = (t.green if key == "mapped"     else
                     t.cyan  if key == "head_id"    else
                     t.amber if key == "snap_state" else t.text)
            tk.Label(row, textvariable=sv, bg=t.panel, fg=color,
                     font=t.xs, wraplength=185,
                     justify="left", anchor="w").pack(side="left")
        self.w.sep(dock)
        # ── IMAGE PREVIEW ──────────────────────────────────────────────
        tk.Label(dock, text="PREVIEW", bg=t.panel, fg=t.muted,
                 font=t.xs, padx=14).pack(anchor="w", pady=(6, 2))
        self._preview_canvas = tk.Canvas(
            dock, bg=t.card, width=260, height=160,
            highlightthickness=0, cursor="hand2")
        self._preview_canvas.pack(padx=12, pady=(0, 4))
        self._preview_img_ref = None   # keep PhotoImage reference
        self._preview_canvas.bind("<Button-1>", lambda _: self.open_sel())
        self._draw_preview_placeholder()

        self.w.sep(dock)
        tk.Label(dock, text="ANGULAR ADDRESS", bg=t.panel, fg=t.muted,
                 font=t.xs, padx=14).pack(anchor="w", pady=(6, 2))
        self.wheel = tk.Canvas(dock, bg=t.card, width=260, height=148,
                               highlightthickness=0)
        self.wheel.pack(padx=12, pady=4)
        self._draw_wheel(0)
        self.w.sep(dock)
        tk.Label(dock, text="VERSION SNAPSHOTS", bg=t.panel, fg=t.muted,
                 font=t.xs, padx=14).pack(anchor="w", pady=(6, 2))
        snf = tk.Frame(dock, bg=t.panel); snf.pack(fill="both", expand=True, padx=8)
        snap_sb = tk.Scrollbar(snf, orient="vertical", bg=t.panel,
                               troughcolor=t.panel, width=10)
        self.snap_lb = tk.Listbox(snf, bg=t.card, fg=t.text, font=t.xs,
                                  selectbackground=t.sel, selectforeground=t.selfg,
                                  activestyle="none", relief="flat", bd=0,
                                  highlightthickness=0, yscrollcommand=snap_sb.set)
        snap_sb.config(command=self.snap_lb.yview)
        snap_sb.pack(side="right", fill="y"); self.snap_lb.pack(fill="both", expand=True, pady=4)
        self.snap_lb.bind("<Double-1>", lambda _: self.restore_sel())
        sbf = tk.Frame(dock, bg=t.panel); sbf.pack(fill="x", padx=8, pady=6)
        self.w.btn(sbf, "📸  New Snap", self.snap_sel, t.amber, t.xs).pack(side="left")
        self.w.btn(sbf, "⏪  Restore",  self.restore_sel, t.cyan, t.xs).pack(side="right")

    # ═══════════════════════════════════════════════════════════════════
    #  TAB 2 — HYDRA LIVE VIEW
    # ═══════════════════════════════════════════════════════════════════

    def _build_hydra_tab(self, parent):
        t = self.theme
        # Header row
        hdr = tk.Frame(parent, bg=t.panel, height=44)
        hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr, text="⬡  Hydra  ·  Audit  ·  VisualFeed",
                 bg=t.panel, fg=t.cyan, font=t.head, padx=16).pack(side="left", pady=10)
        self._hydra_seq_var = tk.StringVar(value="frame #0")
        tk.Label(hdr, textvariable=self._hydra_seq_var,
                 bg=t.panel, fg=t.muted, font=t.xs, padx=12).pack(side="right", pady=10)
        self._hydra_stale = tk.Label(hdr, text="", bg=t.panel, fg=t.amber, font=t.xs, padx=8)
        self._hydra_stale.pack(side="right", pady=10)

        body = tk.Frame(parent, bg=t.bg)
        body.pack(fill="both", expand=True, padx=12, pady=8)

        # ── Row 1: Audit health + counters ───────────────────────────
        row1 = tk.Frame(body, bg=t.bg); row1.pack(fill="x", pady=(0, 8))
        cards = [
            ("AUDIT HEALTH", "OK", t.green, "_hv_audit"),
            ("ACTIVE HEADS", "0",  t.cyan,  "_hv_heads"),
            ("TOTAL SCANS",  "0",  t.text,  "_hv_scans"),
            ("ANOMALIES",    "0",  t.amber, "_hv_anom"),
            ("SPAWNS",       "0",  t.cyan,  "_hv_spawn"),
            ("INCIDENTS",    "0",  t.red,   "_hv_inc"),
        ]
        for label, init, col, attr in cards:
            cf = tk.Frame(row1, bg=t.card, padx=14, pady=10,
                          highlightbackground=t.border, highlightthickness=1)
            cf.pack(side="left", fill="x", expand=True, padx=4)
            tk.Label(cf, text=label, bg=t.card, fg=t.muted, font=t.xs).pack(anchor="w")
            sv = tk.StringVar(value=init)
            tk.Label(cf, textvariable=sv, bg=t.card, fg=col,
                     font=(t.head[0], 18, "bold")).pack(anchor="w", pady=(2, 0))
            setattr(self, attr, sv)

        # ── Row 2: Head chips ─────────────────────────────────────────
        r2 = tk.Frame(body, bg=t.card, padx=12, pady=10,
                      highlightbackground=t.border, highlightthickness=1)
        r2.pack(fill="x", pady=(0, 8))
        tk.Label(r2, text="HYDRA HEADS  (green=ACTIVE · amber=SPAWNING · red=SAFE/MIGRATING)",
                 bg=t.card, fg=t.muted, font=t.xs).pack(anchor="w", pady=(0, 6))
        head_grid = tk.Frame(r2, bg=t.card); head_grid.pack(anchor="w")
        self._hchips: List[tk.Label] = []
        for i in range(16):
            lbl = tk.Label(head_grid, text=f"{i}", width=4, pady=6,
                           bg=t.panel, fg=t.dim, font=t.xs, relief="flat")
            lbl.grid(row=0, column=i, padx=2)
            self._hchips.append(lbl)

        # ── Row 3: Tile grid ──────────────────────────────────────────
        r3 = tk.Frame(body, bg=t.card, padx=12, pady=10,
                      highlightbackground=t.border, highlightthickness=1)
        r3.pack(fill="x", pady=(0, 8))
        tk.Label(r3, text="AUDIT TILES  (# CLEAN · ~ SCANNING · ! ANOMALY · ★ CERTIFIED · · IDLE)",
                 bg=t.card, fg=t.muted, font=t.xs).pack(anchor="w", pady=(0, 6))
        tile_row = tk.Frame(r3, bg=t.card); tile_row.pack(anchor="w")
        self._tsquares: List[tk.Label] = []
        for i in range(16):
            sq = tk.Label(tile_row, text="·", width=3, pady=8,
                          bg=t.panel, fg=t.dim, font=t.xs, relief="flat")
            sq.grid(row=0, column=i, padx=2)
            self._tsquares.append(sq)

        # ── Row 4: Event log ──────────────────────────────────────────
        r4 = tk.Frame(body, bg=t.card, padx=12, pady=8,
                      highlightbackground=t.border, highlightthickness=1)
        r4.pack(fill="both", expand=True)
        tk.Label(r4, text="RECENT EVENTS", bg=t.card, fg=t.muted,
                 font=t.xs).pack(anchor="w", pady=(0, 4))
        elf = tk.Frame(r4, bg=t.card); elf.pack(fill="both", expand=True)
        el_sb = tk.Scrollbar(elf, orient="vertical", bg=t.card,
                             troughcolor=t.card, width=8)
        self._event_lb = tk.Listbox(
            elf, bg=t.bg, fg=t.text, font=t.xs,
            selectbackground=t.sel, activestyle="none",
            relief="flat", bd=0, highlightthickness=0,
            yscrollcommand=el_sb.set)
        el_sb.config(command=self._event_lb.yview)
        el_sb.pack(side="right", fill="y")
        self._event_lb.pack(fill="both", expand=True)

    # ═══════════════════════════════════════════════════════════════════
    #  TAB 3 — SIMULATE
    # ═══════════════════════════════════════════════════════════════════

    def _build_sim_tab(self, parent):
        t = self.theme

        # Header
        hdr = tk.Frame(parent, bg=t.panel, height=44)
        hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr, text="🐉  Simulation Lab",
                 bg=t.panel, fg=t.amber, font=t.head, padx=16).pack(side="left", pady=10)
        tk.Label(hdr, text="กด Scenario เพื่อดูว่าระบบรับมือยังไง  ·  ไม่แตะ Core จริง",
                 bg=t.panel, fg=t.muted, font=t.xs, padx=12).pack(side="right", pady=10)

        body = tk.Frame(parent, bg=t.bg)
        body.pack(fill="both", expand=True)

        # ── LEFT: 2 columns — Scenarios + Manual ────────────────────
        left = tk.Frame(body, bg=t.bg, width=300)
        left.pack(side="left", fill="y", padx=12, pady=10)
        left.pack_propagate(False)

        # SECTION: Scenarios
        tk.Label(left, text="SCENARIOS", bg=t.bg, fg=t.amber,
                 font=t.xs).pack(anchor="w", pady=(0, 4))

        scenarios = [
            ("⚠  Anomaly → auto-recover",
             "head ผิดปกติ → SAFE → กลับ ACTIVE\nดู: anomaly flag + tile + 3s recovery",
             self._scenario_anomaly, t.amber),
            ("❌  Critical → manual fix",
             "MIGRATING + DEGRADED + ไม่ auto-recover\nดู: ระบบ freeze + ต้องกด Recovery",
             self._scenario_critical, t.red),
            ("🐉  Zone overflow → spawn",
             "density เกิน threshold → spawn head ใหม่\nดู: SPAWNING → ACTIVE lifecycle",
             self._scenario_spawn, t.cyan),
            ("↩  Zone shrink → retract",
             "zone < 512KB → retract head\nดู: RETRACTING → ลบออก + reclaim",
             self._scenario_retract, t.muted),
            ("💥  Cascade failure",
             "anomaly หลาย head พร้อมกัน\nดู: DEGRADED + partial recovery",
             self._scenario_cascade, t.red),
        ]

        for title, desc, cmd, col in scenarios:
            cf = tk.Frame(left, bg=t.card, padx=12, pady=8,
                          highlightbackground=t.border, highlightthickness=1)
            cf.pack(fill="x", pady=3)
            self.w.btn(cf, f"  ▶  {title}  ", cmd, col, t.xs).pack(anchor="w")
            tk.Label(cf, text=desc, bg=t.card, fg=t.muted,
                     font=t.xs, wraplength=240, justify="left").pack(anchor="w", pady=(3, 0))

        # SECTION: Manual
        tk.Frame(left, bg=t.border, height=1).pack(fill="x", pady=(10, 6))
        tk.Label(left, text="MANUAL", bg=t.bg, fg=t.muted,
                 font=t.xs).pack(anchor="w", pady=(0, 4))

        manual_btns = [
            ("🐉 Spawn",         self._do_spawn,                   t.cyan),
            ("⚠ Anomaly",        lambda: self._do_anomaly(False),  t.amber),
            ("❌ Critical",       lambda: self._do_anomaly(True),   t.red),
            ("↩ Retract",        self._do_retract,                 t.muted),
            ("★ Certify Tile",   self._do_certify,                 t.green),
            ("🔄 Recovery",      self._do_recovery,                t.cyan),
        ]
        btn_row = None
        for i, (lbl, cmd, col) in enumerate(manual_btns):
            if i % 2 == 0:
                btn_row = tk.Frame(left, bg=t.bg)
                btn_row.pack(fill="x", pady=2)
            self.w.btn(btn_row, lbl, cmd, col, t.xs, px=8, py=4).pack(
                side="left", padx=(0, 4))

        # Manual spawn zone
        zf = tk.Frame(left, bg=t.card, padx=10, pady=8,
                      highlightbackground=t.border, highlightthickness=1)
        zf.pack(fill="x", pady=(6, 0))
        tk.Label(zf, text="Spawn zone (MB)", bg=t.card, fg=t.muted,
                 font=t.xs).pack(anchor="w", pady=(0, 4))
        zr = tk.Frame(zf, bg=t.card); zr.pack(fill="x")
        tk.Label(zr, text="start", bg=t.card, fg=t.dim, font=t.xs).pack(side="left")
        self._sim_zone_start = tk.IntVar(value=10)
        tk.Spinbox(zr, textvariable=self._sim_zone_start, from_=0, to=1000,
                   bg=t.bg, fg=t.text, font=t.xs, relief="flat", width=5,
                   buttonbackground=t.card).pack(side="left", padx=4)
        tk.Label(zr, text="end", bg=t.card, fg=t.dim, font=t.xs).pack(side="left")
        self._sim_zone_end = tk.IntVar(value=12)
        tk.Spinbox(zr, textvariable=self._sim_zone_end, from_=0, to=1000,
                   bg=t.bg, fg=t.text, font=t.xs, relief="flat", width=5,
                   buttonbackground=t.card).pack(side="left", padx=4)
        self.w.btn(zf, "  ▶  Spawn  ", self._do_spawn_manual,
                   t.cyan, t.xs).pack(anchor="w", pady=(6, 0))

        # Reset
        tk.Frame(left, bg=t.border, height=1).pack(fill="x", pady=(10, 6))
        self.w.btn(left, "  ↺  Reset State  ", self._do_reset,
                   t.muted, t.xs).pack(anchor="w")

        # ── RIGHT: Log ──────────────────────────────────────────────
        right = tk.Frame(body, bg=t.bg)
        right.pack(side="left", fill="both", expand=True, padx=(0, 12), pady=10)

        log_hdr = tk.Frame(right, bg=t.bg)
        log_hdr.pack(fill="x", pady=(0, 4))
        tk.Label(log_hdr, text="SIMULATION LOG  —  state transitions · anomaly response · recovery",
                 bg=t.bg, fg=t.muted, font=t.xs).pack(side="left")
        self.w.btn(log_hdr, "  Clear  ", self._sim_log_clear,
                   t.muted, t.xs).pack(side="right")

        log_f = tk.Frame(right, bg=t.card, highlightbackground=t.border,
                         highlightthickness=1)
        log_f.pack(fill="both", expand=True)
        log_sb = tk.Scrollbar(log_f, orient="vertical", bg=t.card,
                              troughcolor=t.card, width=8)
        log_sb.pack(side="right", fill="y")
        # สร้าง Text ตรงใน log_f (ไม่ใช้ pre-created widget)
        self._sim_log_text = tk.Text(
            log_f, bg=t.bg, fg=t.text,
            font=("Consolas", 9),
            relief="flat", bd=0, highlightthickness=0,
            state="disabled", wrap="word",
            yscrollcommand=log_sb.set,
            insertbackground=t.amber,
            selectbackground=t.sel)
        log_sb.config(command=self._sim_log_text.yview)
        self._sim_log_text.pack(fill="both", expand=True, padx=6, pady=4)
        # Color tags
        tl = self._sim_log_text
        tl.tag_configure("STEP",    foreground=t.cyan,  font=("Consolas", 9, "bold"))
        tl.tag_configure("OK",      foreground=t.green, font=("Consolas", 9))
        tl.tag_configure("WARN",    foreground=t.amber, font=("Consolas", 9))
        tl.tag_configure("ERROR",   foreground=t.red,   font=("Consolas", 9, "bold"))
        tl.tag_configure("RECOVER", foreground=t.green, font=("Consolas", 9, "bold"))
        tl.tag_configure("INFO",    foreground=t.muted, font=("Consolas", 9))
        tl.tag_configure("TS",      foreground=t.dim,   font=("Consolas", 8))
        tl.tag_configure("DIV",     foreground=t.border, font=("Consolas", 8))
        tl.tag_configure("HDR",     foreground=t.amber, font=("Consolas", 9, "bold"))

        # Legend bar
        leg = tk.Frame(right, bg=t.panel, height=22)
        leg.pack(fill="x"); leg.pack_propagate(False)
        legend = [("STEP", t.cyan), ("OK", t.green), ("WARN", t.amber),
                  ("ERROR", t.red), ("RECOVER", t.green), ("INFO", t.muted)]
        for lname, lcol in legend:
            tk.Label(leg, text=f"■ {lname}", bg=t.panel, fg=lcol,
                     font=("Consolas", 8)).pack(side="left", padx=6, pady=3)

        # Init message
        self._sim_log_write("DIV", "─" * 52 + "\n")
        self._sim_log_write("INFO", "  SimEngine พร้อมแล้ว\n")
        self._sim_log_write("INFO", "  กด Scenario เพื่อดูว่าระบบตอบสนองยังไง\n")
        self._sim_log_write("INFO", "  หรือกด Manual เพื่อ trigger ทีละ action\n")
        self._sim_log_write("DIV", "─" * 52 + "\n")

    def _sim_log_write(self, tag: str, text: str):
        """Write to sim log Text widget — must be called from main thread"""
        tl = self._sim_log_text
        tl.config(state="normal")
        tl.insert("end", text, tag)
        tl.config(state="disabled")
        tl.see("end")

    def _sim_log_push(self, entry: dict):
        """Called in main thread via after() — render one log entry with color"""
        level = entry.get("level", "INFO")
        msg   = entry.get("msg", "")
        ts    = datetime.fromtimestamp(entry.get("ts", 0)).strftime("%H:%M:%S")
        # timestamp dim, then message in level color
        self._sim_log_write("TS",  f"  {ts}  ")
        self._sim_log_write(level, f"{msg}\n")

    def _sim_log_add(self, msg: str, level: str = "INFO"):
        """Direct add from main thread (for init messages)"""
        ts = datetime.now().strftime("%H:%M:%S")
        self._sim_log_write("TS",   f"  {ts}  ")
        self._sim_log_write(level,  f"{msg}\n")

    def _sim_log_clear(self):
        tl = self._sim_log_text
        tl.config(state="normal")
        tl.delete("1.0", "end")
        tl.config(state="disabled")

    def _do_spawn(self):
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_spawn()
        self.set_status(f"🐉  {result}")

    def _do_spawn_manual(self):
        s = self._sim_zone_start.get(); e = self._sim_zone_end.get()
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_spawn(zone_start_mb=s, zone_end_mb=e)
        self.set_status(f"🐉  {result}")

    def _do_anomaly(self, critical=False):
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_anomaly(critical=critical)
        icon = "❌" if critical else "⚠"
        self.set_status(f"{icon}  {result}")

    def _do_retract(self):
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_retract()
        self.set_status(f"↩  {result}")

    def _do_certify(self):
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_certify()
        self.set_status(f"★  {result}")

    def _do_recovery(self):
        self._sim_log_write("DIV", "─" * 48 + "\n")
        result = self._sim.sim_recovery()
        self.set_status(f"🔄  {result}")

    def _on_sim_log(self, entry: dict):
        """Called from SimEngine background thread — schedule to main thread"""
        try:
            self.after(0, lambda e=entry: self._sim_log_push(e))
        except Exception:
            pass  # app closing

    # ── Scenario runners — รัน multi-step story ผ่าน threading ─────────

    def _scenario_anomaly(self):
        """Scenario: anomaly → SAFE → auto-recover"""
        def run():
            self.after(0, lambda: self._sim_log_write("HDR",
                "\n▶  SCENARIO: Anomaly → Auto-Recovery\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  สถานการณ์: head ตรวจพบ block hash mismatch\n"))
            self.after(0, lambda: self._sim_log_write("DIV", "─"*52+"\n"))
            time.sleep(0.3)
            self._sim.sim_anomaly(critical=False)
            # log เพิ่มเติมจะมาจาก SimEngine subscribe_log อัตโนมัติ
        threading.Thread(target=run, daemon=True).start()

    def _scenario_critical(self):
        """Scenario: critical anomaly → MIGRATING → manual recovery needed"""
        def run():
            self.after(0, lambda: self._sim_log_write("HDR",
                "\n▶  SCENARIO: Critical Anomaly → Manual Recovery\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  สถานการณ์: deep block unreadable — ระบบ freeze\n"))
            self.after(0, lambda: self._sim_log_write("DIV", "─"*52+"\n"))
            time.sleep(0.3)
            self._sim.sim_anomaly(critical=True)
            time.sleep(1.5)
            self.after(0, lambda: self._sim_log_write("WARN",
                "\n  ⚠  ระบบรอ manual recovery — กด 🔄 Recovery เพื่อแก้\n"))
        threading.Thread(target=run, daemon=True).start()

    def _scenario_spawn(self):
        """Scenario: zone overflow → spawn new head"""
        def run():
            self.after(0, lambda: self._sim_log_write("HDR",
                "\n▶  SCENARIO: Zone Overflow → Spawn Head\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  สถานการณ์: zone density เกิน 4MB threshold\n"))
            self.after(0, lambda: self._sim_log_write("DIV", "─"*52+"\n"))
            time.sleep(0.3)
            self._sim.sim_spawn()
        threading.Thread(target=run, daemon=True).start()

    def _scenario_retract(self):
        """Scenario: zone shrink → retract"""
        def run():
            self.after(0, lambda: self._sim_log_write("HDR",
                "\n▶  SCENARIO: Zone Shrink → Retract Head\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  สถานการณ์: zone < 512KB ไม่คุ้มเก็บ head ไว้\n"))
            self.after(0, lambda: self._sim_log_write("DIV", "─"*52+"\n"))
            time.sleep(0.3)
            self._sim.sim_retract()
        threading.Thread(target=run, daemon=True).start()

    def _scenario_cascade(self):
        """Scenario: cascade failure across multiple heads"""
        def run():
            self.after(0, lambda: self._sim_log_write("HDR",
                "\n▶  SCENARIO: Cascade Failure\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  สถานการณ์: anomaly กระจาย → หลาย head พร้อมกัน\n"))
            self.after(0, lambda: self._sim_log_write("DIV", "─"*52+"\n"))
            # 1. spawn extra head ก่อน
            time.sleep(0.3)
            self._sim.sim_spawn()
            time.sleep(1.4)   # รอ ACTIVE
            # 2. anomaly head แรก
            self.after(0, lambda: self._sim_log_write("WARN",
                "\n  ── wave 1: head[0] anomaly ──\n"))
            self._sim.sim_anomaly(head_id=0, critical=False)
            time.sleep(0.8)
            # 3. critical head ที่สอง
            self.after(0, lambda: self._sim_log_write("ERROR",
                "\n  ── wave 2: critical on another head ──\n"))
            self._sim.sim_anomaly(critical=True)
            time.sleep(1.0)
            self.after(0, lambda: self._sim_log_write("ERROR",
                "\n  ❌  Cascade complete — DEGRADED\n"))
            self.after(0, lambda: self._sim_log_write("INFO",
                "  กด 🔄 Recovery เพื่อ restore ทุก head\n"))
        threading.Thread(target=run, daemon=True).start()

    def _do_reset(self):
        if messagebox.askyesno("Reset State", "Reset simulation state กลับ default?"):
            result = self._sim.sim_reset()
            self.set_status(f"↺  {result}")

    # ═══════════════════════════════════════════════════════════════════
    #  TAB 4 — INGEST
    # ═══════════════════════════════════════════════════════════════════

    def _build_ingest_tab(self, parent):
        t = self.theme
        hdr = tk.Frame(parent, bg=t.panel, height=44)
        hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr, text="⬇  Ingest  ·  Chat History  ·  POGLS WAL",
                 bg=t.panel, fg=t.cyan, font=t.head, padx=16).pack(side="left", pady=10)
        tk.Label(hdr, text="import chat exports → WAL → ค้นหาได้",
                 bg=t.panel, fg=t.muted, font=t.xs, padx=12).pack(side="right", pady=10)

        body = tk.Frame(parent, bg=t.bg)
        body.pack(fill="both", expand=True, padx=14, pady=10)

        left = tk.Frame(body, bg=t.bg); left.pack(side="left", fill="y", padx=(0, 10))
        right= tk.Frame(body, bg=t.bg); right.pack(side="left", fill="both", expand=True)

        # Import sources
        def src_card(title, subtitle, cmd, col):
            cf = tk.Frame(left, bg=t.card, padx=14, pady=12,
                          highlightbackground=t.border, highlightthickness=1)
            cf.pack(fill="x", pady=5)
            tk.Label(cf, text=title, bg=t.card, fg=t.text, font=t.sm).pack(anchor="w")
            tk.Label(cf, text=subtitle, bg=t.card, fg=t.muted, font=t.xs,
                     wraplength=230, justify="left").pack(anchor="w", pady=(2, 6))
            self.w.btn(cf, "  ▶  Import  ", cmd, col, t.xs).pack(anchor="w")

        src_card("Claude Export",
                 "claude.ai → Settings → Export\nformat: claude_export.json",
                 lambda: self._ingest_chat("claude"), t.amber)
        src_card("ChatGPT Export",
                 "chatgpt.com → Settings → Data controls\nformat: conversations.json",
                 lambda: self._ingest_chat("chatgpt"), t.green)
        src_card("Gemini Takeout",
                 "Google Takeout → Gemini\nformat: JSON",
                 lambda: self._ingest_chat("gemini"), t.blue)
        src_card("Any File / Folder",
                 "Text, Markdown, JSONL, PDF\ndetect format อัตโนมัติ",
                 self._ingest_any, t.cyan)

        self.w.sep(left); self.w.sec(left, "VAULT")
        self._vault_stat = tk.StringVar(value="no vault")
        tk.Label(left, textvariable=self._vault_stat, bg=t.bg, fg=t.cyan,
                 font=t.xs, padx=0, justify="left").pack(anchor="w", pady=2)
        self.w.btn(left, "  Export CSV  ", self._export_ingest_csv,
                   t.muted, t.xs).pack(anchor="w", pady=(8, 0))

        # Result log
        tk.Label(right, text="INGEST LOG", bg=t.bg, fg=t.muted,
                 font=t.xs).pack(anchor="w", pady=(0, 4))
        log_f = tk.Frame(right, bg=t.card, highlightbackground=t.border,
                         highlightthickness=1)
        log_f.pack(fill="both", expand=True)
        log_sb = tk.Scrollbar(log_f, orient="vertical", bg=t.card,
                              troughcolor=t.card, width=8)
        self._ingest_log = tk.Listbox(
            log_f, bg=t.bg, fg=t.text, font=("Consolas", 9),
            selectbackground=t.sel, activestyle="none",
            relief="flat", bd=0, highlightthickness=0,
            yscrollcommand=log_sb.set)
        log_sb.config(command=self._ingest_log.yview)
        log_sb.pack(side="right", fill="y")
        self._ingest_log.pack(fill="both", expand=True)

        self._ingest_log_add("─" * 40)
        self._ingest_log_add("  Ingest ready")
        self._ingest_log_add("  ต้องมี pogls_ingest.py ใน folder เดียวกัน")
        self._ingest_log_add("─" * 40)
        self._ingestor = None

    def _ingest_log_add(self, msg):
        ts = datetime.now().strftime("%H:%M:%S")
        self._ingest_log.insert("end", f"  {ts}  {msg}")
        self._ingest_log.see("end")

    def _get_ingestor(self):
        if self._ingestor: return self._ingestor
        try:
            from pogls_ingest import POGLSIngestor
            self._ingestor = POGLSIngestor("pogls_vault")
            s = self._ingestor.stats
            self._vault_stat.set(
                f"vault: pogls_vault/\n"
                f"entries: {s['total_entries']}\n"
                f"sessions: {s['sessions']}")
            return self._ingestor
        except ImportError:
            self._ingest_log_add("⚠  pogls_ingest.py not found")
            self._ingest_log_add("   วาง pogls_ingest.py ไว้ใน folder เดียวกัน")
            return None

    def _ingest_chat(self, source: str):
        ing = self._get_ingestor()
        if not ing: return
        path = filedialog.askopenfilename(
            title=f"เลือก {source} export file",
            filetypes=[("JSON","*.json"),("All","*.*")])
        if not path: return
        def worker():
            try:
                n = ing.ingest_file(path, source=source)
                s = ing.stats
                self.after(0, lambda: self._ingest_log_add(
                    f"✅  {source}: {n} entries imported"))
                self.after(0, lambda: self._vault_stat.set(
                    f"vault: pogls_vault/\n"
                    f"entries: {s['total_entries']}\n"
                    f"sessions: {s['sessions']}"))
                self.after(0, lambda: self.set_status(f"✅ Ingest {source}: {n} entries"))
            except Exception as ex:
                self.after(0, lambda: self._ingest_log_add(f"❌  {ex}"))
        threading.Thread(target=worker, daemon=True).start()

    def _ingest_any(self):
        ing = self._get_ingestor()
        if not ing: return
        paths = filedialog.askopenfilenames(
            title="เลือก files",
            filetypes=[("All","*.*"),("JSON","*.json"),
                       ("Text","*.txt"),("Markdown","*.md")])
        if not paths: return
        def worker():
            total = 0
            for p in paths:
                try:
                    n = ing.ingest_file(p)
                    total += n
                    self.after(0, lambda fn=Path(p).name, c=n:
                               self._ingest_log_add(f"✅  {fn}: {c} entries"))
                except Exception as ex:
                    self.after(0, lambda e=ex: self._ingest_log_add(f"❌  {e}"))
            self.after(0, lambda: self.set_status(f"✅ Ingest: {total} entries"))
        threading.Thread(target=worker, daemon=True).start()

    def _export_ingest_csv(self):
        ing = self._get_ingestor()
        if not ing: return
        path = filedialog.asksaveasfilename(
            defaultextension=".csv",
            filetypes=[("CSV","*.csv")],
            initialfile=f"pogls_index_{datetime.now():%Y%m%d_%H%M}.csv")
        if not path: return
        try:
            ing.export_index(path)
            self._ingest_log_add(f"✅  CSV exported → {Path(path).name}")
            self.set_status(f"✅  CSV exported")
        except Exception as ex:
            self._ingest_log_add(f"❌  {ex}")

    # ═══════════════════════════════════════════════════════════════════
    #  STATUS BAR
    # ═══════════════════════════════════════════════════════════════════

    def _build_statusbar(self):
        t  = self.theme
        sb = tk.Frame(self, bg=t.panel, height=24)
        sb.pack(fill="x", side="bottom"); sb.pack_propagate(False)
        self._status = tk.StringVar(value="  Ready")
        tk.Label(sb, textvariable=self._status, bg=t.panel,
                 fg=t.muted, font=t.xs, padx=12).pack(side="left", pady=3)
        tk.Label(sb, text="POGLS V3.5  ·  Angular Addressing  ·  SimEngine",
                 bg=t.panel, fg=t.dim, font=t.xs, padx=12).pack(side="right", pady=3)

    # ═══════════════════════════════════════════════════════════════════
    #  VISUAL FEED POLLING (main thread)
    # ═══════════════════════════════════════════════════════════════════

    def _poll_visual(self):
        frame = self._get_frame_dict()
        if frame:
            self._apply_frame(frame)
        self.after(400, self._poll_visual)

    def _apply_frame(self, f: dict):
        t = self.theme
        health = f.get("audit_health", "OK")
        hcol   = t.green if health == "OK" else (t.amber if health == "DEGRADED" else t.red)

        # Top bar pill
        self._top_health.config(text=f"● {health}", fg=hcol)

        # Files tab stats bar
        self.audit_var.set(f"⬡ {health}")
        self.audit_lbl.config(fg=hcol)
        self.heads_var.set(f"Heads: {f.get('active_heads',0)}")

        # ── Hydra tab ─────────────────────────────────────────────────
        self._hv_audit.set(health)
        self._hv_heads.set(str(f.get("active_heads", 0)))
        self._hv_scans.set(str(f.get("total_scans", 0)))
        self._hv_anom.set( str(f.get("total_anomalies", 0)))
        self._hv_spawn.set(str(f.get("spawn_count", 0)))
        self._hv_inc.set(  str(f.get("incident_count", 0)))
        self._hydra_seq_var.set(f"frame #{f.get('frame_seq', 0)}")
        if f.get("is_stale"):
            self._hydra_stale.config(text=f"⚠ STALE {f.get('frame_age_ms',0)}ms")
        else:
            self._hydra_stale.config(text="")

        # Head chips
        head_map = {h["id"]: h for h in f.get("heads", [])}
        for i, chip in enumerate(self._hchips):
            if i in head_map:
                h = head_map[i]
                col = (t.green  if h["status"] == "ACTIVE"    else
                       t.amber  if h["status"] == "SPAWNING"  else
                       t.red    if h["status"] in ("SAFE","MIGRATING","RETRACTING") else
                       t.dim)
                chip.config(bg=col, fg=t.bg, relief="solid",
                            text=f"{i}\n{h['status'][:4]}")
            else:
                chip.config(bg=t.panel, fg=t.dim, relief="flat", text=str(i))

        # Tile squares
        tiles = f.get("tiles", [])
        for i, sq in enumerate(self._tsquares):
            if i >= len(tiles):
                sq.config(text="·", bg=t.panel, fg=t.dim); continue
            tile = tiles[i]
            state = tile.get("state","IDLE")
            if state == "ANOMALY":   sq.config(text="!", bg=t.red,   fg=t.bg)
            elif state == "CLEAN":   sq.config(text="#", bg=t.green, fg=t.bg)
            elif state == "SCANNING":sq.config(text="~", bg=t.cyan,  fg=t.bg)
            elif state == "CERTIFIED":sq.config(text="★",bg=t.amber, fg=t.bg)
            else:                    sq.config(text="·", bg=t.panel, fg=t.dim)

        # Event log
        events = f.get("events", [])
        if events:
            self._event_lb.delete(0, "end")
            for ev in reversed(events):
                sev = ev.get("severity", 0)
                ico = "❌" if sev >= 2 else ("⚠" if sev >= 1 else "·")
                ts  = datetime.fromtimestamp(ev.get("ts", 0)).strftime("%H:%M:%S") if ev.get("ts") else ""
                self._event_lb.insert("end",
                    f"  {ts}  {ico}  [{ev.get('type','?')}]  "
                    f"Head[{ev.get('head_id', '?')}]  {ev.get('msg','')}")

    # ═══════════════════════════════════════════════════════════════════
    #  DnD + FILE INGESTION
    # ═══════════════════════════════════════════════════════════════════

    def _setup_dnd(self):
        t = self.theme
        if _DND:
            self.drop_target_register(DND_FILES)
            self.dnd_bind("<<Drop>>", self._on_dnd_drop)
            self.drop_banner.config(
                text="  ⬇  ลาก files / folders มาวางที่นี่  ✓ Drag & Drop พร้อมใช้",
                fg=t.cyan)
        else:
            self.drop_banner.config(
                text="  ⬇  คลิกที่นี่เพื่อเพิ่มไฟล์  │  pip install tkinterdnd2  สำหรับ drag-drop",
                fg=t.muted, cursor="hand2")
            self.drop_banner.bind("<Button-1>", lambda _: self.add_files())

    def _on_dnd_drop(self, event):
        import re
        paths = re.findall(r'\{([^}]+)\}|(\S+)', event.data)
        self._ingest([a or b for a, b in paths])

    def add_files(self):
        paths = filedialog.askopenfilenames(
            title="เลือกไฟล์",
            filetypes=[("All files","*.*"),("Safetensors","*.safetensors"),
                       ("Checkpoint","*.ckpt;*.pt"),("JSON","*.json"),
                       ("Python","*.py"),("Zip","*.zip")])
        if paths: self._ingest(list(paths))

    def add_folder(self):
        folder = filedialog.askdirectory(title="เลือกโฟลเดอร์")
        if folder: self._ingest([folder])

    def _ingest(self, paths: list):
        added = 0
        for p in paths:
            pp = Path(p)
            if pp.is_dir():
                for f in pp.rglob("*"):
                    if f.is_file(): added += self._add_one(f)
            elif pp.is_file():
                added += self._add_one(pp)
        if added: self.set_status(f"✅  เพิ่ม {added} ไฟล์"); self.refresh()
        else: self.set_status("ℹ  ไม่มีไฟล์ใหม่")

    def _add_one(self, p: Path) -> int:
        eid = make_eid(str(p))
        if eid in self.db.data: return 0
        try: size = p.stat().st_size
        except Exception: return 0
        addr, theta = file_addr(str(p))
        chunk_mb = self.cfg["behavior"]["chunk_size_mb"]
        e = Entry(eid=eid, name=p.name, path=str(p),
                  size=size, ext=p.suffix.lower(),
                  addr=addr, theta=theta, topo=2,
                  added=datetime.now().strftime("%Y-%m-%d %H:%M"),
                  chunks=max(1, size // (chunk_mb * 1024 * 1024)))
        self.db.add(e); return 1

    # ═══════════════════════════════════════════════════════════════════
    #  LIST OPS
    # ═══════════════════════════════════════════════════════════════════

    def remove_sel(self):
        sel = self.tree.selection()
        if not sel: return
        if self.cfg["behavior"]["confirm_delete"]:
            if not messagebox.askyesno("ลบ", f"ลบ {len(sel)} รายการ?"): return
        for iid in sel:
            self.db.remove(self.tree.item(iid, "tags")[0])
        self.sel_eid = None; self.refresh()
        self.set_status(f"ลบ {len(sel)} รายการ")

    def open_sel(self):
        if not self.sel_eid: return
        e = self.db.data.get(self.sel_eid)
        if e:
            try: win_open(e.path)
            except Exception as ex: self.set_status(f"⚠  เปิดไม่ได้: {ex}")

    def _select_all(self, _=None):
        for iid in self.tree.get_children(): self.tree.selection_add(iid)

    def refresh(self, *_):
        q    = self.q_var.get()
        ext  = self.ext_var.get()
        topo = self.topo_var.get()
        view = self._view_var.get()
        if view == "__snaps__":
            results = sorted([e for e in self.db.data.values() if e.snaps],
                             key=lambda x: x.added, reverse=True)
        elif view == "__mapped__":
            results = self.db.search(q, ext, topo, mapped_only=True)
        else:
            results = self.db.search(q, ext, topo)
        # Folder filter from folder tree
        ff = getattr(self, "_folder_filter", None)
        if ff:
            results = [e for e in results if str(Path(e.path).parent) == ff]
        key_fns = {"name":lambda e:e.name.lower(),"size":lambda e:e.size,
                   "addr":lambda e:e.addr,"topo":lambda e:e.topo,
                   "snaps":lambda e:len(e.snaps),"chunks":lambda e:e.chunks,
                   "added":lambda e:e.added,"mapped":lambda e:e.mapped}
        results.sort(key=key_fns.get(self._sort_col, key_fns["added"]),
                     reverse=not self._sort_asc)
        self.tree.delete(*self.tree.get_children())
        for e in results:
            tag = "mapped" if e.mapped else ("snapped" if e.snaps else "normal")
            self.tree.insert("", "end",
                             values=(e.name, e.size_hr, e.addr_str, e.topo_str,
                                     len(e.snaps), e.chunks, e.added,
                                     "✓" if e.mapped else ""),
                             tags=(e.eid, tag))
        n = len(results); tot = len(self.db.data)
        st = self.db.stats()
        mb = st["size_b"] // (1024*1024)
        self.db_stat.set(f"{tot} files\n{mb:,} MB total")
        self.sel_info.config(text=f"แสดง {n} / {tot}")
        top_exts = "  ".join(f"{x}:{c}" for x, c in st["top_ext"]) if st["top_ext"] else ""
        self.stats_var.set(f"  {tot} files  ·  {size_str(st['size_b'])}  ·  {top_exts}")
        self.mapped_var.set(f"Mapped: {st['mapped']}/{tot}  ·  Snaps: {st['snapped']}  ")

    def _sort(self, col):
        if self._sort_col == col: self._sort_asc = not self._sort_asc
        else: self._sort_col = col; self._sort_asc = True
        self.refresh()

    # ── SELECTION → DOCK ────────────────────────────────────────────────
    def _on_select(self, _=None):
        sel = self.tree.selection()
        if not sel: return
        eid = self.tree.item(sel[0], "tags")[0]
        e   = self.db.data.get(eid)
        if not e: return
        self.sel_eid = eid; t = self.theme
        folder = str(Path(e.path).parent)
        if len(folder) > 36: folder = "…" + folder[-35:]
        self._dv["name"].set(e.name[:38] + ("…" if len(e.name) > 38 else ""))
        self._dv["folder"].set(folder); self._dv["size"].set(e.size_hr)
        self._dv["ext"].set(e.ext or "—")
        self._dv["chunks"].set(f"{e.chunks} × {self.cfg['behavior']['chunk_size_mb']} MB")
        self._dv["addr"].set(e.addr_str); self._dv["theta"].set(f"{e.theta:.3f}°")
        self._dv["topo"].set(e.topo_str); self._dv["added"].set(e.added)
        self._dv["snaps"].set(str(len(e.snaps)))
        self._dv["mapped"].set("✓ yes" if e.mapped else "not yet")
        frame = self._get_frame_dict()
        if frame:
            matched = None
            for h in frame.get("heads", []):
                if h.get("zone_start",0) <= e.addr < h.get("zone_end", 1<<20):
                    matched = h; break
            if matched:
                self._dv["head_id"].set(f"Head[{matched['id']}] {matched['status']}")
                self._dv["branch_id"].set(f"0x{matched.get('branch',0):016X}"[:20])
                self._dv["write_mode"].set(
                    "SAFE" if matched["status"] == "SAFE" else
                    "MIGRATING" if matched["status"] == "MIGRATING" else "NORMAL")
            else:
                self._dv["head_id"].set("Core direct"); self._dv["branch_id"].set("—")
                self._dv["write_mode"].set("NORMAL")
            snap_state = e.snaps[-1].get("state","PENDING") if e.snaps else "—"
            self._dv["snap_state"].set(snap_state)
        else:
            for k in ("head_id","branch_id","snap_state","write_mode"): self._dv[k].set("—")
        self._draw_wheel(e.addr)
        self._update_preview(e)
        self.snap_lb.delete(0, "end")
        for s in reversed(e.snaps):
            state = s.get("state","PENDING")[:4]
            self.snap_lb.insert("end",
                f"[{s['sid']}] {state:<4} {s['label'][:14]:<14}  {s['ts'][-8:]}")
        self.sel_info.config(text=f"{len(sel)} เลือก")

    def _on_double(self, _):
        sel = self.tree.selection()
        if not sel: return
        eid = self.tree.item(sel[0], "tags")[0]
        e   = self.db.data.get(eid)
        if e and Path(e.path).exists():
            try: win_open(e.path)
            except Exception: pass

    def _ctx_menu(self, event):
        iid = self.tree.identify_row(event.y)
        if iid: self.tree.selection_set(iid)
        t = self.theme
        m = tk.Menu(self, tearoff=0, bg=t.card, fg=t.text,
                    activebackground=t.hover, activeforeground=t.amber, font=t.xs)
        m.add_command(label="📸  Snapshot",        command=self.snap_sel)
        m.add_command(label="⏪  Restore",          command=self.restore_sel)
        m.add_separator()
        m.add_command(label="🗜  Zip Selected",     command=self.zip_sel)
        m.add_command(label="📂  Open in Explorer", command=self.open_sel)
        m.add_command(label="🧬  DNA Scan",         command=self.dna_scan)
        m.add_separator()
        m.add_command(label="⚙  Settings",         command=self.open_settings)
        m.add_separator()
        m.add_command(label="✕  Remove",            command=self.remove_sel)
        try: m.tk_popup(event.x_root, event.y_root)
        finally: m.grab_release()

    # ── WHEEL ────────────────────────────────────────────────────────────
    def _draw_wheel(self, addr: int, n: int = 20):
        cv = self.wheel; cv.delete("all"); t = self.theme
        W2, H2 = 260, 148; cx, cy, r = W2//2, H2//2+6, 54
        cv.create_oval(cx-r-10, cy-r-10, cx+r+10, cy+r+10, outline=t.b2, width=1)
        for i in range(72):
            ang  = math.radians(i*5); long = (i%9==0)
            r0, r1 = r+10, r+(18 if long else 13)
            cv.create_line(cx+r0*math.cos(ang), cy-r0*math.sin(ang),
                           cx+r1*math.cos(ang), cy-r1*math.sin(ang),
                           fill=t.b2 if long else t.dim, width=1)
        cv.create_oval(cx-r, cy-r, cx+r, cy+r, outline=t.b2, width=2)
        pct = addr/(1<<n) if addr else 0.0; sweep = pct*360
        if sweep > 0.5:
            cv.create_arc(cx-r, cy-r, cx+r, cy+r, start=90, extent=-sweep,
                          outline=t.amber, width=3, style="arc")
        ang = math.radians(90-sweep); nx=cx+r*math.cos(ang); ny=cy-r*math.sin(ang)
        cv.create_line(cx, cy, nx, ny, fill=t.amber, width=2)
        cv.create_oval(nx-4, ny-4, nx+4, ny+4, fill=t.amber, outline=t.bg, width=1)
        cv.create_oval(cx-5, cy-5, cx+5, cy+5, fill=t.cyan, outline=t.bg, width=2)
        for txt, dx, dy in [("0°",0,-(r+20)),("90°",r+20,0),
                             ("180°",0,r+12),("270°",-(r+28),0)]:
            cv.create_text(cx+dx, cy+dy, text=txt, fill=t.muted, font=("Consolas",7))
        cv.create_text(cx, cy-10, text=f"{sweep:.1f}°", fill=t.text, font=("Consolas",12,"bold"))
        cv.create_text(cx, cy+8,  text=f"A = {addr:,}"[:16], fill=t.muted, font=("Consolas",8))
        cv.create_text(cx, cy+22, text=f"{pct*100:.2f}%  of  2²⁰",
                       fill=t.dim, font=("Consolas",7))

    # ── FOLDER TREE ──────────────────────────────────────────────────────

    def _folder_refresh_db(self, *_):
        """Rebuild folder tree from DB entries (group by parent folder)"""
        ft = self._folder_tree
        ft.delete(*ft.get_children())
        # Root node
        all_node = ft.insert("", "end", text="◈  All Files",
                             values=("__all__",), tags=("folder",), open=True)
        # Group entries by parent folder path
        folders: Dict[str, List[str]] = {}
        for e in self.db.data.values():
            folder = str(Path(e.path).parent)
            folders.setdefault(folder, []).append(e.eid)

        for folder_path, eids in sorted(folders.items()):
            display = Path(folder_path).name or folder_path
            if len(display) > 28: display = "…" + display[-27:]
            node = ft.insert(all_node, "end",
                             text=f"📁  {display}  ({len(eids)})",
                             values=(folder_path,), tags=("folder",))
            # Child entries
            for eid in eids[:8]:  # max 8 visible per folder
                e = self.db.data.get(eid)
                if e:
                    name = e.name[:26] + ("…" if len(e.name)>26 else "")
                    ft.insert(node, "end", text=f"  {name}",
                              values=(eid,), tags=("empty",))
            if len(eids) > 8:
                ft.insert(node, "end",
                          text=f"  … {len(eids)-8} more",
                          values=("",), tags=("empty",))
        if not folders:
            ft.insert(all_node, "end", text="  (no files yet)",
                      values=("",), tags=("empty",))

    def _folder_browse(self):
        """Browse filesystem — show real folder tree"""
        folder = filedialog.askdirectory(title="เลือก folder เพื่อ browse")
        if not folder: return
        ft = self._folder_tree
        ft.delete(*ft.get_children())
        t = self.theme
        root_node = ft.insert("", "end", text=f"📁  {Path(folder).name}",
                              values=(folder,), tags=("fs",), open=True)
        self._folder_load_children(root_node, Path(folder))
        self.set_status(f"Browse: {folder}")

    def _folder_load_children(self, parent_node, path: Path, depth: int = 0):
        """Recursively populate folder tree from filesystem (max depth 2)"""
        if depth > 2: return
        ft = self._folder_tree
        try:
            items = sorted(path.iterdir(), key=lambda x: (x.is_file(), x.name.lower()))
            dirs  = [p for p in items if p.is_dir() and not p.name.startswith(".")][:20]
            files = [p for p in items if p.is_file()][:30]
            for d in dirs:
                node = ft.insert(parent_node, "end",
                                 text=f"📁  {d.name}",
                                 values=(str(d),), tags=("fs",))
                # Lazy expand placeholder
                ft.insert(node, "end", text="…", values=("",), tags=("empty",))
            for f in files:
                name = f.name[:28] + ("…" if len(f.name)>28 else "")
                ft.insert(parent_node, "end",
                          text=f"  {name}",
                          values=(str(f),), tags=("empty",))
            if not dirs and not files:
                ft.insert(parent_node, "end", text="  (empty)",
                          values=("",), tags=("empty",))
        except PermissionError:
            ft.insert(parent_node, "end", text="  ⚠ permission denied",
                      values=("",), tags=("empty",))

    def _on_folder_select(self, _=None):
        """Filter file list when folder selected in tree"""
        sel = self._folder_tree.selection()
        if not sel: return
        val = self._folder_tree.item(sel[0], "values")
        if not val or not val[0]: return
        v = val[0]
        if v == "__all__":
            self._view_var.set("all"); self._folder_filter = None
        elif Path(v).is_dir():
            self._folder_filter = v
        else:
            # it's an eid — select in main tree
            self._folder_filter = None
            for iid in self.tree.get_children():
                tags = self.tree.item(iid, "tags")
                if tags and tags[0] == v:
                    self.tree.selection_set(iid)
                    self.tree.see(iid)
                    self._on_select()
                    return
        self.refresh()

    # ── IMAGE PREVIEW ─────────────────────────────────────────────────────

    def _draw_preview_placeholder(self, msg: str = "no selection"):
        cv = self._preview_canvas; cv.delete("all")
        t  = self.theme
        cv.create_rectangle(0, 0, 260, 160, fill=t.card, outline="")
        cv.create_text(130, 75, text=msg, fill=t.muted,
                       font=("Consolas", 9), anchor="center")

    def _update_preview(self, e):
        """Update preview canvas for selected entry e"""
        IMG_EXTS = {".png",".jpg",".jpeg",".webp",".bmp",".tiff",".tga"}
        cv = self._preview_canvas
        cv.delete("all")
        t  = self.theme
        cv.create_rectangle(0, 0, 260, 160, fill=t.card, outline="")

        if e.ext.lower() not in IMG_EXTS:
            # Non-image: show file type icon + info
            icons = {".safetensors":"🧠",".ckpt":"💾",".pt":"🔥",
                     ".json":"{ }",".py":"🐍",".zip":"🗜",".txt":"📄"}
            icon = icons.get(e.ext.lower(), "◈")
            cv.create_text(130, 60, text=icon,
                           fill=t.amber, font=("Consolas", 28), anchor="center")
            cv.create_text(130, 100, text=e.ext or "unknown",
                           fill=t.muted, font=("Consolas", 10), anchor="center")
            cv.create_text(130, 118, text=e.size_hr,
                           fill=t.dim, font=("Consolas", 8), anchor="center")
            self._preview_img_ref = None
            return

        # Image file — try to load with PIL
        p = Path(e.path)
        if not p.exists():
            self._draw_preview_placeholder("file not found")
            return

        def load_img():
            try:
                from PIL import Image, ImageTk
                img = Image.open(p)
                # Thumbnail fit 256×152
                img.thumbnail((256, 152), Image.LANCZOS)
                photo = ImageTk.PhotoImage(img)
                # Must update canvas in main thread
                self.after(0, lambda ph=photo, im=img: self._set_preview(ph, im, e))
            except ImportError:
                self.after(0, lambda: self._draw_preview_placeholder(
                    "pip install Pillow\nfor image preview"))
            except Exception as ex:
                self.after(0, lambda: self._draw_preview_placeholder(f"load error"))
        threading.Thread(target=load_img, daemon=True).start()
        self._draw_preview_placeholder("loading…")

    def _set_preview(self, photo, img, e):
        """Place loaded image in preview canvas (main thread)"""
        cv = self._preview_canvas
        cv.delete("all")
        t  = self.theme
        cv.create_rectangle(0, 0, 260, 160, fill=t.card, outline="")
        # Center image
        x = (260 - img.width)  // 2
        y = (160 - img.height) // 2
        cv.create_image(x, y, anchor="nw", image=photo)
        # Dim overlay with filename
        cv.create_rectangle(0, 140, 260, 160,
                            fill=t.bg, stipple="gray50", outline="")
        cv.create_text(130, 150, text=f"{e.name[:32]}  {e.size_hr}",
                       fill=t.muted, font=("Consolas", 7), anchor="center")
        self._preview_img_ref = photo   # prevent GC
    def snap_sel(self):
        if not self.sel_eid: self.set_status("เลือกไฟล์ก่อน"); return
        label = self._ask_input("สร้าง Snapshot", "ชื่อ snapshot:")
        if label is None: return
        s = self.db.snap(self.sel_eid, label.strip() or "auto")
        self.refresh(); self._on_select()
        self.set_status(f"📸  Snapshot [{s['sid']}]  '{s['label']}'")

    def restore_sel(self):
        if not self.sel_eid: self.set_status("เลือกไฟล์ก่อน"); return
        idx = self.snap_lb.curselection()
        if not idx: self.set_status("เลือก snapshot ก่อน"); return
        e = self.db.data.get(self.sel_eid)
        snap = e.snaps[-(idx[0]+1)]
        if messagebox.askyesno("Restore", f"Restore → [{snap['sid']}] '{snap['label']}'?"):
            e.addr = snap["addr"]; e.topo = snap["topo"]
            self.db.save(); self.refresh(); self._on_select()
            self.set_status(f"⏪  Restored → snap [{snap['sid']}]")

    def show_history(self):
        if not self.sel_eid: self.set_status("เลือกไฟล์ก่อน"); return
        e = self.db.data.get(self.sel_eid)
        if not e or not e.snaps: self.set_status("ยังไม่มี snapshot"); return
        t = self.theme
        win = tk.Toplevel(self); win.title(f"Version History  ·  {e.name}")
        win.geometry("660x370"); win.configure(bg=t.bg)
        tk.Label(win, text=f"◈  {e.name}", bg=t.bg, fg=t.amber,
                 font=t.head).pack(anchor="w", padx=14, pady=10)
        cols = ("sid","label","ts","addr","topo")
        widths = {"sid":32,"label":190,"ts":150,"addr":118,"topo":100}
        hdrs   = {"sid":"#","label":"Label","ts":"Time","addr":"Address","topo":"Topology"}
        tr = ttk.Treeview(win, columns=cols, show="headings",
                          style="P.Treeview", height=11)
        for c in cols:
            tr.heading(c, text=hdrs[c]); tr.column(c, width=widths[c])
        for s in reversed(e.snaps):
            tr.insert("", "end", values=(s["sid"], s["label"], s["ts"],
                                         f"{s['addr']:,}", TOPO_NAMES[min(s["topo"],4)]))
        tr.pack(fill="both", expand=True, padx=10, pady=4)
        self.w.btn(win, "  ปิด  ", win.destroy, t.muted, t.sm).pack(pady=10)

    # ── ZIP ──────────────────────────────────────────────────────────────
    def zip_sel(self):
        sel = self.tree.selection()
        if not sel: self.set_status("เลือกไฟล์ก่อน"); return
        entries = [self.db.data[self.tree.item(i,"tags")[0]]
                   for i in sel if self.tree.item(i,"tags")[0] in self.db.data]
        self._do_zip(entries)

    def export_all(self):
        if not self.db.data: self.set_status("ไม่มีไฟล์"); return
        self._do_zip(list(self.db.data.values()))

    def _do_zip(self, entries: list):
        if not entries: return
        out = filedialog.asksaveasfilename(
            defaultextension=".zip", filetypes=[("Zip","*.zip")],
            initialfile=f"pogls_{datetime.now():%Y%m%d_%H%M}.zip")
        if not out: return
        def worker():
            self.set_status(f"กำลัง zip {len(entries)} ไฟล์…")
            try:
                with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
                    zf.writestr("pogls_manifest.json",
                                json.dumps({"created": datetime.now().isoformat(),
                                            "pogls_version": "3.5",
                                            "files": [asdict(e) for e in entries]},
                                           indent=2, ensure_ascii=False))
                    for e in entries:
                        p = Path(e.path)
                        if p.exists(): zf.write(p, p.name)
                kb = Path(out).stat().st_size // 1024
                self.set_status(f"✅  {Path(out).name}  ({kb:,} KB)")
            except Exception as ex:
                self.set_status(f"❌  Zip error: {ex}")
        threading.Thread(target=worker, daemon=True).start()

    # ── DNA SCAN ─────────────────────────────────────────────────────────
    def dna_scan(self):
        if not self.sel_eid: self.set_status("เลือกไฟล์ก่อน"); return
        e = self.db.data.get(self.sel_eid)
        if not e or e.ext not in (".safetensors", ".json"):
            self.set_status("DNA Scan รองรับ .safetensors และ .json"); return
        def worker():
            self.set_status(f"🧬  สแกน {e.name}…")
            try:
                from safetensors import safe_open
                import numpy as np
                layers, total_mb = [], 0.0
                with safe_open(e.path, framework="pt", device="cpu") as f:
                    for i, name in enumerate(f.keys()):
                        sl = f.get_slice(name); shape = list(sl.get_shape())
                        mb = int(np.prod(shape)) * 2 / 1_048_576
                        total_mb += mb
                        layers.append({"id":i,"name":name,"shape":shape,
                                       "size_mb":round(mb,4)})
                out_dir  = self.cfg["paths"]["dna_output_dir"]
                out_dir  = Path(out_dir) if out_dir else Path(e.path).parent
                out_path = out_dir / (Path(e.path).stem + "_dna.json")
                out_path.write_text(json.dumps({"model":e.name,
                    "num_layers":len(layers),"total_mb":round(total_mb,2),
                    "layers":layers}, indent=2, ensure_ascii=False), encoding="utf-8")
                e.mapped = True; self.db.save(); self.refresh()
                self.set_status(f"✅  DNA: {out_path.name}  ({len(layers)} layers)")
            except ImportError:
                self.set_status("⚠  pip install safetensors numpy")
            except Exception as ex:
                self.set_status(f"❌  DNA error: {ex}")
        threading.Thread(target=worker, daemon=True).start()

    # ── BATCH IMPORT ─────────────────────────────────────────────────────
    def batch_import(self):
        folder = filedialog.askdirectory(title="เลือกโฟลเดอร์")
        if not folder: return
        t = self.theme
        win = tk.Toplevel(self); win.title("Batch Import"); win.geometry("400x200")
        win.configure(bg=t.bg); win.resizable(False,False); win.grab_set()
        tk.Label(win, text="Batch Import", bg=t.bg, fg=t.amber, font=t.head).pack(pady=12)
        tk.Label(win, text=f"📁  {Path(folder).name}", bg=t.bg, fg=t.text, font=t.sm).pack()
        tf = tk.Frame(win, bg=t.bg); tf.pack(pady=10)
        tk.Label(tf, text="Pattern:", bg=t.bg, fg=t.muted, font=t.xs).pack(side="left")
        pv = tk.StringVar(value="*")
        tk.Entry(tf, textvariable=pv, bg=t.card, fg=t.text, font=t.ui,
                 relief="flat", width=22, insertbackground=t.amber).pack(side="left", padx=8)
        def go():
            pat = pv.get().strip() or "*"
            files = [str(p) for p in Path(folder).rglob(pat) if p.is_file()]
            win.destroy(); self._ingest(files)
        self.w.btn(win, "  ▶  Import  ", go, t.amber, t.sm).pack(pady=12)

    # ── CLEAR / SETTINGS ─────────────────────────────────────────────────
    def clear_db(self):
        if not self.db.data: self.set_status("List ว่างอยู่แล้ว"); return
        if messagebox.askyesno("ล้าง List",
                               f"ลบทั้งหมด {len(self.db.data)} รายการ?\n(ไฟล์จริงไม่ถูกลบ)"):
            self.db.data.clear(); self.db.save()
            self.sel_eid = None; self.refresh()
            self.set_status("ล้าง list แล้ว")

    def open_settings(self): SettingsDialog(self)

    # ── HELPERS ──────────────────────────────────────────────────────────
    def _ask_input(self, title, prompt, default="") -> Optional[str]:
        t = self.theme
        win = tk.Toplevel(self); win.title(title); win.geometry("340x120")
        win.configure(bg=t.bg); win.resizable(False,False); win.grab_set()
        tk.Label(win, text=prompt, bg=t.bg, fg=t.text, font=t.ui).pack(pady=10)
        sv = tk.StringVar(value=default)
        ent = tk.Entry(win, textvariable=sv, bg=t.card, fg=t.text,
                       font=t.ui, relief="flat", width=28, insertbackground=t.amber)
        ent.pack(pady=2); ent.focus_set(); ent.select_range(0, "end")
        result = [None]
        def ok(_=None): result[0] = sv.get(); win.destroy()
        ent.bind("<Return>", ok); ent.bind("<Escape>", lambda _: win.destroy())
        bf = tk.Frame(win, bg=t.bg); bf.pack(pady=8)
        self.w.btn(bf, "  OK  ", ok, t.amber, t.xs).pack(side="left", padx=4)
        self.w.btn(bf, "  ยกเลิก  ", win.destroy, t.muted, t.xs).pack(side="left", padx=4)
        self.wait_window(win); return result[0]

    def set_status(self, msg: str):
        self._status.set(f"  {msg}")
        ms = self.cfg["behavior"]["status_timeout_ms"]
        self.after(ms, lambda: self._status.set("  Ready"))

    def _on_close(self):
        if self._adapter:
            try: self._adapter.stop()
            except Exception: pass
        self._sim.stop()
        self.destroy()


# ═══════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    app = App()
    app.protocol("WM_DELETE_WINDOW", app._on_close)
    app.mainloop()
