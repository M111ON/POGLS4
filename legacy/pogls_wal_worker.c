/*
 * pogls_wal_worker.c — Killable WAL write worker for Layer 3 Kill Test
 *
 * Usage:
 *   pogls_wal_worker <vault_path> <wal_path> <n_records> [seed]
 *
 * Behaviour:
 *   Opens vault + WAL files, writes n_records using WAL protocol,
 *   then exits 0. Kill with SIGKILL at any time — WAL ensures recovery.
 *
 *   Each record = 64B deterministic payload derived from (seed, seq).
 *   On replay: same (seed, seq) → same payload → deterministic verify.
 *
 * Exit codes:
 *   0  = completed all n_records
 *   1  = fatal error
 *
 * Stdout (one line per 1000 records, flushed):
 *   PROGRESS <seq> <n_records>
 *
 * Used by pogls_kill_test.py which:
 *   1. Starts this process
 *   2. Kills it at random point
 *   3. Runs pogls_wal_verify to check consistency
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

/* ── Deterministic payload generator ─────────────────────────────────── */
#define RECORD_PAYLOAD_SIZE 64

static void make_payload(uint8_t out[RECORD_PAYLOAD_SIZE],
                         uint64_t seed, uint64_t seq) {
    /* Simple mixing: each 8B word = LCG(seed ^ seq ^ i) */
    uint64_t state = seed ^ (seq * 6364136223846793005ULL) ^ 0xDEADBEEFCAFEULL;
    for (int i = 0; i < 8; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        memcpy(out + i * 8, &state, 8);
    }
}

/* ── cross-platform open helpers ──────────────────────────────────────── */
static int open_or_create(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    return fd;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <vault_path> <wal_path> <n_records> [seed]\n",
                argv[0]);
        return 1;
    }

    const char *vault_path = argv[1];
    const char *wal_path   = argv[2];
    uint64_t    n_records  = (uint64_t)strtoull(argv[3], NULL, 10);
    uint64_t    seed       = (argc >= 5) ? strtoull(argv[4], NULL, 16) : 0xFEEDC0DEULL;

    if (n_records == 0 || n_records > 10000000ULL) {
        fprintf(stderr, "n_records must be 1..10000000\n");
        return 1;
    }

    /* ── Open files ──────────────────────────────────────────────────── */
    int vault_fd = open_or_create(vault_path);
    if (vault_fd < 0) {
        fprintf(stderr, "open vault %s: %s\n", vault_path, strerror(errno));
        return 1;
    }

    int wal_fd = open_or_create(wal_path);
    if (wal_fd < 0) {
        fprintf(stderr, "open wal %s: %s\n", wal_path, strerror(errno));
        close(vault_fd);
        return 1;
    }

    /* ── Init WAL context (restore next_seq from existing WAL) ───────── */
    WAL_Context ctx;
    if (wal_open(&ctx, wal_fd, vault_fd) != 0) {
        fprintf(stderr, "wal_open failed\n");
        close(vault_fd); close(wal_fd);
        return 1;
    }

    fprintf(stderr, "worker: vault=%s wal=%s n=%llu seed=0x%llx next_seq=%llu\n",
            vault_path, wal_path,
            (unsigned long long)n_records,
            (unsigned long long)seed,
            (unsigned long long)ctx.next_seq);
    fflush(stderr);

    /* ── Write loop ───────────────────────────────────────────────────── */
    uint8_t payload[RECORD_PAYLOAD_SIZE];

    for (uint64_t i = 0; i < n_records; i++) {
        uint64_t seq_here = ctx.next_seq;  /* capture before intent increments */
        uint64_t data_offset = seq_here * RECORD_PAYLOAD_SIZE;

        make_payload(payload, seed, seq_here);

        /* WAL protocol: INTENT → data write → COMMIT */
        int64_t seq = wal_write_intent(&ctx, data_offset, payload, RECORD_PAYLOAD_SIZE);
        if (seq < 0) {
            fprintf(stderr, "wal_write_intent failed at i=%llu\n",
                    (unsigned long long)i);
            close(vault_fd); close(wal_fd);
            return 1;
        }

        /* Actual data write to vault at committed offset */
        if (lseek(vault_fd, (off_t)data_offset, SEEK_SET) < 0 ||
            write(vault_fd, payload, RECORD_PAYLOAD_SIZE) != RECORD_PAYLOAD_SIZE) {
            /* data write failed — abort WAL record */
            wal_write_abort(&ctx, (uint64_t)seq);
            fprintf(stderr, "vault write failed at offset %llu\n",
                    (unsigned long long)data_offset);
            close(vault_fd); close(wal_fd);
            return 1;
        }

        /* COMMIT — only after data is on disk */
        if (wal_write_commit(&ctx, (uint64_t)seq) != 0) {
            fprintf(stderr, "wal_write_commit failed at seq=%lld\n",
                    (long long)seq);
            close(vault_fd); close(wal_fd);
            return 1;
        }

        /* Progress report every 1000 records */
        if ((i + 1) % 1000 == 0) {
            printf("PROGRESS %llu %llu\n",
                   (unsigned long long)(i + 1),
                   (unsigned long long)n_records);
            fflush(stdout);
        }
    }

    printf("DONE %llu\n", (unsigned long long)n_records);
    fflush(stdout);

    close(vault_fd);
    close(wal_fd);
    return 0;
}
