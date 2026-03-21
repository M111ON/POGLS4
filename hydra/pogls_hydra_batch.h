/*
 * pogls_hydra_batch.h — POGLS V4  Hydra Batch-4 Commit Scheduler
 * ══════════════════════════════════════════════════════════════════════
 *
 * Problem (from benchmark B):
 *   DynamicHydra dispatches 1 op per call → function call overhead
 *   dominates at high throughput. 10M calls × ~5ns overhead = 50ms lost.
 *
 * Solution: accumulate 4 ops per head before dispatch → batch commit.
 *   - 4 ops fits in one cache line read (4 × 16B = 64B)
 *   - Compiler can unroll/pipeline the 4 pv2() calls (bench B pattern)
 *   - Delta writer already batches at 256 (WIRE_BATCH) — this feeds it
 *     in aligned 4-groups instead of 1-at-a-time
 *
 * Architecture:
 *
 *   Producer (any thread)
 *       │
 *   hydra_batch_push(hb, head_id, value, addr)
 *       │  accumulates into per-head slot buffer [4 entries]
 *       │  when slot hits 4 → calls dispatch_cb(batch_of_4)
 *       │
 *   Dispatch callback → pipeline_wire_process() × 4  (unrolled)
 *       │
 *   WireDelta lanes  (existing WIRE_BATCH=256 buffer absorbs)
 *
 * Flush:
 *   hydra_batch_flush_head(hb, head_id) — flush partial slot (< 4 ops)
 *   hydra_batch_flush_all(hb)           — flush all heads
 *   Called before pipeline_wire_close().
 *
 * Constants (FROZEN):
 *   HB_BATCH_SIZE = 4    — benchmark B optimal (CPU unroll window)
 *   HB_MAX_HEADS  = 32   — matches DH_MAX_HEADS
 *   HBEntry       = 16B  (value:8 + addr:8)
 *   HBSlot        = 64B  (4 × HBEntry — exactly one cache line)
 *
 * Design rules:
 *   - Never touch DynamicHydra internals — wraps on top
 *   - dispatch_cb signature matches pipeline_wire_process arguments
 *   - Partial flush (< 4 ops) dispatches each op individually
 *     (no padding — no fake writes)
 *   - Thread safety: one slot per head, caller serializes per head
 *     (same guarantee as dh_push — single producer per head)
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_HYDRA_BATCH_H
#define POGLS_HYDRA_BATCH_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── dimensions (FROZEN) ─────────────────────────────────────────── */
#define HB_BATCH_SIZE   4u    /* ops per commit burst                  */
#define HB_MAX_HEADS   32u    /* mirrors DH_MAX_HEADS                  */

/* ══════════════════════════════════════════════════════════════════
 * HBEntry — single op (16B, two uint64_t)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t  value;        /* data value (passed to pipeline)         */
    uint64_t  angular_addr; /* angular address (passed to pipeline)    */
} HBEntry;                  /* 16B                                     */

/* compile-time size check */
typedef char _hbentry_sz[(sizeof(HBEntry) == 16u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * HBSlot — per-head accumulator (64B = one cache line)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    HBEntry  entries[HB_BATCH_SIZE];  /* 4 × 16B = 64B                 */
    uint8_t  count;                   /* 0..4                           */
    uint8_t  active;                  /* 1 if head is enabled           */
    uint16_t _pad;
    uint32_t head_id;                 /* which head owns this slot      */
} __attribute__((aligned(64))) HBSlot; /* force cache-line alignment    */

/* compile-time size check (entries + count/active/pad/head_id = 68B, padded to 128B)  */
/* Note: aligned(64) pads struct to 128B — acceptable, alignment is the goal */

/* ══════════════════════════════════════════════════════════════════
 * Dispatch callback type
 *
 * Called with a full batch (count == HB_BATCH_SIZE) or partial
 * (count < HB_BATCH_SIZE, on flush).
 *
 * Signature matches pipeline_wire_process() per entry.
 * ctx = PipelineWire* (or any user context).
 * ══════════════════════════════════════════════════════════════════ */
typedef void (*HBDispatchCb)(const HBEntry *entries,
                             uint32_t       count,
                             uint32_t       head_id,
                             void          *ctx);

/* ══════════════════════════════════════════════════════════════════
 * HydraBatch — the scheduler
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    HBSlot       slots[HB_MAX_HEADS];    /* per-head accumulators       */
    HBDispatchCb dispatch_cb;            /* called on full batch         */
    void        *dispatch_ctx;           /* ctx passed to cb             */

    /* stats */
    uint64_t  total_pushed;     /* total ops pushed                     */
    uint64_t  full_batches;     /* times a 4-batch was dispatched       */
    uint64_t  partial_flushes;  /* times a partial (< 4) was flushed   */
    uint64_t  total_dispatched; /* total ops dispatched                 */

    uint32_t  magic;
} HydraBatch;

#define HYDRA_BATCH_MAGIC  0x48425443u   /* "HBTC" */

