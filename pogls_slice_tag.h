/*
 * pogls_slice_tag.h — POGLS V4  EngineSlice → WireBlock tagging
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Tags each WireBlock with its origin EngineSlice (0, 1, or 2).
 * Enables Mesh to trace anomalies back to the slice that produced them.
 *
 * WireBlock.data[5] layout (was: reserved zeros):
 *   bits  7- 0  : slice_id (0..2)
 *   bits 15- 8  : ghost hop count (0..2, from EngineSlice.hop_count)
 *   bits 23-16  : engine_id (mirrors slice_id for clarity)
 *   bits 31-24  : reserved (future: lane_id)
 *   bits 63-32  : op_seq mod 2^32 (monotonic per slice)
 *
 * Rules:
 *   - NEVER modifies existing WireBlock fields (data[0..4] frozen)
 *   - slice_tag_block() is the ONLY write point
 *   - slice_read_tag() is the ONLY read point
 *   - No allocation, no malloc, O(1)
 *
 * Integration (pogls_pipeline_wire.h):
 *   [A] Add to PipelineWire struct:
 *         EngineSliceSet slices;
 *   [B] In pipeline_wire_init():
 *         slice_set_init(&pw->slices);
 *   [C] In pipeline_wire_process(), after WireBlock blk init:
 *         uint8_t sid = slice_of_lane(lane);
 *         slice_tag_block(&blk, sid, pw->slices.slices[sid].hop_count,
 *                         (uint32_t)pw->total_in);
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_SLICE_TAG_H
#define POGLS_SLICE_TAG_H

#include <stdint.h>
#include <string.h>

/* ── WireBlock (mirrors pipeline_wire.h definition) ─────────────── */
#ifndef WIRE_BLOCK_DEFINED
#define WIRE_BLOCK_DEFINED
typedef struct { uint64_t data[8]; } WireBlock;  /* 64B */
#endif

/* ── EngineSlice constants (mirrors pogls_engine_slice.h) ────────── */
#ifndef SLICE_LANE_WIDTH
#  define SLICE_LANE_WIDTH   18u
#  define SLICE_COUNT         3u
#  define SLICE_GHOST_OFFSET 27u
#  define SLICE_TOTAL_LANES  54u
#endif

/* WireBlock.data[5] is the slice tag word (was reserved zeros) */
#define SLICE_TAG_WORD   5u

/* bit layout within data[5] low 32 bits */
#define SLICE_TAG_ID_SHIFT      0u
#define SLICE_TAG_HOPS_SHIFT    8u
#define SLICE_TAG_EID_SHIFT    16u
#define SLICE_TAG_RSVD_SHIFT   24u
/* high 32 bits = op_seq */
#define SLICE_TAG_SEQ_SHIFT    32u

/* ══════════════════════════════════════════════════════════════════
 * slice_of_lane — which slice owns this lane?  O(1) divide
 * ══════════════════════════════════════════════════════════════════ */
static inline uint8_t slice_of_lane(uint8_t lane)
{
    return (uint8_t)(lane / SLICE_LANE_WIDTH);  /* 0,1,2 */
}

/* ghost destination slice */
static inline uint8_t slice_ghost_of_lane(uint8_t lane)
{
    return slice_of_lane((uint8_t)((lane + SLICE_GHOST_OFFSET) % SLICE_TOTAL_LANES));
}

/* ══════════════════════════════════════════════════════════════════
 * slice_tag_block — write slice tag into WireBlock.data[5]
 *
 * slice_id  : 0..2
 * hop_count : ghost hop count (0 = not a ghost, 1 = one hop, 2 = two)
 * op_seq    : monotonic counter (pw->total_in & 0xFFFFFFFF)
 * ══════════════════════════════════════════════════════════════════ */
static inline void slice_tag_block(WireBlock *blk,
                                    uint8_t    slice_id,
                                    uint8_t    hop_count,
                                    uint32_t   op_seq)
{
    if (!blk) return;
    uint64_t tag =
        ((uint64_t)(slice_id   & 0xFFu) << SLICE_TAG_ID_SHIFT)
      | ((uint64_t)(hop_count  & 0xFFu) << SLICE_TAG_HOPS_SHIFT)
      | ((uint64_t)(slice_id   & 0xFFu) << SLICE_TAG_EID_SHIFT)
      | ((uint64_t)(op_seq           )  << SLICE_TAG_SEQ_SHIFT);
    blk->data[SLICE_TAG_WORD] = tag;
}

/* ══════════════════════════════════════════════════════════════════
 * SliceTag — decoded tag struct (for reading)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  slice_id;
    uint8_t  hop_count;
    uint8_t  engine_id;
    uint32_t op_seq;
    uint8_t  valid;      /* 1 if tag was written (data[5] != 0) */
} SliceTag;

static inline SliceTag slice_read_tag(const WireBlock *blk)
{
    SliceTag t; memset(&t, 0, sizeof(t));
    if (!blk) return t;
    uint64_t raw = blk->data[SLICE_TAG_WORD];
    if (raw == 0) return t;   /* untagged block */
    t.slice_id  = (uint8_t)((raw >> SLICE_TAG_ID_SHIFT)   & 0xFFu);
    t.hop_count = (uint8_t)((raw >> SLICE_TAG_HOPS_SHIFT) & 0xFFu);
    t.engine_id = (uint8_t)((raw >> SLICE_TAG_EID_SHIFT)  & 0xFFu);
    t.op_seq    = (uint32_t)(raw >> SLICE_TAG_SEQ_SHIFT);
    t.valid     = 1;
    return t;
}

/* ── convenience: is this block a ghost (hop_count > 0)? ─────────── */
static inline int slice_block_is_ghost(const WireBlock *blk)
{
    if (!blk) return 0;
    return ((blk->data[SLICE_TAG_WORD] >> SLICE_TAG_HOPS_SHIFT) & 0xFFu) > 0u;
}

/* ── convenience: extract slice_id fast (no full decode) ─────────── */
static inline uint8_t slice_block_id(const WireBlock *blk)
{
    if (!blk) return 0;
    return (uint8_t)(blk->data[SLICE_TAG_WORD] & 0xFFu);
}

#endif /* POGLS_SLICE_TAG_H */
