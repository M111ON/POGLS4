/*
 * pogls_wal_test.c — Production WAL test suite
 *
 * Tests:
 *   T01  Basic intent+commit+replay
 *   T02  Abort → NOT replayed
 *   T03  Multiple records, partial commit
 *   T04  Replay idempotency (replay twice → same result)
 *   T05  Truncate clears WAL
 *   T06  Corruption detection (bad magic)
 *   T07  Oversized payload rejected
 *   T08  Zero-size payload rejected
 *   T09  Replay with no records (empty WAL)
 */

#include "pogls_wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

/* ── Test harness ─────────────────────────────────────────────────────── */
static int pass_count = 0, fail_count = 0;

#define CHECK(cond, label) do { \
    if (cond) { printf("  PASS  %s\n", label); pass_count++; } \
    else       { printf("  FAIL  %s  (line %d)\n", label, __LINE__); fail_count++; } \
} while(0)

#define SECTION(name) printf("\n── %s ──\n", name)

/* Create anonymous temp fds */
static int tmpfd(void) {
    char path[] = "/tmp/pogls_wal_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    unlink(path);   /* delete name, keep fd open */
    return fd;
}

/* Read data_fd at offset into buf[size] */
static int readat(int fd, off_t off, void *buf, size_t size) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    ssize_t r = read(fd, buf, size);
    return (r == (ssize_t)size) ? 0 : -1;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void t01_basic_commit(void) {
    SECTION("T01  basic intent+commit+replay");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    const char *msg = "HELLO_POGLS";
    int64_t seq = wal_write_intent(&ctx, 0, msg, (uint32_t)strlen(msg));
    CHECK(seq > 0, "write_intent returns valid seq");

    int rc = wal_write_commit(&ctx, (uint64_t)seq);
    CHECK(rc == 0, "write_commit ok");

    int replayed = wal_replay(&ctx);
    CHECK(replayed == 1, "replay count == 1");

    char buf[32] = {0};
    int rr = readat(dfd, 0, buf, strlen(msg));
    CHECK(rr == 0 && memcmp(buf, msg, strlen(msg)) == 0, "data written to data_fd");

    close(wfd); close(dfd);
}

static void t02_abort_not_replayed(void) {
    SECTION("T02  abort → NOT replayed");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    const char *msg = "SHOULD_NOT_APPEAR";
    int64_t seq = wal_write_intent(&ctx, 0, msg, (uint32_t)strlen(msg));
    wal_write_abort(&ctx, (uint64_t)seq);

    int replayed = wal_replay(&ctx);
    CHECK(replayed == 0, "abort: replay count == 0");

    /* data_fd should still be empty */
    off_t sz = lseek(dfd, 0, SEEK_END);
    CHECK(sz == 0, "abort: data_fd untouched");

    close(wfd); close(dfd);
}

static void t03_partial_commit(void) {
    SECTION("T03  multiple records, partial commit");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    /* Write 3 intents — commit only #1 and #3 */
    const char *d1 = "AAAAAA";
    const char *d2 = "BBBBBB";
    const char *d3 = "CCCCCC";
    uint32_t sz = 6;

    int64_t s1 = wal_write_intent(&ctx, 0,  d1, sz);
    int64_t s2 = wal_write_intent(&ctx, 6,  d2, sz);
    int64_t s3 = wal_write_intent(&ctx, 12, d3, sz);

    wal_write_commit(&ctx, (uint64_t)s1);
    wal_write_abort (&ctx, (uint64_t)s2);
    wal_write_commit(&ctx, (uint64_t)s3);

    int replayed = wal_replay(&ctx);
    CHECK(replayed == 2, "partial: 2 records replayed");

    char buf[7] = {0};
    readat(dfd, 0,  buf, 6); CHECK(memcmp(buf, d1, 6) == 0, "offset 0 = AAAAAA");
    readat(dfd, 12, buf, 6); CHECK(memcmp(buf, d3, 6) == 0, "offset 12 = CCCCCC");

    /* offset 6 should NOT have d2 — should be zeros (dfd was fresh) */
    readat(dfd, 6, buf, 6);
    CHECK(buf[0] == 0, "offset 6 untouched (aborted)");

    close(wfd); close(dfd);
}

