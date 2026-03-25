/* pogls_hydra_dynamic.h - Dynamic Hydra Head Management */
#ifndef POGLS_HYDRA_DYNAMIC_H
#define POGLS_HYDRA_DYNAMIC_H

#include <stdint.h>
#include <string.h>
#include <pthread.h>

#define DH_MAX_HEADS 32
#define DH_MIN_HEADS 4
#define DH_QUEUE_SIZE 256

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint64_t tasks[DH_QUEUE_SIZE];
} DHQueue;

typedef struct {
    int heads_min;
    int heads_max;
    int heads_active;
    
    DHQueue queues[DH_MAX_HEADS];
    uint32_t active_mask;  /* bitmap: bit N = head N active */
    
    pthread_t threads[DH_MAX_HEADS];
    volatile int thread_stop[DH_MAX_HEADS];
    
    uint64_t spawn_count;
    uint64_t kill_count;
    
    pthread_mutex_t lock;
} DynamicHydra;

/* Init */
static inline void dh_init(DynamicHydra *dh) {
    memset(dh, 0, sizeof(*dh));
    dh->heads_min = DH_MIN_HEADS;
    dh->heads_max = DH_MAX_HEADS;
    dh->heads_active = DH_MIN_HEADS;
    
    /* activate first N heads */
    for (int i = 0; i < dh->heads_min; i++) {
        dh->active_mask |= (1u << i);
    }
    
    pthread_mutex_init(&dh->lock, NULL);
}

/* Check if head is active */
static inline int dh_is_active(DynamicHydra *dh, int head) {
    return (dh->active_mask & (1u << head)) != 0;
}

/* Spawn new head (if allowed) */
static inline int dh_spawn(DynamicHydra *dh) {
    pthread_mutex_lock(&dh->lock);
    
    if (dh->heads_active >= dh->heads_max) {
        pthread_mutex_unlock(&dh->lock);
        return -1;  /* at max */
    }
    
    /* find first inactive slot */
    int slot = -1;
    for (int i = 0; i < DH_MAX_HEADS; i++) {
        if (!dh_is_active(dh, i)) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        pthread_mutex_unlock(&dh->lock);
        return -1;  /* no slot */
    }
    
    /* activate */
    dh->active_mask |= (1u << slot);
    dh->heads_active++;
    dh->spawn_count++;
    
    /* reset queue */
    dh->queues[slot].head = 0;
    dh->queues[slot].tail = 0;
    
    pthread_mutex_unlock(&dh->lock);
    return slot;
}

/* Kill head (if > min) */
static inline int dh_kill(DynamicHydra *dh, int head) {
    pthread_mutex_lock(&dh->lock);
    
    if (dh->heads_active <= dh->heads_min) {
        pthread_mutex_unlock(&dh->lock);
        return -1;  /* at min */
    }
    
    if (!dh_is_active(dh, head)) {
        pthread_mutex_unlock(&dh->lock);
        return -1;  /* already inactive */
    }
    
    /* deactivate */
    dh->active_mask &= ~(1u << head);
    dh->heads_active--;
    dh->kill_count++;
    
    pthread_mutex_unlock(&dh->lock);
    return 0;
}

/* Push to head (if active) */
static inline int dh_push(DynamicHydra *dh, int head, uint64_t task) {
    if (!dh_is_active(dh, head)) return -1;
    
    DHQueue *q = &dh->queues[head];
    uint32_t next = (q->tail + 1) % DH_QUEUE_SIZE;
    
    if (next == q->head) return -1;  /* full */
    
    q->tasks[q->tail] = task;
    __sync_synchronize();
    q->tail = next;
    return 0;
}

/* Pop from head */
static inline int dh_pop(DynamicHydra *dh, int head, uint64_t *out) {
    if (!dh_is_active(dh, head)) return 0;
    
    DHQueue *q = &dh->queues[head];
    
    if (q->head == q->tail) return 0;  /* empty */
    
    *out = q->tasks[q->head];
    __sync_synchronize();
    q->head = (q->head + 1) % DH_QUEUE_SIZE;
    return 1;
}

/* Get queue depth */
static inline uint32_t dh_depth(DynamicHydra *dh, int head) {
    if (!dh_is_active(dh, head)) return 0;
    
    DHQueue *q = &dh->queues[head];
    return (q->tail + DH_QUEUE_SIZE - q->head) % DH_QUEUE_SIZE;
}

/* Get total depth across all active heads */
static inline uint32_t dh_total_depth(DynamicHydra *dh) {
    uint32_t total = 0;
    for (int i = 0; i < DH_MAX_HEADS; i++) {
        if (dh_is_active(dh, i)) {
            total += dh_depth(dh, i);
        }
    }
    return total;
}

/* Auto-scale based on load */
static inline void dh_autoscale(DynamicHydra *dh) {
    pthread_mutex_lock(&dh->lock);
    
    uint32_t total_depth = dh_total_depth(dh);
    uint32_t avg_depth = dh->heads_active > 0 ? 
                         total_depth / dh->heads_active : 0;
    
    /* spawn if overloaded */
    if (avg_depth > DH_QUEUE_SIZE * 3 / 4 && 
        dh->heads_active < dh->heads_max) {
        pthread_mutex_unlock(&dh->lock);
        dh_spawn(dh);
        return;
    }
    
    /* kill if underutilized */
    if (avg_depth < DH_QUEUE_SIZE / 4 && 
        dh->heads_active > dh->heads_min) {
        /* find least loaded head */
        int min_head = -1;
        uint32_t min_depth = UINT32_MAX;
        
        for (int i = 0; i < DH_MAX_HEADS; i++) {
            if (!dh_is_active(dh, i)) continue;
            uint32_t d = dh_depth(dh, i);
            if (d < min_depth) {
                min_depth = d;
                min_head = i;
            }
        }
        
        pthread_mutex_unlock(&dh->lock);
        if (min_head >= 0) {
            dh_kill(dh, min_head);
        }
        return;
    }
    
    pthread_mutex_unlock(&dh->lock);
}

#endif /* POGLS_HYDRA_DYNAMIC_H */
