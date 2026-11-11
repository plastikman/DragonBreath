// SPDX-License-Identifier: MIT
#pragma once
// Pure, bounded storage for ADC samples. Synchronization belongs to callers.
#include "pb_ntc.h"

typedef struct {
    pb_ntc_diag_sample_t entries[PB_NTC_DIAG_CAPACITY];
    size_t next;
    size_t count;
} pb_ntc_diag_ring_t;

static inline void pb_ntc_diag_ring_push(pb_ntc_diag_ring_t *ring,
                                          const pb_ntc_diag_sample_t *sample)
{
    ring->entries[ring->next] = *sample;
    ring->next = (ring->next + 1) % PB_NTC_DIAG_CAPACITY;
    if (ring->count < PB_NTC_DIAG_CAPACITY) ring->count++;
}

// Snapshot most recent min(count,max) samples, oldest first. No dynamic memory.
// External synchronization must cover this entire call when producers run.
static inline size_t pb_ntc_diag_ring_copy(const pb_ntc_diag_ring_t *ring,
                                           pb_ntc_diag_sample_t *out, size_t max)
{
    if (!out || !max) return 0;
    size_t count = ring->count < max ? ring->count : max;
    size_t start = (ring->next + PB_NTC_DIAG_CAPACITY - count)
                   % PB_NTC_DIAG_CAPACITY;
    for (size_t i = 0; i < count; ++i)
        out[i] = ring->entries[(start + i) % PB_NTC_DIAG_CAPACITY];
    return count;
}
