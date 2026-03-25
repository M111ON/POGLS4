/*
 * pogls_fractal_gate.h — POGLS V3.9  Fractal Gate + Hilbert Bridge
 * ══════════════════════════════════════════════════════════════════════
 *
 * 1. Mandelbrot Fixed-Point (Split Depth Gatekeeper)
 *    z² + c ด้วย int64 >> 20 (zero-float, pure bit-shift)
 *    uses PHI_SCALE = 2²⁰ = 1,048,576 — exact POGLS constant
 *    iteration count → split depth decision
 *
 * 2. Hilbert Bridge (Morton → Hilbert for storage locality)
 *    RAM/GPU hot path: Morton (fast bitshift)
 *    Flush to disk:    Morton → Hilbert (sequential locality)
 *    Bridge via 8-bit LUT blocks (fits L2 cache)
 *
 * ══════════════════════════════════════════════════════════════════════
 * Integration:
 *   Step 1c (FaceState tick) → fractal_gate_check() → split depth
 *   Step 6  (delta_append)  → hilbert_from_morton()  → disk addr
 *
 * Sacred Numbers:
 *   2²⁰ = 1,048,576 = PHI_SCALE  ← Mandelbrot scaling factor
 *   |z| > 2 → escape (in fixed-point: |z|² > 4 × 2²⁰)
 *   ESCAPE_SQ = 4 × 2²⁰ = 4,194,304
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_FRACTAL_GATE_H
#define POGLS_FRACTAL_GATE_H

#include <stdint.h>
#include <string.h>

/* ── PHI_SCALE (from POGLS core law) ────────────────────────────── */
#ifndef FRACTAL_PHI_SCALE
  #define FRACTAL_PHI_SCALE   (1u << 20)   /* 2²⁰ = 1,048,576        */
#endif

/* ══════════════════════════════════════════════════════════════════
 * PART 1: MANDELBROT FIXED-POINT GATE
 * ══════════════════════════════════════════════════════════════════
 *
 * Standard:  zₙ₊₁ = zₙ² + c  (complex, float)
 * POGLS:     z_real = ((z_r² - z_i²) >> 20) + c_r
 *            z_imag = ((2 × z_r × z_i) >> 20) + c_i
 *
 * Escape:    |z|² > 4  →  in fixed-point: z_r² + z_i² > 4 × 2²⁰
 * ══════════════════════════════════════════════════════════════════ */

#define FRACTAL_ESCAPE_SQ     (4u * FRACTAL_PHI_SCALE)  /* 4,194,304 */
#define FRACTAL_MAX_ITER       162u   /* max = NODE_MAX (icosphere)   */
#define FRACTAL_SPLIT_THRESH    54u   /* iter > 54 → split candidate  */
#define FRACTAL_GHOST_THRESH     9u   /* iter < 9  → ghost candidate  */

/*
 * fractal_iterate — one Mandelbrot step (fixed-point)
 * Returns: new z_real, z_imag via pointers
 *          returns 1 if escaped, 0 if still bounded
 */
static inline int fractal_iterate(int64_t *z_r, int64_t *z_i,
                                   int64_t   c_r, int64_t c_i)
{
    int64_t zr = *z_r, zi = *z_i;

    /* z_r_new = (zr² - zi²) >> 20 + c_r */
    int64_t zr2  = (zr * zr) >> 20;
    int64_t zi2  = (zi * zi) >> 20;
    int64_t new_r = zr2 - zi2 + c_r;

    /* z_i_new = (2 × zr × zi) >> 20 + c_i */
    int64_t new_i = ((2 * zr * zi) >> 20) + c_i;

    *z_r = new_r;
    *z_i = new_i;

    /* escape check: |z|² > 4 × PHI_SCALE */
    return (zr2 + zi2) > (int64_t)FRACTAL_ESCAPE_SQ;
}

