/* pogls_spectre.h - Giant Shadow Metadata Ledger */
#ifndef POGLS_SPECTRE_H
#define POGLS_SPECTRE_H

#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* Spectre constants */
#define SPECTRE_MAX (1 << 20)      /* 1M entries = 32 MB */
#define SPECTRE_BATCH_SIZE 256     /* blocks per batch */
#define SPECTRE_WORLD_PENDING (~0ULL)

/* Spectre entry - metadata only (32 bytes) */
typedef struct {
    uint8_t  lane;              /* 0-53 (Rubik lane) */
    uint8_t  world;             /* 0=A, 1=B */
    uint16_t reserved;          /* alignment */
    uint32_t start_index;       /* offset in lane buffer */
    uint32_t count;             /* batch size */
    uint64_t world_offset;      /* disk position (PENDING if not flushed) */
    uint64_t timestamp_ns;      /* creation time */
    uint64_t angular_addr;      /* A = floor(θ × 2²⁰) */
} __attribute__((packed)) SpectreEntry;

/* Spectre ring buffer */
typedef struct {
    SpectreEntry entries[SPECTRE_MAX];
    
    volatile uint32_t head;        /* producer index */
    volatile uint32_t tail;        /* consumer index */
    volatile uint32_t count;       /* current entries */
    volatile uint32_t confirmed;   /* flushed to disk */
    
    uint64_t total_pushed;
    uint64_t total_flushed;
    uint32_t overflow_wraps;
    
    pthread_mutex_t lock;
} SpectreRing;

/* Get current time in nanoseconds */
static inline uint64_t spectre_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Init spectre ring */
static inline void spectre_init(SpectreRing *sr) {
    memset(sr, 0, sizeof(*sr));
    pthread_mutex_init(&sr->lock, NULL);
}

/* Push new batch metadata */
static inline int spectre_push(SpectreRing *sr, SpectreEntry *entry) {
    pthread_mutex_lock(&sr->lock);
    
    uint32_t idx = sr->head % SPECTRE_MAX;
    sr->head++;
    
    /* overflow handling (ring buffer) */
    if (sr->count < SPECTRE_MAX) {
        sr->count++;
    } else {
        sr->tail = (sr->tail + 1) % SPECTRE_MAX;
        sr->overflow_wraps++;
    }
    
    /* copy entry */
    sr->entries[idx] = *entry;
    sr->total_pushed++;
    
    pthread_mutex_unlock(&sr->lock);
    return 0;
}

/* Pop batch metadata for flushing */
static inline int spectre_pop(SpectreRing *sr, SpectreEntry *out) {
    pthread_mutex_lock(&sr->lock);
    
    if (sr->tail == sr->head) {
        pthread_mutex_unlock(&sr->lock);
        return 0;  /* empty */
    }
    
    uint32_t idx = sr->tail % SPECTRE_MAX;
    *out = sr->entries[idx];
    
    sr->tail++;
    if (sr->count > 0) sr->count--;
    
    pthread_mutex_unlock(&sr->lock);
    return 1;  /* got entry */
}

/* Peek without removing */
static inline int spectre_peek(SpectreRing *sr, uint32_t offset, SpectreEntry *out) {
    pthread_mutex_lock(&sr->lock);
    
    uint32_t idx = (sr->tail + offset) % SPECTRE_MAX;
    if (offset >= sr->count) {
        pthread_mutex_unlock(&sr->lock);
        return 0;  /* out of range */
    }
    
    *out = sr->entries[idx];
    pthread_mutex_unlock(&sr->lock);
    return 1;
}

/* Confirm entry flushed to disk */
static inline void spectre_confirm(SpectreRing *sr, uint32_t offset, uint64_t disk_offset) {
    pthread_mutex_lock(&sr->lock);
    
    uint32_t idx = (sr->tail + offset) % SPECTRE_MAX;
    if (offset < sr->count) {
        sr->entries[idx].world_offset = disk_offset;
        sr->confirmed++;
        sr->total_flushed++;
    }
    
    pthread_mutex_unlock(&sr->lock);
}

/* Build batch entry from lane accumulation */
static inline SpectreEntry spectre_build_entry(
    uint8_t lane,
    uint8_t world,
    uint32_t start_idx,
    uint32_t count,
    uint64_t angular_addr)
{
    SpectreEntry e = {
        .lane = lane,
        .world = world,
        .reserved = 0,
        .start_index = start_idx,
        .count = count,
        .world_offset = SPECTRE_WORLD_PENDING,
        .timestamp_ns = spectre_now_ns(),
        .angular_addr = angular_addr
    };
    return e;
}

/* Find pending entries (for crash recovery) */
static inline uint32_t spectre_count_pending(SpectreRing *sr) {
    pthread_mutex_lock(&sr->lock);
    
    uint32_t pending = 0;
    for (uint32_t i = 0; i < sr->count; i++) {
        uint32_t idx = (sr->tail + i) % SPECTRE_MAX;
        if (sr->entries[idx].world_offset == SPECTRE_WORLD_PENDING) {
            pending++;
        }
    }
    
    pthread_mutex_unlock(&sr->lock);
    return pending;
}

/* Get oldest pending timestamp (staleness check) */
static inline uint64_t spectre_oldest_pending_ns(SpectreRing *sr) {
    pthread_mutex_lock(&sr->lock);
    
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < sr->count; i++) {
        uint32_t idx = (sr->tail + i) % SPECTRE_MAX;
        if (sr->entries[idx].world_offset == SPECTRE_WORLD_PENDING) {
            if (sr->entries[idx].timestamp_ns < oldest) {
                oldest = sr->entries[idx].timestamp_ns;
            }
        }
    }
    
    pthread_mutex_unlock(&sr->lock);
    return oldest == UINT64_MAX ? 0 : oldest;
}

/* Utilization stats */
static inline double spectre_utilization(SpectreRing *sr) {
    return 100.0 * sr->count / SPECTRE_MAX;
}

/* Memory size */
static inline size_t spectre_memory_kb(void) {
    return (SPECTRE_MAX * sizeof(SpectreEntry)) / 1024;
}

/* Print stats */
static inline void spectre_print_stats(SpectreRing *sr) {
    pthread_mutex_lock(&sr->lock);
    
    uint32_t pending = spectre_count_pending(sr);
    uint64_t oldest = spectre_oldest_pending_ns(sr);
    uint64_t now = spectre_now_ns();
    double age_sec = oldest > 0 ? (now - oldest) / 1e9 : 0.0;
    
    printf("[Spectre] entries %u/%u (%.1f%%)  pushed %llu  flushed %llu  "
           "pending %u (oldest %.1fs)  overflow %u\n",
           sr->count, SPECTRE_MAX, spectre_utilization(sr),
           (unsigned long long)sr->total_pushed,
           (unsigned long long)sr->total_flushed,
           pending, age_sec, sr->overflow_wraps);
    
    pthread_mutex_unlock(&sr->lock);
}

#endif /* POGLS_SPECTRE_H */
