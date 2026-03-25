/*
 * pogls_shadow_delta_wired.h — Wire Giant Shadow → Delta
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Complete pipeline:
 *   Hydra producers
 *   → DoubleShadowRing (L1 + L2)
 *   → Giant Shadow ledger
 *   → Flush workers (this file)
 *   → Delta lanes (disk)
 *
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_SHADOW_DELTA_WIRED_H
#define POGLS_SHADOW_DELTA_WIRED_H

#include <pthread.h>
#include <unistd.h>

/* Forward declarations */
typedef struct GiantShadow GiantShadow;
typedef struct ShadowEntry ShadowEntry;
typedef struct DeltaWriter DeltaWriter;
typedef struct DiamondBlock DiamondBlock;

/* Flush worker context */
typedef struct {
    GiantShadow  *shadow;      /* shadow ledger */
    DeltaWriter  *delta;       /* delta writer */
    
    int           worker_id;
    pthread_t     thread;
    volatile int  stop;
    
    /* stats */
    uint64_t      batches_processed;
    uint64_t      blocks_written;
    uint64_t      sync_calls;
} FlushWorker;

/* Flush worker thread (pulls from Giant Shadow → writes to Delta) */
static void *flush_worker_thread(void *arg)
{
    FlushWorker *fw = (FlushWorker *)arg;
    if (!fw) return NULL;
    
    printf("[FlushWorker %d] Started\n", fw->worker_id);
    
    while (!fw->stop) {
        /* Try to pop entry from shadow */
        ShadowEntry entry;
        int ret = 0; /* gs_pop(fw->shadow, &entry); */
        
        if (ret != 0) {
            /* No work — sleep briefly */
            usleep(100);  /* 100µs */
            continue;
        }
        
        /* Get blocks from lane buffer (mock for now) */
        DiamondBlock blocks[256];
        memset(blocks, 0, sizeof(blocks));
        
        /* TODO: Get real blocks from DoubleShadowRing L2
         * dsr_get_batch(&lane_buffers[entry.lane], blocks, entry.count);
         */
        
        /* Write to Delta */
        /* ret = delta_append_batch(fw->delta, 
                                    entry.lane,
                                    blocks,
                                    entry.count,
                                    0);  // no sync per batch */
        
        if (ret == 0) {
            fw->batches_processed++;
            fw->blocks_written += entry.count;
        }
        
        /* Sync every 18 batches (gate rhythm) */
        if (fw->batches_processed % 18 == 0) {
            /* delta_sync(fw->delta, entry.lane); */
            fw->sync_calls++;
        }
    }
    
    printf("[FlushWorker %d] Stopped (batches=%llu, blocks=%llu, syncs=%llu)\n",
           fw->worker_id,
           (unsigned long long)fw->batches_processed,
           (unsigned long long)fw->blocks_written,
           (unsigned long long)fw->sync_calls);
    
    return NULL;
}

/* Start flush worker */
static inline int flush_worker_start(FlushWorker *fw,
                                      GiantShadow *shadow,
                                      DeltaWriter *delta,
                                      int worker_id)
{
    if (!fw || !shadow || !delta) return -1;
    
    memset(fw, 0, sizeof(*fw));
    fw->shadow = shadow;
    fw->delta = delta;
    fw->worker_id = worker_id;
    fw->stop = 0;
    
    if (pthread_create(&fw->thread, NULL, flush_worker_thread, fw) != 0) {
        fprintf(stderr, "[FlushWorker] Failed to create thread %d\n", worker_id);
        return -1;
    }
    
    return 0;
}

/* Stop flush worker */
static inline void flush_worker_stop(FlushWorker *fw)
{
    if (!fw) return;
    
    fw->stop = 1;
    pthread_join(fw->thread, NULL);
}

/* ══════════════════════════════════════════════════════════════════
 * Complete Pipeline Manager
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    GiantShadow   shadow;
    DeltaWriter   delta;
    FlushWorker   workers[16];  /* max workers */
    int           num_workers;
} ShadowDeltaPipeline;

static inline int sdp_init(ShadowDeltaPipeline *sdp,
                            const char *delta_base_dir,
                            int num_workers)
{
    if (!sdp || num_workers < 1 || num_workers > 16)
        return -1;
    
    memset(sdp, 0, sizeof(*sdp));
    sdp->num_workers = num_workers;
    
    /* Init Giant Shadow */
    /* gs_init(&sdp->shadow); */
    
    /* Init Delta Writer */
    /* delta_writer_init(&sdp->delta, delta_base_dir); */
    
    /* Start flush workers */
    for (int i = 0; i < num_workers; i++) {
        flush_worker_start(&sdp->workers[i],
                          &sdp->shadow,
                          &sdp->delta,
                          i);
    }
    
    printf("[Pipeline] Initialized: %d flush workers\n", num_workers);
    return 0;
}

static inline void sdp_close(ShadowDeltaPipeline *sdp)
{
    if (!sdp) return;
    
    /* Stop workers */
    for (int i = 0; i < sdp->num_workers; i++) {
        flush_worker_stop(&sdp->workers[i]);
    }
    
    /* Close Delta */
    /* delta_writer_close(&sdp->delta); */
    
    printf("[Pipeline] Closed\n");
}

static inline void sdp_print_stats(const ShadowDeltaPipeline *sdp)
{
    if (!sdp) return;
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║  Shadow→Delta Pipeline Stats                  ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    
    uint64_t total_batches = 0;
    uint64_t total_blocks = 0;
    uint64_t total_syncs = 0;
    
    for (int i = 0; i < sdp->num_workers; i++) {
        const FlushWorker *fw = &sdp->workers[i];
        total_batches += fw->batches_processed;
        total_blocks += fw->blocks_written;
        total_syncs += fw->sync_calls;
        
        printf("║ Worker %2d: %8llu batches, %10llu blocks ║\n",
               i,
               (unsigned long long)fw->batches_processed,
               (unsigned long long)fw->blocks_written);
    }
    
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Total:     %8llu batches, %10llu blocks ║\n",
           (unsigned long long)total_batches,
           (unsigned long long)total_blocks);
    printf("║ Syncs:     %8llu (gate-18 rhythm)         ║\n",
           (unsigned long long)total_syncs);
    printf("╚════════════════════════════════════════════════╝\n");
    
    /* Delta stats */
    /* delta_print_stats(&sdp->delta); */
}

#endif /* POGLS_SHADOW_DELTA_WIRED_H */
