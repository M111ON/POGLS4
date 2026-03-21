# POGLS Chat Bridge + Extension — Session Snapshot
2026-03-16 | Ratchaburi, Thailand

---

## สิ่งที่ทำเสร็จ session นี้

### pogls_chat_bridge/ package
```
channels/all_channels.py     WS(7475) + Clipboard(0.5s poll) + FileDrop
parsers/chat_parser.py       parse_claude/chatgpt/deepseek/gemini/plain
storage/bridge_store.py      dual-write MemoryFabric + GUIDatabase
core_types.py V1.1           ConversationPair + event_id + parent_id
bridge.py                    _on_pairs + session boundary + parent chain
run.py                       CLI launcher
```

### pogls_extension/ (Chrome MV3)
```
manifest.json                hosts: all 5 platforms
content_script.js v1.2       MutationObserver + pre-seed + DOM-order pairing
popup.html + popup.js        slider + platform checkbox + date + keyword
                             per-pair checkbox selection
                             cleanText() strip code/terminal
```

---

## Platform DOM Selectors (verified 2026-03)

### ChatGPT ✅
- user: `.user-message-bubble-color .whitespace-pre-wrap`
- asst: `article.text-token-text-primary` (filter "คุณพูดว่า"/"You said")
- strip: `^ChatGPT พูดว่า:\s*`
- method: `captureDOMOrderChatGPT()` — DOM order (88 elements, 42-43 pairs)

### DeepSeek ✅
- user: `div.d29f3d7d.ds-message`
- asst: `div.ds-message:not(.d29f3d7d)`
- think block: `._74c0879` → filter `.ds-markdown` outside think
- method: `captureDOMOrderDeepSeek()` — DOM order (ASST→USER pattern)
- virtual scroll: pre-seed ได้แค่ 2-8 จาก viewport (ใช้ event_id dedup แทน)

### Gemini ✅
- user: `div.query-text.gds-body-l`
- asst: `model-response`
- strip user: `^คุณบอกว่า\s*` / `^You said\s*`
- strip asst: `^Gemini บอกว่า\s*` / `^Gemini said\s*`
- verified: 11:11 pairs

### Claude ✅
- container: `div.flex-1.flex.flex-col.px-4.max-w-3xl`
- user: `[class*="font-user-message"]` (top-level)
- asst: `[class*="font-claude-response"]` (top-level, filter nested)
- strip: `^(Viewed|Running|Used|Called)\s[^\n]*\n`
- method: `captureDOMOrder()` — DOM order (50+ pairs verified)

### Copilot ⏳ not verified

---

## Key Fixes This Session

### event_id (dedup)
```python
event_id = hash(platform + Q[:80])  # content-based, no timestamp
# same Q = same event_id → bridge blocks duplicate
```

### parent_id (chain of thought)
```python
# auto-link if time_gap < SESSION_IDLE_SEC (30 min)
pair.parent_id = last_pair_by_platform[platform].event_id
```

### Session boundary
```python
SESSION_IDLE_SEC = 1800  # 30 min idle → new session_id
# new session resets parent chain
```

### Pre-seed (capture NEW only)
```javascript
// extension: pre-seed sent Set with existing pairs
// then activate MutationObserver
// → only new messages captured after load
// DeepSeek: virtual scroll → only 2-8 pre-seeded
// → event_id dedup handles the rest
```

### Noise filter (clipboard)
```python
CODE_SIGNS = [
    terminal logs: "[WS] client", "🆕 New session", "websocket→fabric"
    POGLS context: "%%POGLS_INJECTED%%", "── POGLS Memory Context"
    time-prefixed: regex r'^\d{2}:\d{2}:\d{2}\s+'
]
```

### DeepSeek think block
```javascript
const thinkBlock = el.querySelector("._74c0879")
const allMd = [...el.querySelectorAll(".ds-markdown")]
const actual = allMd.filter(m => !thinkBlock?.contains(m))
// actual[0] = real response
```

---

## θ-Platform Mapping
```
chatgpt  → θ=0.0°    addr zone: 0
deepseek → θ=72.0°   addr zone: 209,715
gemini   → θ=144.0°  addr zone: 419,430
copilot  → θ=216.0°  addr zone: 629,145
claude   → θ=288.0°  addr zone: 838,860
```

---

## Run Instructions
```bash
pip install websockets pyperclip
# Terminal 1
python pogls_gui.pyw
# Terminal 2
python -m pogls_chat_bridge.run
# Chrome: Load unpacked → pogls_chat_bridge/extension/
```

---

## Files Delivered This Session
```
pogls_chat_bridge_v1.1.zip   — full bridge package
pogls_extension.zip          — Chrome extension
all_channels.py              — noise filter updated
bridge_store.py              — 60KB truncation fix
```

---

## Known Issues
- DeepSeek virtual scroll → pre-seed แค่ 2-8 pairs (event_id dedup ช่วย)
- DeepSeek Thinking OFF vs ON → DOM structure ต่าง (filterAsst handle ทั้งสอง)
- Copilot ยังไม่ verify
- Tab cross-contamination: ปิด tab อื่นถ้าจะ capture platform เดียว

---

## Next Steps for Chat Bridge
1. Copilot DOM verify
2. Memory scoring (Phase 1) — rank pairs before inject
3. Semantic index (Phase 1) — query by meaning
4. Memory replay UI (Phase 2) — timeline viewer in popup
5. Copcon integration — inject context into agent sessions
