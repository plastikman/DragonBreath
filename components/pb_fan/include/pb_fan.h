// SPDX-License-Identifier: MIT
// pb_fan — AC blower ON/OFF control (matches stock firmware; see docs/FAN_DRIVE.md).
//
// Stock drives the fan gate as a HELD level switched at zero-cross (HIGH = full
// on, LOW = off) — no phase-angle, no PWM, no variable speed. The gate must
// NEVER be PWM'd (free-running switching destroys the TRIAC). This driver mirrors
// stock: on/off only.
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Configure the gate output (idle low) + the zero-cross ISR (clean AC switching).
esp_err_t pb_fan_init(void);

// Fan on/off. 0 = off; any non-zero = on (no variable speed — intermediate values
// are treated as "on"). Off takes effect immediately; on is applied at the next
// zero crossing.
void pb_fan_set_level(uint8_t percent);
uint8_t pb_fan_get_level(void);

// Diagnostics: accepted edges since boot, interval between the last two accepted
// edges, and rejected implausibly-close edges. interval 0 = fewer than two
// accepted edges (ZCD not working / no mains).
void pb_fan_zc_diag(uint32_t *count_out, uint32_t *interval_us_out,
                    uint32_t *rejected_count_out);

#ifdef CONFIG_PB_DEVBOARD_SAFE
// Inject one or more zero-cross events into the dev-board backend.
void pb_fan_hil_zero_cross(uint32_t count, uint32_t interval_us);
#endif
