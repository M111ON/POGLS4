/*
 * pogls_wal.c — Write-Ahead Log implementation for POGLS V3.1
 *
 * Changes vs previous version (from audit):
 *  1. CRC32 per record (header bytes 0..27 + payload) — detects bit-flip corruption
 *  2. fdatasync after INTENT write — power-loss durability
 *  3. fdatasync after COMMIT write — durable commit guarantee
 *  4. wal_open scans WAL to restore next_seq — safe reopen of existing WAL
 *  5. Header size comment corrected: 32 bytes (was 24)
 */

#include "pogls_wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

/* ══════════════════════════════════════════════════════════════════════
   CRC32 — IEEE 802.3 polynomial, table-driven
   ══════════════════════════════════════════════════════════════════════ */

static uint32_t crc32_table[256];
static int      crc32_table_ready = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t wal_crc32(uint32_t crc, const void *data, size_t len) {
    if (!crc32_table_ready) crc32_init_table();
    const uint8_t *p = data;
    crc ^= 0xFFFFFFFFu;
    while (len--) crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/*
 * Compute CRC32 for a record:
 *   covers header bytes [0 .. offsetof(crc32)-1] then payload
 * The crc32 field itself is NOT included in the hash (set to 0 before compute).
 */
static uint32_t record_crc(const WAL_RecordHeader *hdr,
                            const void *payload, uint32_t size) {
    /* Header bytes before crc32 field = 28 bytes (magic+pad+seq+data_offset+data_size) */
    uint32_t crc = wal_crc32(0, hdr, offsetof(WAL_RecordHeader, crc32));
    if (payload && size > 0)
        crc = wal_crc32(crc, payload, size);
    return crc;
}

/* ── I/O helpers ──────────────────────────────────────────────────────── */

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) { ssize_t w = write(fd,p,n); if(w<=0) return -1; p+=w; n-=(size_t)w; }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) { ssize_t r = read(fd,p,n); if(r==0) return 1; if(r<0) return -1; p+=r; n-=(size_t)r; }
    return 0;
}

/* ── SeqSet — compact bitset for committed seqs ───────────────────────── */

typedef struct { uint64_t cap; uint64_t *bits; } SeqSet;

static int seqset_mark(SeqSet *s, uint64_t seq) {
    uint64_t idx = seq / 64;
    if (idx >= s->cap) {
        uint64_t nc = idx + 64;
        uint64_t *nb = realloc(s->bits, nc * sizeof(uint64_t));
        if (!nb) return -1;
        memset(nb + s->cap, 0, (nc - s->cap) * sizeof(uint64_t));
        s->bits = nb; s->cap = nc;
    }
    s->bits[idx] |= (1ULL << (seq % 64));
    return 0;
}

static int seqset_test(const SeqSet *s, uint64_t seq) {
    uint64_t idx = seq / 64;
    return (idx < s->cap) ? (int)((s->bits[idx] >> (seq % 64)) & 1) : 0;
}

static void seqset_free(SeqSet *s) { free(s->bits); s->bits=NULL; s->cap=0; }

/* ══════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════ */

/*
 * wal_open — scan WAL to find max seq, set next_seq = max+1.
 * Safe for both fresh (empty) fd and existing WAL.
 */
int wal_open(WAL_Context *ctx, int wal_fd, int data_fd) {
    if (!ctx || wal_fd < 0 || data_fd < 0) return -1;
    ctx->wal_fd  = wal_fd;
    ctx->data_fd = data_fd;
    ctx->next_seq = 1;   /* default: fresh WAL */

    /* Scan to restore next_seq from existing records */
    if (lseek(wal_fd, 0, SEEK_SET) < 0) return -1;

    uint64_t max_seq = 0;
    WAL_RecordHeader hdr;
    uint8_t  skip_buf[WAL_MAX_PAYLOAD];

    for (;;) {
        int r = read_all(wal_fd, &hdr, sizeof(hdr));
        if (r == 1) break;   /* EOF */
        if (r <  0) break;   /* I/O error */
        if (hdr.magic != WAL_MAGIC) break;

        /* Read payload to verify CRC — same boundary rule as replay */
        uint32_t psz = 0;
        if (hdr.type == WAL_TYPE_INTENT) {
            if (hdr.data_size > WAL_MAX_PAYLOAD) break;
            psz = hdr.data_size;
            if (psz > 0 && read_all(wal_fd, skip_buf, psz) != 0) break;
        }

        /* CRC verify — stop at first mismatch (torn tail), matching replay */
        uint32_t stored = hdr.crc32;
        hdr.crc32 = 0;
        uint32_t computed = record_crc(&hdr, (psz > 0) ? skip_buf : NULL, psz);
        hdr.crc32 = stored;
        if (computed != stored) break;

        if (hdr.seq > max_seq) max_seq = hdr.seq;
    }

    /* next_seq boundary now identical to replay boundary */
    ctx->next_seq = max_seq + 1;
    return 0;
}