/* ── internal dispatch ───────────────────────────────────────────── */
static inline void _hb_dispatch(HydraBatch *hb, HBSlot *slot, int is_full)
{
    if (!hb->dispatch_cb || slot->count == 0) return;

    hb->dispatch_cb(slot->entries, slot->count, slot->head_id,
                    hb->dispatch_ctx);
    hb->total_dispatched += slot->count;

    if (is_full) hb->full_batches++;
    else         hb->partial_flushes++;

    slot->count = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * hydra_batch_init
 * ══════════════════════════════════════════════════════════════════ */
static inline int hydra_batch_init(HydraBatch   *hb,
                                   HBDispatchCb  cb,
                                   void         *ctx)
{
    if (!hb) return -1;
    memset(hb, 0, sizeof(*hb));
    hb->dispatch_cb  = cb;
    hb->dispatch_ctx = ctx;
    hb->magic        = HYDRA_BATCH_MAGIC;

    for (uint32_t i = 0; i < HB_MAX_HEADS; i++) {
        hb->slots[i].head_id = i;
        hb->slots[i].active  = 0;
        hb->slots[i].count   = 0;
    }
    return 0;
}

/* ── activate / deactivate a head ────────────────────────────────── */
static inline void hydra_batch_activate(HydraBatch *hb, uint32_t head_id)
{
    if (!hb || head_id >= HB_MAX_HEADS) return;
    hb->slots[head_id].active = 1;
}

static inline void hydra_batch_deactivate(HydraBatch *hb, uint32_t head_id)
{
    if (!hb || head_id >= HB_MAX_HEADS) return;
    /* flush before deactivating */
    HBSlot *s = &hb->slots[head_id];
    if (s->count > 0) _hb_dispatch(hb, s, 0);
    s->active = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * hydra_batch_push — hot path
 *
 * Adds one op to the head's slot.
 * When slot reaches HB_BATCH_SIZE (4), dispatches immediately.
 *
 * Returns:
 *   1 = pushed (slot not full yet)
 *   2 = pushed + batch dispatched (slot was full)
 *   0 = head not active or invalid
 * ══════════════════════════════════════════════════════════════════ */
static inline int hydra_batch_push(HydraBatch *hb,
                                   uint32_t    head_id,
                                   uint64_t    value,
                                   uint64_t    angular_addr)
{
    if (!hb || head_id >= HB_MAX_HEADS) return 0;
    HBSlot *s = &hb->slots[head_id];
    if (!s->active) return 0;

    /* append to slot */
    HBEntry *e    = &s->entries[s->count];
    e->value        = value;
    e->angular_addr = angular_addr;
    s->count++;
    hb->total_pushed++;

    /* full batch → dispatch immediately */
    if (s->count == HB_BATCH_SIZE) {
        _hb_dispatch(hb, s, 1);
        return 2;
    }
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * hydra_batch_flush_head — flush partial slot for one head
 * ══════════════════════════════════════════════════════════════════ */
static inline void hydra_batch_flush_head(HydraBatch *hb, uint32_t head_id)
{
    if (!hb || head_id >= HB_MAX_HEADS) return;
    HBSlot *s = &hb->slots[head_id];
    if (s->count > 0) _hb_dispatch(hb, s, 0);
}

/* ══════════════════════════════════════════════════════════════════
 * hydra_batch_flush_all — flush all active heads
 * Call before pipeline_wire_close().
 * ══════════════════════════════════════════════════════════════════ */
static inline void hydra_batch_flush_all(HydraBatch *hb)
{
    if (!hb) return;
    for (uint32_t i = 0; i < HB_MAX_HEADS; i++) {
        if (hb->slots[i].active && hb->slots[i].count > 0)
            _hb_dispatch(hb, &hb->slots[i], 0);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * hydra_batch_push_multi — push N ops to the same head
 *
 * Convenience: feed an array of HBEntry directly.
 * Internally calls hydra_batch_push() per entry.
 * Returns total ops dispatched (full batches only).
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t hydra_batch_push_multi(HydraBatch     *hb,
                                              uint32_t        head_id,
                                              const HBEntry  *ops,
                                              uint32_t        n)
{
    uint32_t dispatched = 0;
    for (uint32_t i = 0; i < n; i++) {
        int r = hydra_batch_push(hb, head_id, ops[i].value, ops[i].angular_addr);
        if (r == 2) dispatched += HB_BATCH_SIZE;
    }
    return dispatched;
}

/* ══════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════ */

/* pending ops for one head (0..HB_BATCH_SIZE-1) */
static inline uint32_t hydra_batch_pending(const HydraBatch *hb,
                                           uint32_t head_id)
{
    if (!hb || head_id >= HB_MAX_HEADS) return 0;
    return hb->slots[head_id].count;
}

/* total pending across all active heads */
static inline uint32_t hydra_batch_total_pending(const HydraBatch *hb)
{
    if (!hb) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < HB_MAX_HEADS; i++)
        if (hb->slots[i].active) n += hb->slots[i].count;
    return n;
}

/* efficiency: ratio of full batches to total dispatches (× 100) */
static inline uint32_t hydra_batch_efficiency_pct(const HydraBatch *hb)
{
    if (!hb) return 0;
    uint64_t total = hb->full_batches + hb->partial_flushes;
    if (total == 0) return 0;
    return (uint32_t)(hb->full_batches * 100u / total);
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void hydra_batch_stats(const HydraBatch *hb)
{
    if (!hb) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Hydra Batch-4 Commit Stats                     ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Pushed total:   %10llu                      ║\n",
           (unsigned long long)hb->total_pushed);
    printf("║ Full batches:   %10llu (× 4 ops each)       ║\n",
           (unsigned long long)hb->full_batches);
    printf("║ Partial flush:  %10llu                      ║\n",
           (unsigned long long)hb->partial_flushes);
    printf("║ Dispatched:     %10llu                      ║\n",
           (unsigned long long)hb->total_dispatched);
    printf("║ Efficiency:     %9u%%                      ║\n",
           hydra_batch_efficiency_pct(hb));
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_HYDRA_BATCH_H */
