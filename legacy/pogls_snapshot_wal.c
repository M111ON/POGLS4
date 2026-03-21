/*
 * pogls_snapshot_wal.c — Snapshot Integrity Layer
 *
 * SHA-256: FIPS 180-4 compliant, self-contained (no OpenSSL dep).
 * Snapshot format:
 *   [SNAP_Header:56B]
 *   [N × record: (seq:8 + data_offset:8 + size:4 + data:size)]
 *
 * Hash covers: entire record stream (everything after the 56B header).
 * Incremental: SHA256 updated per snap_append → no double-pass needed.
 */

#include "pogls_snapshot_wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* ══════════════════════════════════════════════════════════════════════
   SHA-256 — FIPS 180-4
   ══════════════════════════════════════════════════════════════════════ */

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a)     (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define EP1(e)     (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define SIG0(x)    (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x)    (ROR32(x,17)^ROR32(x,19)^((x)>>10))

static void sha256_transform(SHA256_Ctx *c, const uint8_t block[64]) {
    uint32_t w[64], a,b,d,e,f,g,h,t1,t2;
    uint32_t *s = c->state;

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)
              |((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    a=s[0];b=s[1];uint32_t c2=s[2];d=s[3];e=s[4];f=s[5];g=s[6];h=s[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K[i] + w[i];
        t2 = EP0(a) + MAJ(a,b,c2);
        h=g; g=f; f=e; e=d+t1; d=c2; c2=b; b=a; a=t1+t2;
    }
    s[0]+=a;s[1]+=b;s[2]+=c2;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}

void sha256_init(SHA256_Ctx *c) {
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85;
    c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c;
    c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
    c->count = 0; c->buf_len = 0;
}

void sha256_update(SHA256_Ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->count += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = 64 - c->buf_len;
        uint32_t take  = (len < space) ? (uint32_t)len : space;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take; p += take; len -= take;
        if (c->buf_len == 64) {
            sha256_transform(c, c->buf);
            c->buf_len = 0;
        }
    }
}

void sha256_final(SHA256_Ctx *c, uint8_t out[32]) {
    /* Save original bit-count BEFORE padding (padding must not affect length field) */
    uint64_t orig_count = c->count;

    /* Append 0x80 */
    uint8_t one = 0x80;
    sha256_update(c, &one, 1);

    /* Pad with zeros until buf_len == 56 (leaving 8 bytes for length) */
    uint8_t zero = 0x00;
    while (c->buf_len != 56)
        sha256_update(c, &zero, 1);

    /* Append original length as big-endian 64-bit (restoring correct count) */
    uint8_t bits[8];
    for (uint32_t i = 0; i < 8; i++) bits[i] = (uint8_t)(orig_count >> (56 - i*8));
    sha256_update(c, bits, 8);

    for (uint32_t i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(c->state[i]>>24);
        out[i*4+1] = (uint8_t)(c->state[i]>>16);
        out[i*4+2] = (uint8_t)(c->state[i]>>8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

void sha256_digest(const void *data, size_t len, uint8_t out[32]) {
    SHA256_Ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ══════════════════════════════════════════════════════════════════════
   I/O helpers
   ══════════════════════════════════════════════════════════════════════ */

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) { ssize_t w = write(fd,p,n); if(w<=0) return -1; p+=w; n-=w; }
    return 0;
}
static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) { ssize_t r = read(fd,p,n); if(r==0) return 1; if(r<0) return -1; p+=r; n-=r; }
    return 0;
}

/* Per-record wire header (20 bytes, preceding data) */
typedef struct __attribute__((packed)) {
    uint64_t seq;
    uint64_t data_offset;
    uint32_t size;
} SNAP_RecHdr;

/* ══════════════════════════════════════════════════════════════════════
   Snapshot Writer
   ══════════════════════════════════════════════════════════════════════ */

int snap_begin(SNAP_Writer *w, int snap_fd) {
    if (!w || snap_fd < 0) return -1;
    w->snap_fd      = snap_fd;
    w->last_seq     = 0;
    w->record_count = 0;
    w->finalized    = 0;
    sha256_init(&w->hash_ctx);

    /* Reserve space for header — will be filled by snap_finalize */
    if (lseek(snap_fd, 0, SEEK_SET) < 0) return -1;
    if (ftruncate(snap_fd, 0) < 0) return -1;
    uint8_t zero[sizeof(SNAP_Header)] = {0};
    return write_all(snap_fd, zero, sizeof(SNAP_Header));
}

int snap_append(SNAP_Writer *w, uint64_t seq,
                uint64_t data_offset, const void *data, uint32_t size) {
    if (!w || w->finalized || !data || size == 0) return -1;

    SNAP_RecHdr rh = { .seq = seq, .data_offset = data_offset, .size = size };

    /* Incremental hash: cover record header + payload */
    sha256_update(&w->hash_ctx, &rh,  sizeof(rh));
    sha256_update(&w->hash_ctx, data, size);

    /* Write to file */
    if (write_all(w->snap_fd, &rh,  sizeof(rh)) != 0) return -1;
    if (write_all(w->snap_fd, data, size)        != 0) return -1;

    w->last_seq     = seq;
    w->record_count++;
    return 0;
}

int snap_finalize(SNAP_Writer *w) {
    if (!w || w->finalized) return -1;

    SNAP_Header hdr = {
        .magic        = SNAP_MAGIC,
        .version      = SNAP_VERSION,
        ._pad         = {0,0,0},
        .last_seq     = w->last_seq,
        .record_count = w->record_count,
    };
    sha256_final(&w->hash_ctx, hdr.payload_hash);

    /* Seek back and write header */
    if (lseek(w->snap_fd, 0, SEEK_SET) < 0) return -1;
    if (write_all(w->snap_fd, &hdr, sizeof(hdr)) != 0) return -1;

    w->finalized = 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   Snapshot Loader — verify then deliver
   ══════════════════════════════════════════════════════════════════════ */

/*
 * Internal: stream through payload, compute hash, optionally call cb.
 * Returns 0 = OK, -1 = I/O error.
 */
static int _stream_payload(int snap_fd, uint64_t record_count,
                           SHA256_Ctx *hctx,
                           snap_record_cb cb, void *ud) {
    /* Reuse a single heap buffer; max sensible record size = WAL_MAX_PAYLOAD */
    const uint32_t MAX_REC = 65536u;
    uint8_t *buf = malloc(MAX_REC);
    if (!buf) return -1;

    int rc = 0;
    for (uint64_t i = 0; i < record_count; i++) {
        SNAP_RecHdr rh;
        int r = read_all(snap_fd, &rh, sizeof(rh));
        if (r != 0) { rc = -1; break; }
        if (rh.size == 0 || rh.size > MAX_REC) { rc = -1; break; }

        r = read_all(snap_fd, buf, rh.size);
        if (r != 0) { rc = -1; break; }

        if (hctx) {
            sha256_update(hctx, &rh,  sizeof(rh));
            sha256_update(hctx, buf, rh.size);
        }
        if (cb) cb(rh.seq, rh.data_offset, buf, rh.size, ud);
    }

    free(buf);
    return rc;
}

int snap_load(int snap_fd, SNAP_Header *out_hdr,
              snap_record_cb cb, void *userdata) {
    if (snap_fd < 0) return -1;
    if (lseek(snap_fd, 0, SEEK_SET) < 0) return -1;

    SNAP_Header hdr;
    int r = read_all(snap_fd, &hdr, sizeof(hdr));
    if (r == 1) return -2;   /* empty file */
    if (r <  0) return -1;

    if (hdr.magic   != SNAP_MAGIC)   return -2;
    if (hdr.version != SNAP_VERSION) return -2;

    /* ── Verification pass: re-stream payload, compute hash ─────────── */
    SHA256_Ctx hctx;
    sha256_init(&hctx);

    off_t payload_start = (off_t)sizeof(SNAP_Header);
    if (lseek(snap_fd, payload_start, SEEK_SET) < 0) return -1;

    if (_stream_payload(snap_fd, hdr.record_count, &hctx, NULL, NULL) != 0)
        return -1;

    uint8_t computed[32];
    sha256_final(&hctx, computed);

    /* Fail-fast on mismatch — do NOT deliver data to caller */
    if (memcmp(computed, hdr.payload_hash, 32) != 0) return -3;

    /* ── Deliver pass: re-stream, call callback ──────────────────────── */
    if (cb) {
        if (lseek(snap_fd, payload_start, SEEK_SET) < 0) return -1;
        if (_stream_payload(snap_fd, hdr.record_count, NULL, cb, userdata) != 0)
            return -1;
    }

    if (out_hdr) *out_hdr = hdr;
    return 0;
}

int snap_verify(int snap_fd) {
    /* load with no callback — just hash check */
    return snap_load(snap_fd, NULL, NULL, NULL);
}
