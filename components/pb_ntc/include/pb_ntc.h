// SPDX-License-Identifier: MIT
// pb_ntc — chamber + PTC-element temperature, reproducing the stock firmware's
// exact ADC->temperature conversion (reverse-engineered; see docs/NTC_CONVERSION.md).
//
//   Rntc_kOhm = Rref * V / (Vrail - V)      low-side divider, Vrail = 0.1 V
//   temp_C    = interpolate(114-entry R/T table)   (stock uses nearest-entry)
//
// Rref (82 or 33 kOhm) is selected by the GPIO19 strap at init.
#pragma once

#include "esp_err.h"

typedef enum {
    PB_NTC_OK = 0,      // reading valid
    PB_NTC_SHORT,       // raw under-range (thermistor shorted / very hot)
    PB_NTC_OPEN,        // raw over-range (thermistor open / disconnected)
    PB_NTC_UNINIT,      // pb_ntc_init not called / failed
} pb_ntc_status_t;

typedef enum {
    PB_NTC_CHAMBER = 0,
    PB_NTC_PTC = 1,
} pb_ntc_channel_t;

// Initialize ADC1 oneshot + curve-fit calibration for both channels and latch
// Rref from the board strap. Returns ESP_OK on success.
esp_err_t pb_ntc_init(void);

// Take one calibrated reading of the given channel.
// On PB_NTC_OK, *out_c holds the temperature in degrees C. Returns the status.
pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c);

// Convenience: latest smoothed temperature (5-sample moving average, matching
// the stock filter). NAN if no valid reading yet. Call pb_ntc_read periodically
// (e.g. 1 Hz) to feed the average.
float pb_ntc_smoothed_c(pb_ntc_channel_t ch);
