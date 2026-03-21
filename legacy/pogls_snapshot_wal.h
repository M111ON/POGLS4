/*
 * pogls_snapshot_wal.h — Snapshot Integrity Layer (WAL Stage 2)
 *
 * Design:
 *   Snapshot = point-in-time freeze of WAL state.
 *   On-disk format:
 *     [SNAP_MAGIC:4][version:1][pad:3][last_seq:8][record_count:8][sha256:32] = 56B header
 *     followed by raw data payload (all committed data, in seq order)
 *
 *   Hash = SHA-256 of entire payload region (incremental via streaming).
 *   Trust anchor: load verifies hash → fail-fast on mismatch.
 *
 * Recovery hierarchy:
 *   1. Load snapshot → verify hash → restore state
 *   2. Replay WAL from last_seq+1 → bring state current
 */

#ifndef POGLS_SNAPSHOT_WAL_H
#define POGLS_SNAPSHOT_WAL_H

#include <stdint.h>
#include <stddef.h>

/* ── SHA-256 (self-contained, no external dep) ────────────────────────── */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
    uint32_t buf_len;
} SHA256_Ctx;

void     sha256_init  (SHA256_Ctx *c);
void     sha256_update(SHA256_Ctx *c, const void *data, size_t len);
void     sha256_final (SHA256_Ctx *c, uint8_t out[32]);
/* One-shot convenience */
void     sha256_digest(const void *data, size_t len, uint8_t out[32]);

/* ── On-disk snapshot header (packed, 56 bytes) ───────────────────────── */
#define SNAP_MAGIC    0x534E4150u   /* "SNAP" */
#define SNAP_VERSION  0x01

typedef struct __attribute__((packed)) {
    uint32_t magic;           /* SNAP_MAGIC                               */
    uint8_t  version;         /* SNAP_VERSION                             */
    uint8_t  _pad[3];
    uint64_t last_seq;        /* Highest committed seq included           */
    uint64_t record_count;    /* Number of data records in payload        */
    uint8_t  payload_hash[32];/* SHA-256 of payload region                */
} SNAP_Header;
/* sizeof = 4+1+3+8+8+32 = 56 bytes */

/* ── In-memory snapshot writer context ───────────────────────────────── */
typedef struct {
    int        snap_fd;       /* Snapshot file descriptor                 */
    SHA256_Ctx hash_ctx;      /* Incremental hash — updated on each append*/
    uint64_t   last_seq;      /* Highest seq written so far               */
    uint64_t   record_count;  /* Records appended                         */
    int        finalized;     /* 1 = snap_finalize() called               */
} SNAP_Writer;

/* ── API ──────────────────────────────────────────────────────────────── */

/* Begin a new snapshot. Seeks snap_fd to 0, reserves header space. */
int  snap_begin  (SNAP_Writer *w, int snap_fd);

/*
 * Append one committed record to snapshot.
 * Updates incremental SHA-256.
 * seq must be monotonically increasing.
 */
int  snap_append (SNAP_Writer *w, uint64_t seq,
                  uint64_t data_offset, const void *data, uint32_t size);

/*
 * Finalize: compute final hash, write header at offset 0.
 * After this, snap_fd holds a self-verifying snapshot file.
 */
int  snap_finalize(SNAP_Writer *w);

/*
 * Load & verify snapshot from snap_fd.
 * Fills out_header with metadata.
 * Calls record_cb for each record (in seq order) so caller can restore state.
 *
 * Returns:
 *   0  = OK, hash verified, all records delivered
 *  -1  = I/O error
 *  -2  = bad magic or version
 *  -3  = hash mismatch  ← fail-fast corruption sentinel
 */
typedef void (*snap_record_cb)(uint64_t seq, uint64_t data_offset,
                               const void *data, uint32_t size, void *ud);

int  snap_load(int snap_fd, SNAP_Header *out_header,
               snap_record_cb cb, void *userdata);

/*
 * Quick integrity check — returns 0 if hash OK, -3 if corrupt.
 * Does NOT invoke callback; useful as a pre-flight check.
 */
int  snap_verify(int snap_fd);

#endif /* POGLS_SNAPSHOT_WAL_H */
