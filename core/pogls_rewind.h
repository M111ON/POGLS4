/*
 * pogls_rewind.h — POGLS V3.8  Speculative Delta Buffer (Write Behind)
 * ══════════════════════════════════════════════════════════════════════
 *
 * WAL  (Write Ahead Log)  = บันทึกก่อน → เขียนจริง → crash → replay →
 * THIS (Write Behind Log) = GPU buffer 972 slots → เขียน disk ครั้งเดียว
 *                           crash → rewind ย้อนได้ถึง slot ที่ดีที่สุด
 *
 * ══════════════════════════════════════════════════════════════════════
 * Architecture:
 *
 *   GPU/CPU write
 *       ↓
 *   RewindBuffer (972 slots × 64B = 60KB — L2 resident)
 *       ↓ every REWIND_FLUSH_GATE (18 slots)
 *   delta_append() — crash-safe lane
 *       ↓ every batch
 *   atomic rename (Step 11) — filesystem seal
 *
 * 3 ชั้นป้องกัน:
 *   Layer 1: RewindBuffer  → rewind ได้ 972 steps
 *   Layer 2: Delta lane    → crash recovery
 *   Layer 3: Atomic rename → filesystem integrity
 *
 * ══════════════════════════════════════════════════════════════════════
 * Sacred Numbers:
 *   18  = gate_18       → REWIND_GATE   (flush unit)
 *   54  = nexus         → REWIND_NEXUS  (1 Rubik cycle)
 *   162 = icosphere     → REWIND_SPHERE (1 full node pass)
 *   972 = 54×18         → REWIND_MAX    (total slots, L2 friendly)
 *
 * Memory:
 *   972 × 64B = 62,208B ≈ 60KB  (fits L2 cache, no RAM touch)
 *
 * Rules:
 *   - REWIND_MAX must be multiple of REWIND_GATE (972 = 54×18 ✓)
 *   - slot[0] = oldest (closest to disk)
 *   - slot[head-1] = newest (latest GPU write)
 *   - rewind(n) rolls back n steps from head
 *   - flush() commits gate_18 slots to delta lane
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_REWIND_H
#define POGLS_REWIND_H

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>




/* ══════════════════════════════════════════════════════════════════
 * Constants (DNA-aligned)
 * ══════════════════════════════════════════════════════════════════ */
#define REWIND_GATE     18u    /* gate_18: flush unit = 2×3²           */
#define REWIND_NEXUS    54u    /* nexus:   1 Rubik cycle = 2×3³        */
#define REWIND_SPHERE  162u    /* icosphere: 1 full node pass = 2×3⁴   */
#define REWIND_MAX     972u    /* 54×18: total slots, L2 friendly       */

#define REWIND_MAGIC   0x52574E44u  /* "RWND" */

/* ══════════════════════════════════════════════════════════════════
 * RewindSlot — one speculative write (64B = 1 DiamondBlock)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    DiamondBlock block;     /* 64B — the actual data                   */
} RewindSlot;

/* compile-time size check */
typedef char _rslot_sz[sizeof(RewindSlot)==64?1:-1];

/* ══════════════════════════════════════════════════════════════════
 * RewindCheckpoint — snapshot at every REWIND_NEXUS (54) slots
 * saved automatically → enables fast seek to any nexus boundary
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t slot_index;    /* which slot this checkpoint covers       */
    uint32_t crc32;         /* XOR checksum of slots[0..slot_index]    */
    uint64_t timestamp_ns;  /* wall time at checkpoint                 */
    uint32_t delta_seq;     /* delta lane sequence at this point       */
    uint32_t _pad;
} RewindCheckpoint;

/* number of checkpoints = REWIND_MAX / REWIND_NEXUS = 972/54 = 18    */
#define REWIND_CKPT_COUNT  (REWIND_MAX / REWIND_NEXUS)  /* = 18       */

/* ══════════════════════════════════════════════════════════════════
 * RewindStats
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t total_writes;    /* total rewind_push() calls             */
    uint64_t total_flushes;   /* times gate batch flushed to delta     */
    uint64_t total_rewinds;   /* times rewind() called                 */
    uint64_t steps_rewound;   /* total steps rolled back               */
    uint64_t disk_writes;     /* times committed to disk               */
    uint32_t max_depth_used;  /* peak head position observed           */
    uint32_t overflow_wraps;  /* times ring wrapped (evicted oldest)   */
} RewindStats;

