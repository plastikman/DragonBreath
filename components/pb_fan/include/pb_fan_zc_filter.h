// SPDX-License-Identifier: MIT
// Pure zero-cross edge filter shared by the Panda ISR and host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Reject the ~1 ms duplicate edges observed on Panda while retaining valid
// 60 Hz (~8.33 ms) and 50 Hz (10 ms) half-cycle edges.
#define PB_FAN_ZC_MIN_INTERVAL_US 4000U

typedef struct {
    volatile uint32_t accepted_count;
    volatile uint32_t accepted_interval_us;
    volatile uint32_t rejected_count;
    volatile uint64_t last_accepted_us;
} pb_fan_zc_filter_t;

void pb_fan_zc_filter_reset(pb_fan_zc_filter_t *state);

// Record one observed edge. Rejected edges increment only rejected_count; they
// never advance accepted timing/count state.
bool pb_fan_zc_filter_record(pb_fan_zc_filter_t *state, uint64_t now_us);
