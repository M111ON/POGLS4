/* pogls_rate_limiter.h - Token Bucket Rate Limiter */
#ifndef POGLS_RATE_LIMITER_H
#define POGLS_RATE_LIMITER_H

#include <stdint.h>
#include <time.h>

/* Rate limits (tunable) */
#define RL_REWIND_MAX_RATE      1000000ULL  /* 1M ops/s per thread */
#define RL_HYDRA_SPAWN_MIN_SEC  5           /* min 5s between spawns */
#define RL_FLUSH_MAX_RATE       100         /* 100 flushes/s */

typedef struct {
    uint64_t tokens;            /* current tokens */
    uint64_t max_tokens;        /* bucket capacity */
    uint64_t refill_rate;       /* tokens per second */
    uint64_t last_refill_ns;    /* last refill timestamp */
    uint64_t acquired_total;    /* stats */
    uint64_t rejected_total;    /* stats */
} RateLimiter;

/* Get monotonic time in nanoseconds */
static inline uint64_t rl_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Init limiter */
static inline void rl_init(RateLimiter *rl, uint64_t max_rate) {
    rl->max_tokens = max_rate;
    rl->tokens = max_rate;  /* start full */
    rl->refill_rate = max_rate;
    rl->last_refill_ns = rl_now_ns();
    rl->acquired_total = 0;
    rl->rejected_total = 0;
}

/* Refill tokens based on elapsed time */
static inline void rl_refill(RateLimiter *rl) {
    uint64_t now = rl_now_ns();
    uint64_t elapsed_ns = now - rl->last_refill_ns;
    
    if (elapsed_ns > 0) {
        /* tokens = rate × time */
        uint64_t new_tokens = (rl->refill_rate * elapsed_ns) / 1000000000ULL;
        
        rl->tokens += new_tokens;
        if (rl->tokens > rl->max_tokens) {
            rl->tokens = rl->max_tokens;  /* cap at max */
        }
        
        rl->last_refill_ns = now;
    }
}

/* Try to acquire tokens (non-blocking) */
static inline int rl_try_acquire(RateLimiter *rl, uint64_t cost) {
    rl_refill(rl);
    
    if (rl->tokens >= cost) {
        rl->tokens -= cost;
        rl->acquired_total++;
        return 0;  /* success */
    }
    
    rl->rejected_total++;
    return -1;  /* rate limit hit */
}

/* Blocking acquire with backpressure */
static inline void rl_acquire(RateLimiter *rl, uint64_t cost) {
    while (rl_try_acquire(rl, cost) != 0) {
        /* backpressure: yield CPU */
        struct timespec ts = {0, 1000000};  /* 1ms */
        nanosleep(&ts, NULL);
    }
}

/* Get rejection rate (0.0 - 1.0) */
static inline double rl_rejection_rate(RateLimiter *rl) {
    uint64_t total = rl->acquired_total + rl->rejected_total;
    if (total == 0) return 0.0;
    return (double)rl->rejected_total / total;
}

/* Reset stats */
static inline void rl_reset_stats(RateLimiter *rl) {
    rl->acquired_total = 0;
    rl->rejected_total = 0;
}

#endif /* POGLS_RATE_LIMITER_H */
