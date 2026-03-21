/*
 * pogls_wal_verify.c — Post-kill vault consistency verifier for Layer 3
 *
 * Usage:
 *   pogls_wal_verify <vault_path> <wal_path> <seed>
 *
 * Steps:
 *   1. Replay WAL into vault (idempotent — safe to call multiple times)
 *   2. Read every committed record from vault
 *   3. Recompute expected payload from (seed, seq)
 *   4. Compare — any mismatch = CORRUPTION
 *
 * Exit codes:
 *   0  = PASS: vault consistent, no corruption
 *   1  = FAIL: corruption detected
 *   2  = ERROR: I/O or usage error
 *
 * Stdout (machine-parseable for Python runner):
 *   REPLAY_OK <n_replayed>
 *   VERIFIED <n_records>   <- number of records in vault after replay
 *   PASS                   <- only if exit 0
 *   FAIL seq=<N> ...       <- only if exit 1
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "pogls_wal.h"

#define RECORD_PAYLOAD_SIZE 64

static void make_payload(uint8_t out[RECORD_PAYLOAD_SIZE],
                         uint64_t seed, uint64_t seq) {
    uint64_t state = seed ^ (seq * 6364136223846793005ULL) ^ 0xDEADBEEFCAFEULL;
    for (int i = 0; i < 8; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        memcpy(out + i * 8, &state, 8);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <vault_path> <wal_path> <seed>\n", argv[0]);
        return 2;
    }

    const char *vault_path = argv[1];
    const char *wal_path   = argv[2];
    uint64_t    seed       = strtoull(argv[3], NULL, 16);

    int vault_fd = open(vault_path, O_RDWR | O_CREAT, 0644);
    if (vault_fd < 0) {
        fprintf(stderr, "open vault: %s\n", strerror(errno));
        return 2;
    }

    int wal_fd = open(wal_path, O_RDWR | O_CREAT, 0644);
    if (wal_fd < 0) {
        fprintf(stderr, "open wal: %s\n", strerror(errno));
        close(vault_fd);
        return 2;
    }

    /* ── Step 1: Replay WAL → vault ───────────────────────────────────── */
    WAL_Context ctx;
    if (wal_open(&ctx, wal_fd, vault_fd) != 0) {
        fprintf(stderr, "wal_open failed\n");
        close(vault_fd); close(wal_fd);
        return 2;
    }

    int replayed = wal_replay(&ctx);
    if (replayed < 0) {
        fprintf(stderr, "wal_replay failed\n");
        close(vault_fd); close(wal_fd);
        return 2;
    }
    printf("REPLAY_OK %d\n", replayed);
    fflush(stdout);

    /* ── Step 2: Determine how many records are in vault ─────────────── */
    off_t vault_size = lseek(vault_fd, 0, SEEK_END);
    if (vault_size < 0) {
        fprintf(stderr, "lseek vault: %s\n", strerror(errno));
        close(vault_fd); close(wal_fd);
        return 2;
    }

    /* Records start at offset RECORD_PAYLOAD_SIZE (seq=1 → offset=64)
       seq=0 is reserved (wal_open starts at seq=1) */
    uint64_t n_records = (uint64_t)vault_size / RECORD_PAYLOAD_SIZE;
    if (n_records == 0) {
        printf("VERIFIED 0\nPASS\n");
        close(vault_fd); close(wal_fd);
        return 0;
    }

    /* ── Step 3: Verify each record ───────────────────────────────────── */
    uint8_t actual[RECORD_PAYLOAD_SIZE];
    uint8_t expected[RECORD_PAYLOAD_SIZE];
    int corrupt = 0;

    /* seq starts at 1 (wal_open convention) */
    for (uint64_t seq = 1; seq < n_records; seq++) {
        uint64_t offset = seq * RECORD_PAYLOAD_SIZE;

        if (lseek(vault_fd, (off_t)offset, SEEK_SET) < 0) {
            fprintf(stderr, "lseek seq=%llu: %s\n",
                    (unsigned long long)seq, strerror(errno));
            close(vault_fd); close(wal_fd);
            return 2;
        }

        ssize_t r = read(vault_fd, actual, RECORD_PAYLOAD_SIZE);
        if (r != RECORD_PAYLOAD_SIZE) {
            /* Partial record at tail — not corruption, just incomplete write */
            /* This is expected after a kill — stop here */
            break;
        }

        make_payload(expected, seed, seq);

        if (memcmp(actual, expected, RECORD_PAYLOAD_SIZE) != 0) {
            fprintf(stderr, "FAIL seq=%llu offset=%llu payload mismatch\n",
                    (unsigned long long)seq, (unsigned long long)offset);
            printf("FAIL seq=%llu\n", (unsigned long long)seq);
            fflush(stdout);
            corrupt++;
            if (corrupt >= 5) break;  /* report first 5 corruptions */
        }
    }

    printf("VERIFIED %llu\n", (unsigned long long)(n_records - 1));
    fflush(stdout);

    close(vault_fd);
    close(wal_fd);

    if (corrupt == 0) {
        printf("PASS\n");
        return 0;
    }
    return 1;
}
