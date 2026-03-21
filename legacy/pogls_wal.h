/*
 * pogls_wal.h — Write-Ahead Log for POGLS V3.1
 *
 * Record format:
 *   INTENT  = WAL_RecordHeader (32B) + data payload (data_size bytes)
 *   COMMIT  = WAL_RecordHeader (32B) only
 *   ABORT   = WAL_RecordHeader (32B) only
 *
 * Integrity: CRC32 in header covers (header fields excl. crc32) + payload.
 * Durability: fdatasync after INTENT, fdatasync after COMMIT.
 * Replay: two-pass — pass1 collect committed seqs, pass2 apply INTENTs.
 */

#ifndef POGLS_WAL_H
#define POGLS_WAL_H

#include <stdint.h>
#include <stddef.h>

#define WAL_TYPE_INTENT  0x01
#define WAL_TYPE_COMMIT  0x02
#define WAL_TYPE_ABORT   0x03
#define WAL_MAGIC        0x57414C50u   /* "WALP" */
#define WAL_MAX_PAYLOAD  4096u

/* On-disk record header — 32 bytes (packed) */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* WAL_MAGIC                                     */
    uint8_t  type;        /* WAL_TYPE_INTENT / COMMIT / ABORT              */
    uint8_t  _pad[3];
    uint64_t seq;         /* Monotonic sequence number                     */
    uint64_t data_offset; /* Destination offset in data file (INTENT only) */
    uint32_t data_size;   /* Payload bytes after this header (0 = none)    */
    uint32_t crc32;       /* CRC32 of (header bytes 0..27) + payload       */
} WAL_RecordHeader;
/* sizeof = 4+1+3+8+8+4+4 = 32 bytes */

typedef struct {
    int      wal_fd;
    int      data_fd;
    uint64_t next_seq;   /* restored from WAL on wal_open */
} WAL_Context;

/* Open/create WAL — scans file to restore next_seq. */
int     wal_open(WAL_Context *ctx, int wal_fd, int data_fd);

/* Write INTENT + fdatasync. Returns seq on success, -1 on error. */
int64_t wal_write_intent(WAL_Context *ctx, uint64_t data_offset,
                         const void *data, uint32_t size);

/* Write COMMIT + fdatasync. Returns 0/-1. */
int     wal_write_commit(WAL_Context *ctx, uint64_t seq);

/* Write ABORT. Returns 0/-1. */
int     wal_write_abort(WAL_Context *ctx, uint64_t seq);

/* Two-pass replay with CRC verification. Returns replayed count or -1. */
int     wal_replay(WAL_Context *ctx);

/* Truncate WAL after checkpoint. */
int     wal_truncate(WAL_Context *ctx);

/* CRC32 (exposed for testing) */
uint32_t wal_crc32(uint32_t crc, const void *data, size_t len);

#endif /* POGLS_WAL_H */