static void t04_replay_idempotent(void) {
    SECTION("T04  replay idempotency");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    const char *msg = "IDEM";
    int64_t seq = wal_write_intent(&ctx, 0, msg, 4);
    wal_write_commit(&ctx, (uint64_t)seq);

    int r1 = wal_replay(&ctx);
    int r2 = wal_replay(&ctx);   /* replay again — same WAL */
    CHECK(r1 == 1 && r2 == 1, "both replays return 1");

    char buf[5] = {0};
    readat(dfd, 0, buf, 4);
    CHECK(memcmp(buf, msg, 4) == 0, "data correct after double replay");

    close(wfd); close(dfd);
}

static void t05_truncate(void) {
    SECTION("T05  truncate clears WAL");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    const char *msg = "BEFORE";
    int64_t seq = wal_write_intent(&ctx, 0, msg, 6);
    wal_write_commit(&ctx, (uint64_t)seq);

    wal_truncate(&ctx);

    off_t wsz = lseek(wfd, 0, SEEK_END);
    CHECK(wsz == 0, "WAL fd is empty after truncate");

    /* seq counter reset */
    int64_t seq2 = wal_write_intent(&ctx, 0, "AFTER", 5);
    CHECK(seq2 == 1, "seq reset to 1 after truncate");

    close(wfd); close(dfd);
}

static void t06_corruption_detected(void) {
    SECTION("T06  corruption detection (bad magic)");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    const char *msg = "CORRUPT";
    int64_t seq = wal_write_intent(&ctx, 0, msg, 7);
    wal_write_commit(&ctx, (uint64_t)seq);

    /* Overwrite first 4 bytes (magic) with garbage */
    lseek(wfd, 0, SEEK_SET);
    uint32_t bad = 0xDEADBEEF;
    if (write(wfd, &bad, 4) < 0) { perror("write"); }

    int replayed = wal_replay(&ctx);
    CHECK(replayed < 0, "corrupt magic → replay returns error");

    close(wfd); close(dfd);
}

static void t07_oversized_payload_rejected(void) {
    SECTION("T07  oversized payload rejected");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    uint8_t *big = calloc(WAL_MAX_PAYLOAD + 1, 1);
    int64_t seq = wal_write_intent(&ctx, 0, big, WAL_MAX_PAYLOAD + 1);
    CHECK(seq < 0, "oversized payload → write_intent returns error");
    free(big);

    close(wfd); close(dfd);
}

static void t08_zero_size_rejected(void) {
    SECTION("T08  zero-size payload rejected");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    int64_t seq = wal_write_intent(&ctx, 0, "x", 0);
    CHECK(seq < 0, "zero-size → write_intent returns error");

    close(wfd); close(dfd);
}

static void t09_empty_wal_replay(void) {
    SECTION("T09  replay on empty WAL");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    int replayed = wal_replay(&ctx);
    CHECK(replayed == 0, "empty WAL → replay returns 0");

    close(wfd); close(dfd);
}

static void t10_crc32_known_vector(void) {
    SECTION("T10  CRC32 known vector");
    /* CRC32("123456789") = 0xCBF43926 — standard check value */
    uint32_t crc = wal_crc32(0, "123456789", 9);
    CHECK(crc == 0xCBF43926u, "CRC32('123456789') == 0xCBF43926");
    /* incremental == one-shot */
    uint32_t c1 = wal_crc32(0,  "1234", 4);
    uint32_t c2 = wal_crc32(c1, "56789", 5);
    CHECK(c2 == 0xCBF43926u, "incremental CRC32 == one-shot");
}

