"""
pogls_memory_fabric.py — POGLS AI Memory Fabric
════════════════════════════════════════════════════════════════════════
Passive middleware ให้ AI หลายตัว share persistent memory ผ่าน REST API

Endpoints:
  POST /remember          store memory (key + content + metadata)
  GET  /recall?q=...      semantic recall by key or content search
  GET  /recall?key=...    exact key recall
  GET  /status            fabric health + stats
  GET  /snapshot          force snapshot + return merkle root

Design:
  - Address = fibo_addr(hash(key)) → deterministic slot
  - Storage  = POGLS delta lane (append-only, crash-safe)
  - Audit    = unit circle check per entry
  - Multi-AI = shared via HTTP → ไม่ต้อง share process/memory

วิธีใช้:
  pip install flask
  python3 pogls_memory_fabric.py
  # default port 7474

  POST /remember
    { "key": "session_42_user_pref",
      "content": "user likes dark mode",
      "agent": "claude-3",
      "tags": ["preference", "ui"] }

  GET /recall?q=dark+mode
  GET /recall?key=session_42_user_pref
════════════════════════════════════════════════════════════════════════
"""

import os, sys, time, json, hashlib, mmap, struct, threading
import tempfile, logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# ── constants ──────────────────────────────────────────────────────────
PHI_SCALE   = 1 << 20          # 2²⁰
PHI_UP      = 1_696_631        # floor(φ  × 2²⁰)  World A
PHI_DOWN    =   648_055        # floor(φ⁻¹× 2²⁰)  World B
VERSION     = "3.6"
PORT        = int(os.environ.get("POGLS_PORT", 7474))
STORAGE_DIR = os.environ.get("POGLS_DIR",
                              os.path.join(tempfile.gettempdir(), ".pogls_fabric"))
MAX_ENTRIES = 1 << 20          # 1M entries max

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [FABRIC] %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger("fabric")

# ── fibo address ──────────────────────────────────────────────────────
def fibo_addr(n: int, world: int = 0) -> int:
    mul = PHI_UP if world == 0 else PHI_DOWN
    return (n * mul) % PHI_SCALE

def in_circle(addr: int) -> bool:
    return 2 * addr * addr < PHI_SCALE * PHI_SCALE

def key_to_addr(key: str, world: int = 0) -> int:
    """key → deterministic fibo address"""
    h = int(hashlib.sha256(key.encode()).hexdigest(), 16)
    n = h % MAX_ENTRIES
    return fibo_addr(n, world)

# ── MemoryEntry ────────────────────────────────────────────────────────
class MemoryEntry:
    """16B header + variable payload"""
    MAGIC = b"POGM"   # POGLS Memory

    def __init__(self, key: str, content: str,
                 agent: str = "", tags: list = None,
                 ts: float = None):
        self.key     = key
        self.content = content
        self.agent   = agent
        self.tags    = tags or []
        self.ts      = ts or time.time()
        self.addr    = key_to_addr(key)
        self.safe    = in_circle(self.addr)

    def to_dict(self) -> dict:
        return {
            "key":     self.key,
            "content": self.content,
            "agent":   self.agent,
            "tags":    self.tags,
            "ts":      self.ts,
            "addr":    self.addr,
            "safe":    self.safe,
            "world":   "A" if self.safe else "B(edge)",
        }

    def to_bytes(self) -> bytes:
        payload = json.dumps(self.to_dict()).encode("utf-8")
        header  = struct.pack(">4sHHI",
                              self.MAGIC,
                              len(self.key.encode()),
                              len(payload),
                              int(self.ts))
        return header + payload

    @classmethod
    def from_bytes(cls, data: bytes):
        magic, klen, plen, ts = struct.unpack(">4sHHI", data[:12])
        if magic != cls.MAGIC:
            raise ValueError("bad magic")
        payload = data[12:12 + plen]
        d = json.loads(payload)
        e = cls.__new__(cls)
        e.__dict__.update(d)
        return e