/*
 * fractal_gate_check — compute iteration depth for addr
 *
 * Maps addr → c_r, c_i in Mandelbrot space using angular addressing:
 *   c_r = (addr × PHI_UP) >> 20  (mod PHI_SCALE)
 *   c_i = (addr × PHI_DOWN) >> 20
 *
 * Returns: iteration count (0..FRACTAL_MAX_ITER)
 *   high iter → complex region → split candidate
 *   low iter  → simple region  → ghost candidate
 */
static inline uint32_t fractal_gate_check(uint32_t addr)
{
    /* map addr → Mandelbrot window [-2, 0.5] × [-1.25, 1.25]
     * r_norm, i_norm ∈ [0, PHI_SCALE) via modular PHI walk
     * c_r = r_norm × 2.5 - 2×PHI_SCALE
     * c_i = i_norm × 2.5 - 1.25×PHI_SCALE                         */
#ifndef POGLS_PHI_CONSTANTS
#  include "../pogls_platform.h"
#endif
    int64_t r_norm = (int64_t)(((uint64_t)addr * POGLS_PHI_UP)   % FRACTAL_PHI_SCALE);
    int64_t i_norm = (int64_t)(((uint64_t)addr * POGLS_PHI_DOWN) % FRACTAL_PHI_SCALE);

    int64_t c_r = (r_norm * 5 / 2) - 2*(int64_t)FRACTAL_PHI_SCALE;
    int64_t c_i = (i_norm * 5 / 2) - (5*(int64_t)FRACTAL_PHI_SCALE / 4);

    int64_t z_r = 0, z_i = 0;
    uint32_t iter = 0;

    while (iter < FRACTAL_MAX_ITER) {
        if (fractal_iterate(&z_r, &z_i, c_r, c_i)) break;
        iter++;
    }
    return iter;
}

/*
 * fractal_split_depth — decide split depth from iteration count
 *
 *   iter > SPLIT_THRESH  → split (depth = (iter - thresh) / 18)
 *   iter < GHOST_THRESH  → ghost candidate
 *   else                 → normal
 *
 * Returns: suggested split depth (0 = no split)
 */
static inline uint32_t fractal_split_depth(uint32_t iter)
{
    if (iter < FRACTAL_GHOST_THRESH)  return 0;   /* ghost zone     */
    if (iter < FRACTAL_SPLIT_THRESH)  return 0;   /* normal zone    */
    /* above split threshold: 1 depth per 18 iterations (gate_18)   */
    return (iter - FRACTAL_SPLIT_THRESH) / 18u + 1u;
}

/*
 * fractal_is_ghost_zone — returns 1 if addr should be ghosted
 */