static void t11_crc_mismatch_stops_replay(void) {
    SECTION("T11  CRC mismatch in payload → replay stops at tail");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    int64_t seq = wal_write_intent(&ctx, 0, "GOOD", 4);
    wal_write_commit(&ctx, (uint64_t)seq);

    /* Flip a byte inside the payload (after 32B header) */
    lseek(wfd, 32 + 2, SEEK_SET);
    uint8_t bad = 0xFF;
    if (write(wfd, &bad, 1) < 0) perror("write");

    /* Replay should stop at the corrupted record — 0 committed replays */
    int replayed = wal_replay(&ctx);
    CHECK(replayed == 0, "CRC mismatch → corrupt record not replayed");

    close(wfd); close(dfd);
}

static void t14_reopen_crc_boundary(void) {
    SECTION("T14  reopen: CRC mismatch stops seq scan at torn tail");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    /* Write seq 1 (good), seq 2 (will be corrupted) */
    wal_write_intent(&ctx, 0,  "FIRST", 5);   /* seq 1 */
    wal_write_intent(&ctx, 5,  "SECND", 5);   /* seq 2 */

    /* Corrupt seq 2's payload — CRC will fail */
    lseek(wfd, 0, SEEK_END);
    off_t end = lseek(wfd, 0, SEEK_CUR);
    /* seq 2 header starts at 32 + (32+5) = 69 bytes */
    lseek(wfd, 69, SEEK_SET);
    uint8_t bad = 0xFF;
    if (write(wfd, &bad, 1) < 0) perror("write");

    /* Reopen — should stop at seq 1 (seq 2 CRC fails) */
    WAL_Context ctx2;
    wal_open(&ctx2, wfd, dfd);
    int64_t next = wal_write_intent(&ctx2, 10, "NEXT", 4);
    CHECK(next == 2, "reopen stops at CRC mismatch: next_seq = 1+1 = 2");
    (void)end;

    close(wfd); close(dfd);
}

static void t12_reopen_restores_seq(void) {
    SECTION("T12  wal_open restores next_seq on reopen");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);

    wal_write_intent(&ctx, 0, "FIRST", 5);   /* seq 1 */
    wal_write_intent(&ctx, 5, "SECND", 5);   /* seq 2 */
    /* Don't truncate — simulate process restart */

    WAL_Context ctx2;
    wal_open(&ctx2, wfd, dfd);               /* reopen same fd */
    int64_t seq3 = wal_write_intent(&ctx2, 10, "THIRD", 5);
    CHECK(seq3 == 3, "after reopen, next_seq continues from max+1");

    close(wfd); close(dfd);
}

static void t13_reopen_empty_seq_starts_at_1(void) {
    SECTION("T13  wal_open on empty WAL starts at seq 1");
    int wfd = tmpfd(), dfd = tmpfd();
    WAL_Context ctx;
    wal_open(&ctx, wfd, dfd);
    int64_t seq = wal_write_intent(&ctx, 0, "X", 1);
    CHECK(seq == 1, "fresh WAL: first seq == 1");
    close(wfd); close(dfd);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    printf("POGLS WAL Test Suite — Production\n");
    printf("===================================\n");

    t01_basic_commit();
    t02_abort_not_replayed();
    t03_partial_commit();
    t04_replay_idempotent();
    t05_truncate();
    t06_corruption_detected();
    t07_oversized_payload_rejected();
    t08_zero_size_rejected();
    t09_empty_wal_replay();
    t10_crc32_known_vector();
    t11_crc_mismatch_stops_replay();
    t12_reopen_restores_seq();
    t13_reopen_empty_seq_starts_at_1();
    t14_reopen_crc_boundary();

    printf("\n===================================\n");
    printf("Results: %d/%d PASS", pass_count, pass_count + fail_count);
    if (fail_count == 0) printf("  ✓ ALL PASS\n");
    else printf("  ✗ %d FAIL\n", fail_count);

    return fail_count > 0 ? 1 : 0;
}