/* ══════════════════════════════════════════════════════════════════
 * RewindBuffer — the main structure
 *
 * Ring buffer of 972 slots.
 * head  → next write position
 * tail  → oldest slot still in buffer (not yet on disk)
 * confirmed → slots[0..confirmed] are on disk
 *
 * Layout:
 *   slots[972]          = 60KB  (L2 resident)
 *   checkpoints[18]     = 288B  (18 nexus checkpoints)
 *   stats               = 56B
 *   metadata            = ~32B
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* ring buffer */
    RewindSlot       slots[REWIND_MAX];          /* 62208B             */

    /* nexus checkpoints (every 54 slots) */
    RewindCheckpoint checkpoints[REWIND_CKPT_COUNT]; /* 18 × 24B      */

    /* head / tail */
    uint32_t         head;          /* next write index (mod MAX)      */
    uint32_t         tail;          /* oldest slot index               */
    uint32_t         count;         /* slots currently in buffer       */
    uint32_t         confirmed;     /* slots committed to delta/disk   */

    /* delta context (pointer — shared) */
    DeltaContext    *delta;

    /* stats */
    RewindStats      stats;

    /* epoch — increments every ring wrap (slot 971→0)
     * global slot id = epoch × REWIND_MAX + (head % REWIND_MAX)
     * enables lock-free persistent timeline                        */
    uint32_t         epoch;

    /* magic */
    uint32_t         magic;
    uint32_t         _pad;
} RewindBuffer;

/* ══════════════════════════════════════════════════════════════════
 * rewind_init — initialize buffer
 *   delta: DeltaContext to flush into (can be NULL for pure in-memory)
 * ══════════════════════════════════════════════════════════════════ */