int64_t wal_write_intent(WAL_Context *ctx,
                          uint64_t data_offset,
                          const void *data, uint32_t size) {
    if (!ctx || !data || size == 0 || size > WAL_MAX_PAYLOAD) return -1;

    WAL_RecordHeader hdr = {
        .magic       = WAL_MAGIC,
        .type        = WAL_TYPE_INTENT,
        ._pad        = {0,0,0},
        .seq         = ctx->next_seq,
        .data_offset = data_offset,
        .data_size   = size,
        .crc32       = 0,   /* compute below */
    };
    hdr.crc32 = record_crc(&hdr, data, size);

    if (write_all(ctx->wal_fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (write_all(ctx->wal_fd, data, size)         != 0) return -1;
    /* fdatasync: ensure INTENT is durable before caller writes COMMIT */
    if (fdatasync(ctx->wal_fd) != 0) return -1;

    return (int64_t)(ctx->next_seq++);
}

int wal_write_commit(WAL_Context *ctx, uint64_t seq) {
    if (!ctx || seq == 0) return -1;
    WAL_RecordHeader hdr = {
        .magic = WAL_MAGIC, .type = WAL_TYPE_COMMIT,
        ._pad  = {0,0,0},  .seq  = seq,
        .data_offset = 0,  .data_size = 0, .crc32 = 0,
    };
    hdr.crc32 = record_crc(&hdr, NULL, 0);
    if (write_all(ctx->wal_fd, &hdr, sizeof(hdr)) != 0) return -1;
    /* fdatasync: COMMIT must be durable — this is the durability guarantee */
    return fdatasync(ctx->wal_fd);
}

int wal_write_abort(WAL_Context *ctx, uint64_t seq) {
    if (!ctx || seq == 0) return -1;
    WAL_RecordHeader hdr = {
        .magic = WAL_MAGIC, .type = WAL_TYPE_ABORT,
        ._pad  = {0,0,0},  .seq  = seq,
        .data_offset = 0,  .data_size = 0, .crc32 = 0,
    };
    hdr.crc32 = record_crc(&hdr, NULL, 0);
    return write_all(ctx->wal_fd, &hdr, sizeof(hdr));
}

/*
 * Two-pass replay with CRC32 verification.
 *
 * Pass 1: scan all records, verify CRC, collect committed seqs.
 *         First CRC mismatch → stop (treat as truncated WAL tail).
 * Pass 2: apply INTENT payloads for committed seqs only.
 */
int wal_replay(WAL_Context *ctx) {
    if (!ctx) return -1;
    if (lseek(ctx->wal_fd, 0, SEEK_SET) < 0) return -1;

    uint8_t *buf = malloc(WAL_MAX_PAYLOAD);
    if (!buf) return -1;

    SeqSet committed = {0, NULL};
    int replayed = 0, rc = 0;
    WAL_RecordHeader hdr;

    /* ── Pass 1: verify CRC, collect commits ──────────────────────────── */
    for (;;) {
        int r = read_all(ctx->wal_fd, &hdr, sizeof(hdr));
        if (r == 1) break;
        if (r <  0) { rc = -1; goto done; }

        if (hdr.magic != WAL_MAGIC) {
            fprintf(stderr, "wal_replay pass1: bad magic seq=%llu\n",
                    (unsigned long long)hdr.seq);
            rc = -1; goto done;
        }

        /* Read payload (need it for CRC) */
        uint32_t psz = 0;
        if (hdr.type == WAL_TYPE_INTENT) {
            if (hdr.data_size > WAL_MAX_PAYLOAD) { rc = -1; goto done; }
            psz = hdr.data_size;
            if (psz > 0 && read_all(ctx->wal_fd, buf, psz) != 0) { rc = -1; goto done; }
        }

        /* Verify CRC — stop at first mismatch (torn write at tail) */
        uint32_t stored = hdr.crc32;
        hdr.crc32 = 0;
        uint32_t computed = record_crc(&hdr, (psz > 0) ? buf : NULL, psz);
        hdr.crc32 = stored;

        if (computed != stored) {
            fprintf(stderr, "wal_replay pass1: CRC mismatch seq=%llu (treating as WAL tail)\n",
                    (unsigned long long)hdr.seq);
            break;   /* stop — don't treat as hard error, truncated tail is expected */
        }

        if (hdr.type == WAL_TYPE_COMMIT) {
            if (seqset_mark(&committed, hdr.seq) != 0) { rc = -1; goto done; }
        }
    }

    /* ── Pass 2: replay committed INTENTs ────────────────────────────── */
    if (lseek(ctx->wal_fd, 0, SEEK_SET) < 0) { rc = -1; goto done; }

    for (;;) {
        int r = read_all(ctx->wal_fd, &hdr, sizeof(hdr));
        if (r == 1) break;
        if (r <  0) { rc = -1; goto done; }
        if (hdr.magic != WAL_MAGIC) break;  /* same boundary as pass 1 */

        uint32_t psz = 0;
        if (hdr.type == WAL_TYPE_INTENT) {
            if (hdr.data_size > WAL_MAX_PAYLOAD) { rc = -1; goto done; }
            psz = hdr.data_size;
            if (psz > 0 && read_all(ctx->wal_fd, buf, psz) != 0) { rc = -1; goto done; }

            /* Verify again in pass 2 — buf may differ from pass 1 if FS is misbehaving */
            uint32_t stored = hdr.crc32;
            hdr.crc32 = 0;
            uint32_t chk = record_crc(&hdr, (psz > 0) ? buf : NULL, psz);
            hdr.crc32 = stored;
            if (chk != stored) break;   /* same: treat as tail */

            if (seqset_test(&committed, hdr.seq)) {
                if (lseek(ctx->data_fd, (off_t)hdr.data_offset, SEEK_SET) < 0)
                    { rc = -1; goto done; }
                if (write_all(ctx->data_fd, buf, psz) != 0)
                    { rc = -1; goto done; }
                replayed++;
            }
        }
    }

done:
    free(buf);
    seqset_free(&committed);
    return (rc == 0) ? replayed : rc;
}

int wal_truncate(WAL_Context *ctx) {
    if (!ctx) return -1;
    if (ftruncate(ctx->wal_fd, 0) != 0) return -1;
    if (lseek(ctx->wal_fd, 0, SEEK_SET) < 0) return -1;
    ctx->next_seq = 1;
    return 0;
}
