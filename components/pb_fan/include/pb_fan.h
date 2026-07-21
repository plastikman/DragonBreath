// SPDX-License-Identifier: MIT
// pb_fan — AC blower speed control via TRIAC phase-angle firing.
//
// Hardware (RE'd): GPIO3 gate -> MOC3021 random-phase opto-triac -> BT136-800E
// -> ~220VAC EC blower (STKJ ST7530B220H). A zero-cross detector (MB6S bridge +
// TLP785 opto) pulses GPIO7 at each mains zero crossing. Speed = delay the gate
// pulse after each zero cross (leading-edge phase control); longer delay = lower
// power. This mirrors the stock firmware's approach.
//
// WARNING: this drives mains-referenced hardware. Get the ISR timing right on
// the bench before trusting it. min/max phase clamps prevent gate firing too
// close to the zero cross (unreliable) or too late (missed half-cycle).
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Configure the gate output (idle low = TRIAC off) and the zero-cross ISR.
esp_err_t pb_fan_init(void);

// Set fan speed 0-100 (%). 0 = off (no gate pulses), 100 = full (fire immediately
// after each zero cross). Values in between map to a phase delay.
void pb_fan_set_level(uint8_t percent);

uint8_t pb_fan_get_level(void);

// Bring-up diagnostics. count_out = total zero-cross interrupts seen since boot;
// interval_us_out = microseconds between the last two edges (the mains
// half-period: ~10000 us => 50 Hz mains, ~8333 us => 60 Hz). interval 0 means no
// zero-cross has been detected yet (ZCD not working / wrong pin / no mains).
void pb_fan_zc_diag(uint32_t *count_out, uint32_t *interval_us_out);
