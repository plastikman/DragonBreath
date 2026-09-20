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

// SSR time-proportioning window. Widened from 10 s to 30 s to cut solid-state-relay
// switching wear: at a steady mid-range duty the SSR toggles at most twice per
// window, so a 30 s window caps steady-state switching near ~4/min instead of the
// ~12/min a 10 s window produced. The chamber's thermal time constant is minutes,
// so the coarser window costs no meaningful control quality.
#define PB_HEATER_PID_WINDOW_US  30000000LL
// Minimum SSR on- or off-segment. A window whose ON (or OFF) portion would be
// shorter than this is squelched to a clean full-off (or full-on) window, so duty
// extremes near 0 % / 100 % cannot chatter the relay with sub-dwell blips.
#define PB_HEATER_PID_MIN_DWELL_US 2000000LL

// --- Approach anti-overshoot (soft, rate-gated) ---------------------------------
// The pre-1.1.16-fix cap limited duty purely by distance-to-target, which also
// clamped the STEADY-STATE hold and parked the chamber ~2 C below setpoint for any
// enclosure whose hold duty exceeded the cap. The element (PTC) foldback in
// pb_heater.c is the real thermal ceiling; the approach shaping here exists ONLY to
// damp overshoot while the chamber is still sprinting UP toward a near target. It
// therefore engages only when BOTH the error is inside the band AND the chamber is
// still rising faster than the rate gate. Once the chamber settles (rate at/below
// the gate) the cap releases to full authority, so the integrator can hold whatever
// duty the losses demand and actually reach setpoint.
#define PB_HEATER_PID_APPROACH_BAND_C      3.0f   // shape only within 3 C of target
#define PB_HEATER_PID_APPROACH_FLOOR       0.50f  // deepest damping (at target edge)
// Rate gate: damp only a genuinely fast approach (>0.06 C/s ~= 3.6 C/min). Bench
// data (enclosed U1) showed a foldback-limited steady climb runs ~0.014 C/s while a
// hold's own element-foldback oscillations sit ~0.03-0.05 C/s; both stay below the
// gate so a settled/oscillating hold keeps full authority, while a real fast climb on
// a low-loss enclosure still gets overshoot protection.
#define PB_HEATER_PID_APPROACH_RATE_C_PER_S 0.06f
// The rate fed to the gate is EMA-filtered, because the chamber NTC reports in ~0.1 C
// steps: a single upward quantization tick makes the raw one-sample rate spike to
// ~0.2 C/s, which would trip ANY reasonable gate on nearly every tick and flicker the
// cap through the whole hold. Filtering averages those isolated ticks away (a lone
// spike moves the estimate only ~alpha*spike) while a sustained climb still builds the
// estimate above the gate. alpha 0.20 matches the derivative filter's responsiveness.
#define PB_HEATER_PID_RATE_ALPHA 0.20f

typedef struct {
    dc_pid_state_t controller;
    int64_t window_start_us;
    bool window_initialized;
    bool source_known;
    bool using_external;
    float rate_filt_c_per_s;  // EMA-filtered process-variable rise rate (gate input)
    float last_approach_cap;  // most recent soft cap (1.0 = unshaped); for telemetry
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
    state->rate_filt_c_per_s = 0.0f;
    state->last_approach_cap = 1.0f;
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

// Soft, rate-gated approach ceiling. Returns 1.0 (no shaping) unless the chamber is
// BOTH inside the approach band AND still rising faster than the rate gate, i.e.
// genuinely sprinting toward a near target and liable to overshoot. In that case it
// damps toward PB_HEATER_PID_APPROACH_FLOOR, deepest right at the setpoint. Crucially
// it never clamps a settled hold: at/below the rate gate it returns 1.0, so the
// integrator is free to command whatever steady duty the enclosure's losses require
// and the chamber can actually reach setpoint. rate_c_per_s is the measured rise rate
// of the process variable (positive = warming).
static inline float pb_heater_pid_approach_soft_cap(float error_c, float rate_c_per_s)
{
    if (error_c <= 0.0f) return 0.0f;                          // at/above target
    if (error_c >= PB_HEATER_PID_APPROACH_BAND_C) return 1.0f; // far off → full power
    if (rate_c_per_s <= PB_HEATER_PID_APPROACH_RATE_C_PER_S)
        return 1.0f;                                           // settled/holding → full authority
    // Sprinting up within the band: damp, deepest as the target is approached.
    const float depth = error_c / PB_HEATER_PID_APPROACH_BAND_C; // 0 at target .. 1 at band edge
    return PB_HEATER_PID_APPROACH_FLOOR
         + (1.0f - PB_HEATER_PID_APPROACH_FLOOR) * depth;      // FLOOR .. 1.0
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
    // Measured rise rate of the process variable, from the controller's retained
    // previous measurement (updated by dc_pid_step below), then EMA-filtered to reject
    // 0.1 C sensor-quantization spikes (see PB_HEATER_PID_RATE_ALPHA). Zero on the first
    // sample before history exists, which yields no shaping — the large initial error
    // puts us outside the band anyway.
    const float rate_raw_c_per_s = state->controller.initialized
        ? (measurement_c - state->controller.prev_measurement) / PB_HEATER_PID_DT_S
        : 0.0f;
    state->rate_filt_c_per_s = state->controller.initialized
        ? PB_HEATER_PID_RATE_ALPHA * rate_raw_c_per_s
              + (1.0f - PB_HEATER_PID_RATE_ALPHA) * state->rate_filt_c_per_s
        : 0.0f;
    const float approach_cap =
        pb_heater_pid_approach_soft_cap(error_c, state->rate_filt_c_per_s);
    state->last_approach_cap = approach_cap;
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
    // Minimum-dwell squelch: never open a sub-dwell ON or OFF segment, so duty near
    // 0 % / 100 % resolves to a clean full-off / full-on window instead of chattering
    // the relay with a fraction-of-a-second blip every window.
    if (on_us < PB_HEATER_PID_MIN_DWELL_US)
        return false;  // too little ON demanded → hold OFF this window
    if (PB_HEATER_PID_WINDOW_US - on_us < PB_HEATER_PID_MIN_DWELL_US)
        return true;   // too little OFF left → hold ON this window
    return elapsed < on_us;
}
