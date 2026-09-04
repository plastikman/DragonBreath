// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "dc_pid.h"

// DragonBreath-owned chamber-controller policy. dc_pid supplies only the generic
// PID math; sensor selection, safety inhibition, approach limiting, and the SSR
// time-proportioning actuator remain product responsibilities.
#define PB_HEATER_PID_KP         0.1000f
#define PB_HEATER_PID_KI         0.0010f
#define PB_HEATER_PID_KD         0.0400f
#define PB_HEATER_PID_D_ALPHA    0.20f
#define PB_HEATER_PID_DT_S       0.50f
#define PB_HEATER_PID_WINDOW_US  10000000LL

typedef struct {
    dc_pid_state_t controller;
    int64_t window_start_us;
    bool window_initialized;
    bool source_known;
    bool using_external;
} pb_heater_pid_state_t;

// Select the effective process variable while keeping source policy explicit.
// The caller decides whether external telemetry is eligible; NAN represents
// unavailable/stale/not-authorized and therefore falls back to the local NTC.
static inline float pb_heater_pid_process_variable(float local_chamber_c,
                                                    float external_chamber_c,
                                                    bool *using_external)
{
    bool external = isfinite(external_chamber_c);
    if (using_external) *using_external = external;
    return external ? external_chamber_c : local_chamber_c;
}

static inline void pb_heater_pid_reset(pb_heater_pid_state_t *state)
{
    if (!state) return;
    dc_pid_reset(&state->controller);
    state->window_start_us = 0;
    state->window_initialized = false;
    state->source_known = false;
    state->using_external = false;
}

// Prevent derivative/integral history from crossing between physically distinct
// chamber sensors. Returns true when a new source became active.
static inline bool pb_heater_pid_set_source(pb_heater_pid_state_t *state,
                                            bool using_external)
{
    if (!state) return false;
    if (state->source_known && state->using_external == using_external)
        return false;

    pb_heater_pid_reset(state);
    state->source_known = true;
    state->using_external = using_external;
    return true;
}

static inline float pb_heater_pid_approach_max_duty(float error_c)
{
    if (error_c <= 0.0f) return 0.0f;
    if (error_c < 2.0f) return 0.40f;
    if (error_c < 5.0f) return 0.70f;
    return 1.0f;
}

// Advance the common chamber PID path. When integrate is false, dc_pid still
// updates measurement/derivative history but holds the integral so product
// safety governors cannot hide accumulating demand behind an inhibited heater.
// DragonBreath's heater-only policy commands zero at/above target without
// discarding valid controller history.
static inline bool pb_heater_pid_step(pb_heater_pid_state_t *state,
                                      float target_c, float measurement_c,
                                      bool integrate, float *duty)
{
    if (duty) *duty = 0.0f;
    if (!state || !duty) return false;

    const float error_c = target_c - measurement_c;
    const float approach_cap = pb_heater_pid_approach_max_duty(error_c);
    const float output_max = approach_cap > 0.0f ? approach_cap : 1.0f;
    const float proportional = PB_HEATER_PID_KP * error_c;
    // Keep stored, non-derivative demand inside the actuator range that
    // DragonBreath currently exposes. dc_pid normalizes an existing integral
    // against a contracted bound transactionally, so a 1.00 -> 0.70 -> 0.40
    // approach transition cannot leave hidden integral behind the tighter cap.
    // Derivative/history continuity is preserved; a transient derivative may
    // still move the requested output within dc_pid's active output range.
    float integral_max = output_max - proportional;
    if (integral_max < 0.0f) integral_max = 0.0f;
    if (integral_max > 1.0f) integral_max = 1.0f;
    const dc_pid_config_t config = {
        .kp = PB_HEATER_PID_KP,
        .ki = PB_HEATER_PID_KI,
        .kd = PB_HEATER_PID_KD,
        .derivative_alpha = PB_HEATER_PID_D_ALPHA,
        .output_min = 0.0f,
        // The approach ceiling is normal actuator shaping, so dc_pid must see
        // it while deciding whether additional integration would wind up.
        // At/above target the heater-only policy below commands zero; retain a
        // valid controller range so history and negative-error unwinding advance.
        .output_max = output_max,
        .integral_min = 0.0f,
        .integral_max = integral_max,
    };
    dc_pid_result_t result;
    if (!dc_pid_step(&state->controller, &config, target_c, measurement_c,
                     PB_HEATER_PID_DT_S, integrate, &result))
        return false;

    if (measurement_c >= target_c)
        return true;

    *duty = result.output;
    return true;
}

// Convert normalized duty into zero-cross-SSR time proportioning. `now_us`
// may jump backwards across a timer reset; that simply starts a fresh window.
static inline bool pb_heater_pid_window_on(pb_heater_pid_state_t *state,
                                           float duty, int64_t now_us)
{
    if (!state || duty <= 0.0f) return false;
    if (duty > 1.0f) duty = 1.0f;

    if (!state->window_initialized || now_us < state->window_start_us) {
        state->window_start_us = now_us;
        state->window_initialized = true;
    }

    int64_t elapsed = now_us - state->window_start_us;
    if (elapsed >= PB_HEATER_PID_WINDOW_US) {
        int64_t windows = elapsed / PB_HEATER_PID_WINDOW_US;
        state->window_start_us += windows * PB_HEATER_PID_WINDOW_US;
        elapsed = now_us - state->window_start_us;
    }

    int64_t on_us = (int64_t)(duty * (float)PB_HEATER_PID_WINDOW_US);
    return on_us > 0 && elapsed < on_us;
}