static inline int fractal_is_ghost_zone(uint32_t addr)
{
    return fractal_gate_check(addr) < FRACTAL_GHOST_THRESH;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2: HILBERT BRIDGE (Morton → Hilbert for disk locality)
 * ══════════════════════════════════════════════════════════════════
 *
 * Morton: fast bitshift interleave → RAM/GPU hot path
 * Hilbert: space-filling curve → better disk locality
 *
 * Bridge: 8-bit block LUT (256 entries = 256B, fits in 4 cache lines)
 *   block_id = morton >> 8
 *   local_id = morton & 0xFF
 *   hilbert  = (block_id << 8) | hilbert_lut[local_id]
 *
 * LUT is precomputed for 2D 4×4 blocks (16 cells, 8-bit index)
 * Fits L2 cache → zero disk I/O overhead
 * ══════════════════════════════════════════════════════════════════ */

/* 8-bit Hilbert LUT for 2D 4×4 = 16 cells
 * maps Morton index 0..15 → Hilbert index 0..15
 * pattern follows standard Hilbert U-curve             */
static const uint8_t HILBERT_LUT_4x4[16] = {
    0, 3, 4, 5,   /* row 0: Morton 0-3  → Hilbert order  */
    1, 2, 7, 6,   /* row 1: Morton 4-7  */
    14,13, 8, 9,  /* row 2: Morton 8-11 */
    15,12,11,10   /* row 3: Morton 12-15 */
};

/* extended 8-bit LUT (256 entries) built from 4×4 blocks */
typedef struct {
    uint8_t  lut[256];    /* 256B — fits 4 cache lines              */
    uint32_t magic;
} HilbertLUT;

#define HILBERT_LUT_MAGIC  0x48494C42u  /* "HILB" */

/*
 * hilbert_lut_build — precompute 256-entry LUT from 4×4 base
 * Call once at init. Result is L2-resident (256B).
 */
static inline void hilbert_lut_build(HilbertLUT *lut)
{
    if (!lut) return;
    /* tile the 4×4 pattern into 256 entries
     * 256 = 16 tiles × 16 cells per tile              */
    for (int tile = 0; tile < 16; tile++) {
        for (int cell = 0; cell < 16; cell++) {
            int morton_idx = tile * 16 + cell;
            /* within tile: use base LUT
             * tile offset: tile × 16 in hilbert space  */
            lut->lut[morton_idx] = (uint8_t)(tile * 16
                                   + HILBERT_LUT_4x4[cell]);
        }
    }
    lut->magic = HILBERT_LUT_MAGIC;
}

/*
 * hilbert_from_morton — convert Morton index → Hilbert index
 *
 * For small addresses (< 256): direct LUT lookup
 * For large addresses: block decomposition
 *   block_id = addr >> 8
 *   local_id = addr & 0xFF
 *   result   = (block_id << 8) | lut[local_id]
 *
 * Used in delta_append flush path (NOT in hot write path)
 */
static inline uint32_t hilbert_from_morton(const HilbertLUT *lut,
                                            uint32_t morton)
{
    if (!lut) return morton;   /* fallback: identity */
    uint32_t block = morton >> 8;
    uint32_t local = morton & 0xFF;
    return (block << 8) | lut->lut[local];
}

/*
 * hilbert_disk_addr — compute final disk address
 *   base:     base address of delta file
 *   morton:   Morton index from ComputeLUT
 *   slot_sz:  size per slot (typically 256B = DELTA_BLOCK_SIZE)
 */
static inline uint64_t hilbert_disk_addr(uint64_t          base,
                                          const HilbertLUT *lut,
                                          uint32_t          morton,
                                          uint32_t          slot_sz)
{
    uint32_t h = hilbert_from_morton(lut, morton);
    return base + (uint64_t)h * slot_sz;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 3: Combined FractalGate context
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    HilbertLUT  hilbert;       /* 256B LUT, L2 resident              */
    uint32_t    checks;        /* total fractal_gate_check() calls   */
    uint32_t    splits_triggered; /* times fractal suggested split   */
    uint32_t    ghosts_triggered; /* times fractal suggested ghost   */
    uint32_t    magic;
} FractalGate;

#define FRACTAL_GATE_MAGIC  0x46524354u  /* "FRCT" */

static inline int fractal_gate_init(FractalGate *fg)
{
    if (!fg) return -1;
    memset(fg, 0, sizeof(*fg));
    hilbert_lut_build(&fg->hilbert);
    fg->magic = FRACTAL_GATE_MAGIC;
    return 0;
}

/*
 * fractal_gate_advise — check addr and return advice
 *
 * Returns:
 *   0 = NORMAL
 *   negative = GHOST zone
 *   positive = split depth (1, 2, 3...)
 */
static inline int fractal_gate_advise(FractalGate *fg, uint32_t addr)
{
    if (!fg) return 0;
    fg->checks++;
    uint32_t iter  = fractal_gate_check(addr);
    uint32_t depth = fractal_split_depth(iter);

    if (iter < FRACTAL_GHOST_THRESH) {
        fg->ghosts_triggered++;
        return -1;   /* ghost zone */
    }
    if (depth > 0) {
        fg->splits_triggered++;
        return (int)depth;
    }
    return 0;   /* normal */
}

#endif /* POGLS_FRACTAL_GATE_H */
