/* pogls_resource_guard.h - Production Resource Monitor */
#ifndef POGLS_RESOURCE_GUARD_H
#define POGLS_RESOURCE_GUARD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Thresholds (tunable) */
#define RG_RAM_MIN_KB       (512 * 1024)    /* 512 MB free minimum */
#define RG_CPU_MAX_PCT      80.0            /* 80% CPU max */
#define RG_HYDRA_MIN        4               /* min heads */
#define RG_HYDRA_MAX        32              /* max heads */
#define RG_UPDATE_INTERVAL  1               /* update every 1s */

typedef struct {
    /* metrics */
    size_t ram_total_kb;
    size_t ram_free_kb;
    size_t ram_avail_kb;
    double cpu_load_pct;
    int hydra_heads_active;
    
    /* limits */
    int hydra_heads_max;
    
    /* flags */
    volatile int block_expand;
    volatile int force_shrink;
    volatile int throttle_rate;
    
    /* internal */
    uint64_t last_update_sec;
    uint64_t cpu_prev_total;
    uint64_t cpu_prev_idle;
} ResourceGuard;

/* Init */
static inline void rg_init(ResourceGuard *rg) {
    memset(rg, 0, sizeof(*rg));
    rg->hydra_heads_max = RG_HYDRA_MAX;
    rg->hydra_heads_active = RG_HYDRA_MIN;  /* start conservative */
}

/* Read /proc/meminfo (Linux) */
static inline int rg_read_ram(ResourceGuard *rg) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %zu kB", &rg->ram_total_kb) == 1) continue;
        if (sscanf(line, "MemFree: %zu kB", &rg->ram_free_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %zu kB", &rg->ram_avail_kb) == 1) continue;
    }
    fclose(f);
    return 0;
}

/* Read /proc/stat (Linux) */
static inline int rg_read_cpu(ResourceGuard *rg) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    
    uint64_t user, nice, system, idle, iowait, irq, softirq;
    if (fscanf(f, "cpu %lu %lu %lu %lu %lu %lu %lu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
        fclose(f);
        return -1;
    }
    fclose(f);
    
    uint64_t total = user + nice + system + idle + iowait + irq + softirq;
    
    if (rg->cpu_prev_total > 0) {
        uint64_t d_total = total - rg->cpu_prev_total;
        uint64_t d_idle = idle - rg->cpu_prev_idle;
        
        if (d_total > 0) {
            rg->cpu_load_pct = 100.0 * (d_total - d_idle) / d_total;
        }
    }
    
    rg->cpu_prev_total = total;
    rg->cpu_prev_idle = idle;
    return 0;
}

/* Update all metrics */
static inline void rg_update(ResourceGuard *rg) {
    uint64_t now = (uint64_t)time(NULL);
    if (now - rg->last_update_sec < RG_UPDATE_INTERVAL) return;
    
    rg_read_ram(rg);
    rg_read_cpu(rg);
    
    /* Apply thresholds */
    rg->block_expand = 0;
    rg->force_shrink = 0;
    rg->throttle_rate = 0;
    
    if (rg->ram_avail_kb < RG_RAM_MIN_KB) {
        rg->block_expand = 1;
        rg->force_shrink = 1;
        rg->throttle_rate = 1;
    }
    
    if (rg->cpu_load_pct > RG_CPU_MAX_PCT) {
        rg->throttle_rate = 1;
    }
    
    rg->last_update_sec = now;
}

/* Check: can expand Shell-N? */
static inline int rg_can_expand(ResourceGuard *rg) {
    rg_update(rg);
    return !rg->block_expand;
}

/* Check: can spawn new Hydra head? */
static inline int rg_can_spawn_hydra(ResourceGuard *rg) {
    rg_update(rg);
    if (rg->block_expand) return 0;
    if (rg->hydra_heads_active >= rg->hydra_heads_max) return 0;
    if (rg->ram_avail_kb < RG_RAM_MIN_KB * 2) return 0;  /* extra margin */
    return 1;
}

/* Check: should shrink? */
static inline int rg_should_shrink(ResourceGuard *rg) {
    rg_update(rg);
    return rg->force_shrink;
}

/* Check: should throttle? */
static inline int rg_should_throttle(ResourceGuard *rg) {
    rg_update(rg);
    return rg->throttle_rate;
}

/* Print status */
static inline void rg_print(ResourceGuard *rg) {
    printf("[RG] RAM: %zu/%zu MB avail  CPU: %.1f%%  Hydra: %d/%d heads  "
           "flags: exp=%d shrink=%d throttle=%d\n",
           rg->ram_avail_kb / 1024, rg->ram_total_kb / 1024,
           rg->cpu_load_pct, rg->hydra_heads_active, rg->hydra_heads_max,
           !rg->block_expand, rg->force_shrink, rg->throttle_rate);
}

#endif /* POGLS_RESOURCE_GUARD_H */
