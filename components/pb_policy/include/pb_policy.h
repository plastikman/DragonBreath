// SPDX-License-Identifier: MIT
// pb_policy — glue between the network layer (Moonraker/HA, imported from the
// OpenVent shared core) and the device actuators (pb_heater, pb_fan).
//
// It owns the automation rules: translate a desired chamber set-point + printer
// state into heater target + fan level, and feed the heater comms watchdog while
// a controller link is alive. Kept deliberately thin so the same shape can be
// shared across the Panda family.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float chamber_target_c;   // requested chamber set-point (0 = off)
    uint8_t fan_percent;      // requested fan speed 0..100
    bool link_alive;          // a controller (Moonraker/HA) is currently connected
} pb_policy_input_t;

esp_err_t pb_policy_init(void);

// Apply the latest desired state from the controller. Safe to call on every
// update / message. Applies clamps and feeds the heater watchdog.
void pb_policy_apply(const pb_policy_input_t *in);

// Periodic tick (call at ~1-2 Hz): drives pb_heater_tick() and any local
// autonomous behaviour (e.g. run the fan while the heater is on).
void pb_policy_tick(void);
