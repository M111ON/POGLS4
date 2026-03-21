/* pogls_spectre_delta_bridge.h - Wire Spectre → Rewind → Delta */
#ifndef POGLS_SPECTRE_DELTA_BRIDGE_H
#define POGLS_SPECTRE_DELTA_BRIDGE_H

#include <stdint.h>
#include <string.h>

/* Forward declarations (ใช้ตัวจริงจาก project) */
typedef struct DiamondBlock DiamondBlock;
typedef struct RewindBuffer RewindBuffer;

/* Spectre → Rewind → Delta pipeline */
typedef struct {
    /* Layers */
    void *spectre_ring;      /* SpectreRing* */
    void *rewind_buffer;     /* RewindBuffer* */
    void *delta_context;     /* Delta writer context */
    
    /* Lane buffers (54 shards) */
    struct {
        DiamondBlock blocks[2048];  /* payload staging */
        uint32_t count;
        uint32_t start_index;
    } lanes[54];
    
    /* Stats */
    uint64_t batches_formed;
    uint64_t rewind_pushes;
    uint64_t delta_commits;
    
    /* Flush worker control */
    int flush_workers_active;
    volatile int stop_flush;
} SpectreDeltaBridge;

/* Initialize bridge */
static inline int sdb_init(SpectreDeltaBridge *br) {
    memset(br, 0, sizeof(*br));
    br->flush_workers_active = 4;  /* default */
    return 0;
}

/* Push DiamondBlock through pipeline */
static inline int sdb_push(SpectreDeltaBridge *br, 
                           DiamondBlock *block,
                           uint64_t angular_addr)
{
    /* Route to lane (Rubik 54-shard) */
    int lane = (int)(angular_addr % 54);
    
    /* Stage in lane buffer */
    if (br->lanes[lane].count < 2048) {
        br->lanes[lane].blocks[br->lanes[lane].count++] = *block;
    } else {
        return -1;  /* lane full */
    }
    
    /* Form batch when count reaches gate_18 multiple */
    if (br->lanes[lane].count >= 256) {
        /* TODO: Push to Spectre
         * SpectreEntry entry = {
         *     .lane = lane,
         *     .start_index = br->lanes[lane].start_index,
         *     .count = br->lanes[lane].count,
         *     .angular_addr = angular_addr
         * };
         * spectre_push(br->spectre_ring, &entry);
         */
        
        br->batches_formed++;
        br->lanes[lane].start_index += br->lanes[lane].count;
        br->lanes[lane].count = 0;
    }
    
    return 0;
}

/* Flush worker: Spectre → Rewind → Delta */
static inline int sdb_flush_batch(SpectreDeltaBridge *br,
                                  int lane,
                                  uint32_t start_idx,
                                  uint32_t count)
{
    /* Step 1: Get batch from lane buffer */
    DiamondBlock *batch = &br->lanes[lane].blocks[start_idx % 2048];
    
    /* Step 2: Push to Rewind (18-slot gate) */
    for (uint32_t i = 0; i < count; i += 18) {
        uint32_t gate_size = (i + 18 <= count) ? 18 : (count - i);
        
        /* TODO: Wire to real rewind_push_gate()
         * rewind_push_gate(br->rewind_buffer, 
         *                  &batch[i], 
         *                  gate_size);
         */
        
        br->rewind_pushes += gate_size;
    }
    
    /* Step 3: Rewind auto-flushes to Delta at gate boundary */
    /* (no explicit call needed — rewind handles it) */
    
    br->delta_commits++;
    return 0;
}

/* Stats */
static inline void sdb_print_stats(SpectreDeltaBridge *br) {
    printf("[Bridge] batches=%llu rewind=%llu delta=%llu\n",
           (unsigned long long)br->batches_formed,
           (unsigned long long)br->rewind_pushes,
           (unsigned long long)br->delta_commits);
}

#endif /* POGLS_SPECTRE_DELTA_BRIDGE_H */
