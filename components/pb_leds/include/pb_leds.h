// SPDX-License-Identifier: MIT
// pb_leds — the three button-indicator LEDs (K1/K2/K3, active-high via 1K on
// GPIO6/5/4). One 50 ms-tick task drives all three from an atomic per-LED
// pattern, so any task can set a pattern without touching GPIO or blocking.
// pb_heater no longer drives any LED; pb_policy owns the indication.
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Physical panel labels (top→bottom on the board: Power, Dry, On, Auto), mapped
// to their driving GPIO. Determined by bench probe. The "Power" light is hardwired
// on (not on any of these pins) and is not represented here.
typedef enum {
    PB_LED_K1 = 0,   // GPIO6 = "Auto"
    PB_LED_K2 = 1,   // GPIO5 = "On"  (heater indicator)
    PB_LED_K3 = 2,   // GPIO4 = "Dry"
    PB_LED_COUNT = 3,
    // Semantic aliases (use these; the K-names are the physical button positions).
    PB_LED_AUTO = PB_LED_K1,
    PB_LED_ON   = PB_LED_K2,
    PB_LED_DRY  = PB_LED_K3,
} pb_led_id_t;

typedef enum {
    PB_LED_OFF = 0,
    PB_LED_SOLID,
    PB_LED_BLINK,        // ~2.5 Hz
    PB_LED_BLINK_SLOW,   // ~0.5 Hz
    PB_LED_CODE,         // N short pulses, gap, repeat (count via pb_leds_set_code)
} pb_led_pattern_t;

// Configure the 3 LED GPIOs (driven low) and start the driver task. Idempotent-
// guarded: a second call returns ESP_ERR_INVALID_STATE.
esp_err_t pb_leds_start(void);

// Set a LED's pattern (atomic; safe from any task).
void pb_leds_set(pb_led_id_t id, pb_led_pattern_t pattern);

// Set a LED to the CODE pattern blinking `pulses` short pulses per cycle.
void pb_leds_set_code(pb_led_id_t id, uint8_t pulses);
