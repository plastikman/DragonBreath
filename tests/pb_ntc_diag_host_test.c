// SPDX-License-Identifier: MIT
// Host-only validation of the bounded diagnostic ring (no ESP-IDF or ADC).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "pb_ntc_diag_ring.h"

static pb_ntc_diag_ring_t ring;
static pb_ntc_diag_sample_t result[PB_NTC_DIAG_CAPACITY];

static pb_ntc_diag_sample_t sample(unsigned n)
{
    pb_ntc_diag_sample_t s = {
        .timestamp_us = (uint64_t)n * 500000,
        .raw = (int)n,
        .millivolts = (int)n + 100,
        .temperature_c = (float)n,
        .status = PB_NTC_OK,
        .channel = n % 2 ? PB_NTC_PTC : PB_NTC_CHAMBER,
        .source_index = (int)(n % 2),
    };
    return s;
}

int main(void)
{
    assert(pb_ntc_diag_ring_copy(&ring, result, PB_NTC_DIAG_CAPACITY) == 0);
    assert(pb_ntc_diag_ring_copy(&ring, NULL, PB_NTC_DIAG_CAPACITY) == 0);
    assert(pb_ntc_diag_ring_copy(&ring, result, 0) == 0);

    for (unsigned i = 0; i < PB_NTC_DIAG_CAPACITY + 20; i++) {
        pb_ntc_diag_sample_t s = sample(i);
        pb_ntc_diag_ring_push(&ring, &s);
    }
    assert(ring.count == PB_NTC_DIAG_CAPACITY);
    size_t n = pb_ntc_diag_ring_copy(&ring, result, PB_NTC_DIAG_CAPACITY);
    assert(n == PB_NTC_DIAG_CAPACITY);
    for (size_t i = 0; i < n; i++) {
        unsigned expected = (unsigned)i + 20;
        assert(result[i].raw == (int)expected);
        assert(result[i].millivolts == (int)expected + 100);
        assert(result[i].timestamp_us == (uint64_t)expected * 500000);
        assert(result[i].source_index == (int)(expected % 2));
    }

    n = pb_ntc_diag_ring_copy(&ring, result, 3);
    assert(n == 3);
    assert(result[0].raw == PB_NTC_DIAG_CAPACITY + 17);
    assert(result[1].raw == PB_NTC_DIAG_CAPACITY + 18);
    assert(result[2].raw == PB_NTC_DIAG_CAPACITY + 19);
    assert(ring.count == PB_NTC_DIAG_CAPACITY); // reading never consumes records

    pb_ntc_diag_sample_t bad = sample(999);
    bad.raw = -1;
    bad.millivolts = -1;
    bad.status = PB_NTC_UNINIT;
    pb_ntc_diag_ring_push(&ring, &bad);
    n = pb_ntc_diag_ring_copy(&ring, result, 1);
    assert(n == 1);
    assert(result[0].status == PB_NTC_UNINIT);
    assert(result[0].raw == -1);
    puts("ADC diagnostic ring: PASS");
    return 0;
}