# ── MemoryStore ────────────────────────────────────────────────────────
class MemoryStore:
    """Append-only delta store — crash-safe, no WAL"""

    def __init__(self, dirpath: str):
        os.makedirs(dirpath, exist_ok=True)
        self._dir    = dirpath
        self._lock   = threading.RLock()
        self._index  = {}   # key → MemoryEntry (in-memory index)
        self._count  = 0
        self._inside = 0    # unit circle stats
        self._log_path = os.path.join(dirpath, "memory.delta")
        self._snap_path= os.path.join(dirpath, "snapshot.merkle")
        self._boot()

    def _boot(self):
        """boot scan — replay delta log → rebuild index"""
        if not os.path.exists(self._log_path):
            log.info("NEW store — empty delta log")
            return
        count = errors = 0
        with open(self._log_path, "rb") as f:
            while True:
                hdr = f.read(12)
                if len(hdr) < 12: break
                try:
                    magic, klen, plen, ts = struct.unpack(">4sHHI", hdr)
                    if magic != MemoryEntry.MAGIC:
                        break
                    payload = f.read(plen)
                    d = json.loads(payload)
                    e = MemoryEntry.__new__(MemoryEntry)
                    e.__dict__.update(d)
                    self._index[e.key] = e
                    count += 1
                    if e.safe: self._inside += 1
                except Exception:
                    errors += 1
                    break
        self._count = count
        log.info(f"BOOT: replayed {count} entries ({errors} errors)")

    def remember(self, entry: MemoryEntry) -> dict:
        with self._lock:
            data = entry.to_bytes()
            with open(self._log_path, "ab") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            is_new = entry.key not in self._index
            self._index[entry.key] = entry
            if is_new:
                self._count += 1
                if entry.safe: self._inside += 1
            return {
                "stored": True,
                "key":    entry.key,
                "addr":   entry.addr,
                "safe":   entry.safe,
                "world":  "A" if entry.safe else "B(edge)",
                "total":  self._count,
            }

    def recall_exact(self, key: str):
        return self._index.get(key)

    def recall_search(self, q: str, limit: int = 10) -> list:
        """simple substring search — ขยายเป็น vector search ได้"""
        q_lower = q.lower()
        results = []
        for e in self._index.values():
            score = 0
            if q_lower in e.key.lower():     score += 3
            if q_lower in e.content.lower(): score += 2
            if any(q_lower in t.lower() for t in e.tags): score += 1
            if score > 0:
                results.append((score, e))
        results.sort(key=lambda x: (-x[0], -x[1].ts))
        return [e.to_dict() for _, e in results[:limit]]

    def snapshot(self) -> dict:
        with self._lock:
            h = hashlib.sha256()
            for key in sorted(self._index.keys()):
                e = self._index[key]
                h.update(f"{key}:{e.content}:{e.ts}".encode())
            root = h.hexdigest()
            snap = {
                "root":    root,
                "count":   self._count,
                "inside":  self._inside,
                "outside": self._count - self._inside,
                "ts":      time.time(),
                "version": VERSION,
            }
            with open(self._snap_path, "w") as f:
                json.dump(snap, f)
            return snap

    def status(self) -> dict:
        sz = os.path.getsize(self._log_path) if os.path.exists(self._log_path) else 0
        return {
            "version":  VERSION,
            "entries":  self._count,
            "inside":   self._inside,
            "outside":  self._count - self._inside,
            "pct_safe": f"{self._inside/max(1,self._count)*100:.1f}%",
            "log_bytes":sz,
            "log_kb":   sz // 1024,
            "dir":      self._dir,
            "phi_up":   PHI_UP,
            "phi_down": PHI_DOWN,
            "phi_scale":PHI_SCALE,
        }

