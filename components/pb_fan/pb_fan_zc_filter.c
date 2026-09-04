// SPDX-License-Identifier: MIT
#include "pb_fan_zc_filter.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

#include <limits.h>

void pb_fan_zc_filter_reset(pb_fan_zc_filter_t *state)
{
    if (!state) return;
    state->accepted_count = 0;
    state->accepted_interval_us = 0;
    state->rejected_count = 0;
    state->last_accepted_us = 0;
}

bool IRAM_ATTR pb_fan_zc_filter_record(pb_fan_zc_filter_t *state,
                                       uint64_t now_us)
{
    if (!state) return false;

    const uint64_t last = state->last_accepted_us;
    if (last != 0) {
        if (now_us <= last || now_us - last < PB_FAN_ZC_MIN_INTERVAL_US) {
            state->rejected_count++;
            return false;
        }

        const uint64_t interval = now_us - last;
        state->accepted_interval_us = interval > UINT32_MAX
            ? UINT32_MAX : (uint32_t)interval;
    }

    state->last_accepted_us = now_us;
    state->accepted_count++;
    return true;
}
