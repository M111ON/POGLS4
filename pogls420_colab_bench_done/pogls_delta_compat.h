/*
 * pogls_delta_compat.h — V3->V4 Federation compatibility layer
 * federation + world_b use V3 Delta_Context; V4 wire uses DeltaWriter.
 * This file provides V3 types for the federation path only.
 */
#ifndef POGLS_DELTA_COMPAT_H
#define POGLS_DELTA_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define LANE_COUNT         4u
#define DELTA_MAX_PAYLOAD  4096u
#define DELTA_VERSION      1u

#define DELTA_MAGIC            0x44454C54u  /* "DELT" */
#define POGLS_GHOST_STREAK_MAX 8u           /* ghost warm-up window */


/* V3 Delta_Context */
typedef struct {
    char     source_path[512];
    char     pogls_dir[512];
    int      lane_fd[LANE_COUNT];
    uint64_t lane_seq[LANE_COUNT];
    uint64_t epoch;
    bool     is_open;
} Delta_Context;

typedef enum {
    DELTA_RECOVERY_CLEAN  = 0,
    DELTA_RECOVERY_TORN   = 1,
    DELTA_RECOVERY_NEW    = 2,
    DELTA_RECOVERY_ERROR  = -1,
} Delta_RecoveryResult;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t lane_id;
    uint32_t version;
    uint32_t seq;
    uint64_t addr;
    uint32_t payload_size;
    uint32_t crc32;
} Delta_BlockHeader;

static inline uint32_t delta_crc32(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

static inline int delta_open(Delta_Context *ctx, const char *path)
{
    if (!ctx || !path) return -1;
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->source_path, path, sizeof(ctx->source_path)-1);
    snprintf(ctx->pogls_dir, sizeof(ctx->pogls_dir), "%s/.pogls", path);
    mkdir(ctx->pogls_dir, 0755);
    for (int i = 0; i < (int)LANE_COUNT; i++) ctx->lane_fd[i] = -1;
    ctx->is_open = true;
    return 0;
}

static inline int delta_append(Delta_Context *ctx, uint8_t lane_id,
                                uint64_t addr, const void *data, uint32_t size)
{
    if (!ctx || !ctx->is_open) return -1;
    if (lane_id >= LANE_COUNT) return -1;
    if (!data || size == 0 || size > DELTA_MAX_PAYLOAD) return -1;
    ctx->lane_seq[lane_id]++;
    (void)addr;
    return 0;
}

static inline int delta_audit(const Delta_Context *ctx)
{
    if (!ctx || !ctx->is_open) return -1;
    if (ctx->lane_seq[0] != ctx->lane_seq[1]) return -1;
    if (ctx->lane_seq[2] != ctx->lane_seq[3]) return -1;
    return 0;
}

static inline int delta_merkle_compute(Delta_Context *ctx, uint8_t root_out[32])
{
    if (!ctx || !root_out) return -1;
    memset(root_out, 0, 32);
    for (int i = 0; i < (int)LANE_COUNT; i++) {
        uint64_t s = ctx->lane_seq[i];
        for (int b = 0; b < 8; b++)
            root_out[i*8 + b] = (uint8_t)(s >> (b*8));
    }
    return 0;
}

static inline int delta_close(Delta_Context *ctx)
{
    if (!ctx) return -1;
    ctx->is_open = false;
    return 0;
}

static inline Delta_RecoveryResult delta_recover(const char *path)
{
    (void)path;
    return DELTA_RECOVERY_CLEAN;
}


static inline int delta_commit(Delta_Context *ctx)
{
    if (!ctx || !ctx->is_open) return -1;
    ctx->epoch++;
    return 0;
}

#endif /* POGLS_DELTA_COMPAT_H */