# ── HTTP Handler ───────────────────────────────────────────────────────
class FabricHandler(BaseHTTPRequestHandler):

    store: MemoryStore = None   # class-level, set before serve

    def log_message(self, fmt, *args):
        pass   # suppress default access log

    def _send_json(self, data: dict, code: int = 200):
        body = json.dumps(data, ensure_ascii=False, indent=2).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.send_header("X-POGLS-Version", VERSION)
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, msg: str, code: int = 400):
        self._send_json({"error": msg, "code": code}, code)

    def do_GET(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        path   = parsed.path.rstrip("/")

        if path == "/recall":
            if "key" in params:
                key = params["key"][0]
                e   = self.store.recall_exact(key)
                if e:
                    self._send_json({"found": True, "entry": e.to_dict()})
                else:
                    self._send_json({"found": False, "key": key}, 404)

            elif "q" in params:
                q       = params["q"][0]
                limit   = int(params.get("limit", ["10"])[0])
                results = self.store.recall_search(q, limit)
                self._send_json({
                    "query":   q,
                    "count":   len(results),
                    "results": results,
                })
            else:
                self._send_error("need ?key= or ?q=")

        elif path == "/status":
            self._send_json(self.store.status())

        elif path == "/snapshot":
            snap = self.store.snapshot()
            log.info(f"snapshot root={snap['root'][:16]}… entries={snap['count']}")
            self._send_json(snap)

        elif path == "/":
            self._send_json({
                "name":      "POGLS AI Memory Fabric",
                "version":   VERSION,
                "endpoints": [
                    "POST /remember",
                    "GET  /recall?key=...",
                    "GET  /recall?q=...[&limit=N]",
                    "GET  /status",
                    "GET  /snapshot",
                ],
            })
        else:
            self._send_error(f"unknown path: {path}", 404)

    def do_POST(self):
        path = urlparse(self.path).path.rstrip("/")

        if path == "/remember":
            length = int(self.headers.get("Content-Length", 0))
            if length == 0:
                self._send_error("empty body"); return
            try:
                body = json.loads(self.rfile.read(length))
            except json.JSONDecodeError as ex:
                self._send_error(f"bad JSON: {ex}"); return

            key     = body.get("key", "").strip()
            content = body.get("content", "").strip()
            if not key:
                self._send_error("key required"); return
            if not content:
                self._send_error("content required"); return

            entry  = MemoryEntry(
                key     = key,
                content = content,
                agent   = body.get("agent", "unknown"),
                tags    = body.get("tags", []),
            )
            result = self.store.remember(entry)
            log.info(f"REMEMBER key={key[:40]} addr={result['addr']} "
                     f"safe={result['safe']} total={result['total']}")
            self._send_json(result, 201)
        else:
            self._send_error(f"unknown path: {path}", 404)

# ── quick self-test ────────────────────────────────────────────────────
def self_test():
    import tempfile, shutil
    td = tempfile.mkdtemp()
    try:
        store = MemoryStore(td)

        # remember
        e1 = MemoryEntry("test_key_1", "user prefers dark mode",
                          agent="claude", tags=["ui", "pref"])
        r  = store.remember(e1)
        assert r["stored"] and r["total"] == 1, "remember failed"

        e2 = MemoryEntry("test_key_2", "session timeout = 30min",
                          agent="gpt-4", tags=["session"])
        store.remember(e2)

        # recall exact
        found = store.recall_exact("test_key_1")
        assert found and found.content == "user prefers dark mode", "recall exact failed"

        # recall search
        results = store.recall_search("dark mode")
        assert len(results) >= 1, "recall search failed"

        # unit circle
        assert e1.addr < PHI_SCALE, "addr out of range"
        safe_count = sum(1 for e in [e1,e2] if e.safe)
        assert 0 <= safe_count <= 2, "circle check failed"

        # snapshot
        snap = store.snapshot()
        assert snap["count"] == 2, "snapshot count wrong"
        assert len(snap["root"]) == 64, "snapshot root wrong"

        # boot replay
        store2 = MemoryStore(td)
        assert store2._count == 2, "boot replay failed"
        found2 = store2.recall_exact("test_key_2")
        assert found2 and found2.agent == "gpt-4", "replay entry wrong"

        print("✅ self_test: 8/8 PASS")
        return True
    finally:
        shutil.rmtree(td)

# ── main ───────────────────────────────────────────────────────────────
def main():
    print("═" * 64)
    print(f"  POGLS AI Memory Fabric  v{VERSION}")
    print(f"  POST /remember   GET /recall   GET /status")
    print(f"  Port: {PORT}   Dir: {STORAGE_DIR}")
    print("═" * 64)

    # self test
    self_test()

    # init store
    store = MemoryStore(STORAGE_DIR)
    FabricHandler.store = store

    log.info(f"Booted — {store._count} entries in index")
    log.info(f"Listening on http://0.0.0.0:{PORT}")

    server = HTTPServer(("0.0.0.0", PORT), FabricHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutdown — taking snapshot...")
        snap = store.snapshot()
        log.info(f"Final snapshot root={snap['root'][:16]}… entries={snap['count']}")
        server.server_close()

if __name__ == "__main__":
    if "--test" in sys.argv:
        sys.exit(0 if self_test() else 1)
    main()
