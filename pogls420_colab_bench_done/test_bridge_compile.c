/*
 * test_bridge_compile.c — V4x Wire + Federation Bridge
 *
 * Problem: storage/pogls_delta.h defines delta_append(DeltaWriter*,4 args)
 *          core_c/pogls_delta.h  defines delta_append(Delta_Context*,5 args)
 * Solution: alias storage version before include, restore after.
 */

/* ── Step 1: V4x wire path with storage delta ─────────────────── */
#define delta_append        v4_delta_append_storage
#define delta_append_batch  v4_delta_append_batch_storage
#include "storage/pogls_delta.h"
#undef delta_append
#undef delta_append_batch
#include "pogls_v4x_wire.h"

/* ── Step 2: Federation path with core_c delta ─────────────────── */
#include <stdbool.h>
#include "core_c/pogls_delta.h"
#include "core_c/pogls_delta_world_b.h"
#include "pogls_federation.h"
#include "pogls_v4x_fed_bridge.h"
#include <stdio.h>

int main(void) {
    V4xWire       w;
    FederationCtx fed;

    v4x_wire_init(&w, 4);
    fed_init(&fed, "/tmp/bridge_test");

    /* 720 steps = 1 full cycle through both V4x + federation */
    for (uint32_t s = 0; s < 720; s++)
        v4x_fed_step(&w, &fed, 36 + s * 3);

    v4x_fed_flush(&w, &fed);

    printf("bridge_ok cycle_ends=%llu fed_epoch=%llu commit_pending=%u\n",
           (unsigned long long)w.cycle_ends,
           (unsigned long long)fed.ss.epoch,
           fed.commit_pending);
    printf("snap_certified=%llu gate_passed=%llu\n",
           (unsigned long long)w.snap_certified,
           (unsigned long long)fed.gate.passed);
    fed_close(&fed);
    return 0;
}
