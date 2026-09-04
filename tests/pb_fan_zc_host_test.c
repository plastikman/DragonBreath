// SPDX-License-Identifier: MIT
#include "pb_fan_zc_filter.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    pb_fan_zc_filter_t state = {0};
    pb_fan_zc_filter_reset(&state);

    assert(pb_fan_zc_filter_record(&state, 1000000));
    assert(state.accepted_count == 1);
    assert(state.accepted_interval_us == 0);

    // The observed ~0.99 ms companion edge is rejected and cannot move the
    // accepted timestamp or interval.
    assert(!pb_fan_zc_filter_record(&state, 1000990));
    assert(state.accepted_count == 1);
    assert(state.rejected_count == 1);
    assert(state.last_accepted_us == 1000000);
    assert(state.accepted_interval_us == 0);

    // The following real 60 Hz half-cycle remains 8.33 ms from the last
    // accepted edge even though a rejected edge occurred in between.
    assert(pb_fan_zc_filter_record(&state, 1008333));
    assert(state.accepted_count == 2);
    assert(state.accepted_interval_us == 8333);

    // A 50 Hz half-cycle remains valid.
    assert(pb_fan_zc_filter_record(&state, 1018333));
    assert(state.accepted_count == 3);
    assert(state.accepted_interval_us == 10000);

    assert(!pb_fan_zc_filter_record(&state, 1022332));
    assert(state.rejected_count == 2);
    assert(state.last_accepted_us == 1018333);

    // The threshold is inclusive: 3999 us is rejected, exactly 4000 us is
    // accepted.
    assert(pb_fan_zc_filter_record(&state, 1022333));
    assert(state.accepted_count == 4);
    assert(state.accepted_interval_us == PB_FAN_ZC_MIN_INTERVAL_US);

    puts("pb_fan zero-cross filter checks: PASS");
    return 0;
}