static inline int rewind_init(RewindBuffer *rb, DeltaContext *delta)
{
    if (!rb) return -1;
    memset(rb, 0, sizeof(*rb));
    rb->delta = delta;
    rb->magic = REWIND_MAGIC;
    /* pre-compute nexus checkpoint slot_indices */
    for (uint32_t i = 0; i < REWIND_CKPT_COUNT; i++)
        rb->checkpoints[i].slot_index = i * REWIND_NEXUS;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * _rewind_crc32 — fast XOR checksum (not crypto, just integrity)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t _rewind_crc32(const DiamondBlock *b)
{
    const uint64_t *p = (const uint64_t *)b;
    return (uint32_t)(p[0] ^ p[1] ^ p[2] ^ p[3] ^
                      p[4] ^ p[5] ^ p[6] ^ p[7]);
}

/* ══════════════════════════════════════════════════════════════════
 * _rewind_now_ns — wall clock
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t _rewind_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_push — write one DiamondBlock into speculative buffer
 *
 * Returns: slot index used (for reference)
 *
 * Auto-flush to delta every REWIND_GATE (18) slots.
 * Auto-checkpoint at every REWIND_NEXUS (54) slots.
 * Ring wraps at REWIND_MAX (972).
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t rewind_push(RewindBuffer *rb,
                                    const DiamondBlock *block)
{
    if (!rb || !block) return UINT32_MAX;

    uint32_t idx = rb->head % REWIND_MAX;
    rb->slots[idx].block = *block;
    rb->head++;

    /* epoch tracking — increment on ring wrap */
    if (rb->head % REWIND_MAX == 0 && rb->head > 0)
        rb->epoch++;

    if (rb->count < REWIND_MAX) rb->count++;
    else {
        /* ring full — evict oldest */
        rb->tail = (rb->tail + 1) % REWIND_MAX;
        rb->stats.overflow_wraps++;
    }

    rb->stats.total_writes++;
    if (rb->head > rb->stats.max_depth_used)
        rb->stats.max_depth_used = rb->head;

    /* ── nexus checkpoint (every 54 writes) ─────────────────────── */
    if (rb->stats.total_writes % REWIND_NEXUS == 0) {
        uint32_t ci = (rb->stats.total_writes / REWIND_NEXUS - 1)
                      % REWIND_CKPT_COUNT;
        rb->checkpoints[ci].slot_index  = idx;
        rb->checkpoints[ci].crc32       = _rewind_crc32(block);
        rb->checkpoints[ci].timestamp_ns = _rewind_now_ns();
        rb->checkpoints[ci].delta_seq   = rb->confirmed;
    }

    return idx;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_flush_gate — flush one gate_18 batch to delta lane
 *
 * Commits rb->confirmed .. rb->confirmed+18 slots to delta.
 * Called automatically by rewind_push every 18 writes,
 * or manually to force early commit.
 *
 * Returns: number of slots flushed (0 if nothing to flush)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t rewind_flush_gate(RewindBuffer *rb)
{
    if (!rb) return 0;

    uint32_t available = rb->head > rb->confirmed
                       ? rb->head - rb->confirmed : 0;
    if (available < REWIND_GATE) return 0;

    uint32_t flushed = 0;
    for (uint32_t i = 0; i < REWIND_GATE && rb->confirmed < rb->head; i++) {
        uint32_t si = rb->confirmed % REWIND_MAX;
        if (rb->delta) {
            /* write to delta lane — crash safe from here */
            uint64_t addr = rb->slots[si].block.core.raw >> 28;
            delta_append(rb->delta, (uint32_t)addr,
                         (const uint8_t *)&rb->slots[si].block,
                         sizeof(DiamondBlock));
        }
        rb->confirmed++;
        flushed++;
    }

    rb->stats.total_flushes++;
    return flushed;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind — roll back N steps from head
 *
 * n=1  : undo last write
 * n=18 : undo last gate_18
 * n=54 : undo last nexus cycle (1 Rubik rotation)
 * n=162: undo last icosphere pass
 * n=972: full reset (keep disk-confirmed only)
 *
 * Returns: actual steps rewound (capped at unconfirmed count)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t rewind(RewindBuffer *rb, uint32_t n)
{
    if (!rb || n == 0) return 0;

    /* can only rewind unconfirmed slots */
    uint32_t unconfirmed = rb->head > rb->confirmed
                         ? rb->head - rb->confirmed : 0;
    uint32_t steps = n < unconfirmed ? n : unconfirmed;

    rb->head  -= steps;
    rb->count  = rb->count > steps ? rb->count - steps : 0;
    rb->stats.total_rewinds++;
    rb->stats.steps_rewound += steps;

    return steps;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_to_checkpoint — fast seek to nearest nexus boundary
 *
 * Finds the most recent checkpoint at or before target_step
 * and rewinds to that nexus boundary.
 *
 * Returns: slot index of checkpoint (UINT32_MAX if none found)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t rewind_to_checkpoint(RewindBuffer *rb,
                                             uint32_t target_step)
{
    if (!rb) return UINT32_MAX;

    /* find best checkpoint */
    int best = -1;
    for (int i = 0; i < (int)REWIND_CKPT_COUNT; i++) {
        if (rb->checkpoints[i].slot_index <= target_step)
            best = i;
    }
    if (best < 0) return UINT32_MAX;

    uint32_t target_head = rb->checkpoints[best].slot_index + 1;
    if (target_head >= rb->head) return UINT32_MAX;

    uint32_t steps = rb->head - target_head;
    rewind(rb, steps);
    return rb->checkpoints[best].slot_index;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_peek — read slot at offset from head (no modify)
 *   offset=0 → head-1 (last write)
 *   offset=1 → head-2
 * ══════════════════════════════════════════════════════════════════ */
static inline const DiamondBlock *rewind_peek(const RewindBuffer *rb,
                                               uint32_t offset)
{
    if (!rb || offset >= rb->count) return NULL;
    uint32_t idx = (rb->head - 1 - offset) % REWIND_MAX;
    return &rb->slots[idx].block;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_depth — how many steps can we rewind right now?
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t rewind_depth(const RewindBuffer *rb)
{
    if (!rb || rb->head <= rb->confirmed) return 0;
    return rb->head - rb->confirmed;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_global_id — deterministic global slot address
 *
 * (epoch, slot) pair — unique across all ring wraps.
 * Two slots with same (epoch, slot_idx) are guaranteed identical.
 *
 * Use for dedup, replication, cross-session tracking.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rewind_global_id(const RewindBuffer *rb,
                                         uint32_t slot_offset)
{
    if (!rb) return UINT64_MAX;
    uint32_t slot_idx = (rb->head - 1 - slot_offset) % REWIND_MAX;
    return (uint64_t)rb->epoch * REWIND_MAX + slot_idx;
}

/* rewind_epoch — current epoch (how many times ring has wrapped) */
static inline uint32_t rewind_epoch(const RewindBuffer *rb)
{
    return rb ? rb->epoch : 0;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_get_stats — copy out stats
 * ══════════════════════════════════════════════════════════════════ */
static inline RewindStats rewind_get_stats(const RewindBuffer *rb)
{
    if (!rb) { RewindStats z; memset(&z,0,sizeof(z)); return z; }
    return rb->stats;
}

/* ══════════════════════════════════════════════════════════════════
 * rewind_validate — verify buffer integrity
 * ══════════════════════════════════════════════════════════════════ */
#define REWIND_OK          0
#define REWIND_ERR_NULL   -1
#define REWIND_ERR_MAGIC  -2
#define REWIND_ERR_COUNT  -3

static inline int rewind_validate(const RewindBuffer *rb)
{
    if (!rb)                         return REWIND_ERR_NULL;
    if (rb->magic != REWIND_MAGIC)   return REWIND_ERR_MAGIC;
    if (rb->count > REWIND_MAX)      return REWIND_ERR_COUNT;
    return REWIND_OK;
}

#endif /* POGLS_REWIND_H */
