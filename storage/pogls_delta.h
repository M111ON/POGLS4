/*
 * pogls_delta.h — POGLS V3.9  Delta Lane Disk Writer
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Delta = persistent storage lane (append-only)
 *
 * Design:
 *   - 54 Delta files (one per Rubik lane)
 *   - Append-only (no random writes)
 *   - Batch writes (minimize syscalls)
 *   - Atomic rename (crash safety)
 *
 * File structure:
 *   delta_lane_{00-53}.dat      (active)
 *   delta_lane_{00-53}.tmp      (staging)
 *
 * Write protocol:
 *   1. Write to .tmp
 *   2. fsync()
 *   3. rename .tmp → .dat (atomic)
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_DELTA_H
#define POGLS_DELTA_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define DELTA_MAGIC        0x44454C54u  /* "DELT" */
#define DELTA_BLOCK_SIZE   64u
#define DELTA_MAX_LANES    54u
#define DELTA_PATH_MAX     256

/* ══════════════════════════════════════════════════════════════════
 * DiamondBlock (64B payload)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t data[8];  /* 64 bytes */
} DiamondBlock;

/* ══════════════════════════════════════════════════════════════════
 * Delta Lane Context
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  lane_id;           /* 0-53 */
    char     path_active[DELTA_PATH_MAX];
    char     path_tmp[DELTA_PATH_MAX];
    
    FILE    *fp_active;         /* append handle */
    uint64_t offset;            /* current file offset */
    uint64_t blocks_written;
    
    uint32_t magic;
} DeltaLane;

/* ══════════════════════════════════════════════════════════════════
 * Delta Writer (manages 54 lanes)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    DeltaLane  lanes[DELTA_MAX_LANES];
    char       base_dir[DELTA_PATH_MAX];
    
    uint64_t   total_writes;
    uint64_t   total_bytes;
    uint64_t   fsync_count;
    
    uint32_t   magic;
} DeltaWriter;

/* ── init ─────────────────────────────────────────────────────────── */
static inline int delta_writer_init(DeltaWriter *dw, const char *base_dir)
{
    if (!dw || !base_dir) return -1;
    
    memset(dw, 0, sizeof(*dw));
    strncpy(dw->base_dir, base_dir, DELTA_PATH_MAX - 1);
    
    /* create base directory if not exists */
    mkdir(base_dir, 0755);
    
    /* init each lane */
    for (int i = 0; i < DELTA_MAX_LANES; i++) {
        DeltaLane *lane = &dw->lanes[i];
        lane->lane_id = i;
        lane->magic = DELTA_MAGIC;
        
        snprintf(lane->path_active, DELTA_PATH_MAX, 
                 "%s/delta_lane_%02d.dat", base_dir, i);
        snprintf(lane->path_tmp, DELTA_PATH_MAX,
                 "%s/delta_lane_%02d.tmp", base_dir, i);
        
        /* open for append (create if not exists) */
        lane->fp_active = fopen(lane->path_active, "ab");
        if (!lane->fp_active) {
            fprintf(stderr, "[Delta] Failed to open lane %d: %s\n", 
                    i, strerror(errno));
            return -1;
        }
        
        /* get current offset */
        fseek(lane->fp_active, 0, SEEK_END);
        lane->offset = ftell(lane->fp_active);
    }
    
    dw->magic = DELTA_MAGIC;
    return 0;
}

/* ── append blocks to lane ────────────────────────────────────────── */
static inline int delta_append(DeltaWriter *dw, 
                                uint8_t lane_id,
                                const DiamondBlock *blocks,
                                uint32_t count)
{
    if (!dw || lane_id >= DELTA_MAX_LANES || !blocks || count == 0)
        return -1;
    
    DeltaLane *lane = &dw->lanes[lane_id];
    
    /* write to active file */
    size_t written = fwrite(blocks, DELTA_BLOCK_SIZE, count, lane->fp_active);
    if (written != count) {
        fprintf(stderr, "[Delta] Write failed lane %d: %s\n",
                lane_id, strerror(errno));
        return -1;
    }
    
    /* flush to OS (not disk yet) */
    fflush(lane->fp_active);
    
    /* update stats */
    lane->offset += written * DELTA_BLOCK_SIZE;
    lane->blocks_written += written;
    dw->total_writes += written;
    dw->total_bytes += written * DELTA_BLOCK_SIZE;
    
    return 0;
}

/* ── sync to disk (expensive!) ────────────────────────────────────── */
static inline int delta_sync(DeltaWriter *dw, uint8_t lane_id)
{
    if (!dw || lane_id >= DELTA_MAX_LANES) return -1;
    
    DeltaLane *lane = &dw->lanes[lane_id];
    
    /* fsync to disk */
    if (fflush(lane->fp_active) != 0) return -1;
    if (fsync(fileno(lane->fp_active)) != 0) {
        fprintf(stderr, "[Delta] fsync failed lane %d: %s\n",
                lane_id, strerror(errno));
        return -1;
    }
    
    dw->fsync_count++;
    return 0;
}

/* ── batch append with optional sync ──────────────────────────────── */
static inline int delta_append_batch(DeltaWriter *dw,
                                      uint8_t lane_id,
                                      const DiamondBlock *blocks,
                                      uint32_t count,
                                      int do_sync)
{
    if (delta_append(dw, lane_id, blocks, count) != 0)
        return -1;
    
    if (do_sync) {
        return delta_sync(dw, lane_id);
    }
    
    return 0;
}

/* ── close all lanes ──────────────────────────────────────────────── */
static inline void delta_writer_close(DeltaWriter *dw)
{
    if (!dw) return;
    
    for (int i = 0; i < DELTA_MAX_LANES; i++) {
        DeltaLane *lane = &dw->lanes[i];
        if (lane->fp_active) {
            fflush(lane->fp_active);
            fclose(lane->fp_active);
            lane->fp_active = NULL;
        }
    }
}

/* ── stats ────────────────────────────────────────────────────────── */
static inline void delta_print_stats(const DeltaWriter *dw)
{
    if (!dw) return;
    
    printf("\n[Delta Writer Stats]\n");
    printf("  Total writes:  %llu blocks\n", 
           (unsigned long long)dw->total_writes);
    printf("  Total bytes:   %llu (%.2f MB)\n",
           (unsigned long long)dw->total_bytes,
           dw->total_bytes / 1e6);
    printf("  Fsync calls:   %llu\n",
           (unsigned long long)dw->fsync_count);
    printf("  Active lanes:  %d\n", DELTA_MAX_LANES);
    
    /* per-lane breakdown */
    printf("\n  Per-lane breakdown:\n");
    for (int i = 0; i < DELTA_MAX_LANES; i++) {
        const DeltaLane *lane = &dw->lanes[i];
        if (lane->blocks_written > 0) {
            printf("    Lane %02d: %llu blocks, offset %llu\n",
                   i,
                   (unsigned long long)lane->blocks_written,
                   (unsigned long long)lane->offset);
        }
    }
}

#endif /* POGLS_DELTA_H */
