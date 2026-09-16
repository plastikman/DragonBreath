// Host regression coverage for DragonBreath's dc_pid adapter and product-owned
// chamber/SSR shaping. Heater safety itself remains in pb_heater.h.
#include "pb_heater.h"
#include "pb_heater_pid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabsf((a) - (b)) <= (eps))

static void test_known_gains_and_heater_policy(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = -1.0f;

    CHECK(pb_heater_pid_step(&state, 60.0f, 52.0f, true, &duty));
    CHECK(isfinite(duty));
    CHECK(duty > 0.80f && duty < 0.81f);
    CHECK(state.controller.initialized);

    for (int i = 0; i < 120; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 52.0f, true, &duty));
    CHECK(duty > 0.99f && duty <= 1.0f);

    // DragonBreath is heater-only: exact/above target commands zero, while
    // dc_pid history remains coherent rather than being reset at every crossing.
    CHECK(pb_heater_pid_step(&state, 60.0f, 60.0f, true, &duty));
    CHECK(duty == 0.0f);
    CHECK(state.controller.initialized);
    CHECK(pb_heater_pid_step(&state, 60.0f, 61.0f, true, &duty));
    CHECK(duty == 0.0f);
    CHECK(state.controller.prev_measurement == 61.0f);
}

static void test_process_variable_selection(void)
{
    bool external = true;
    CHECK(pb_heater_pid_process_variable(53.0f, NAN, &external) == 53.0f);
    CHECK(!external);
    CHECK(pb_heater_pid_process_variable(53.0f, 31.0f, &external) == 31.0f);
    CHECK(external);
}

static void test_approach_soft_cap(void)
{
    // At/above target the heater-only policy commands nothing.
    CHECK(pb_heater_pid_approach_soft_cap(-0.1f, 1.0f) == 0.0f);
    CHECK(pb_heater_pid_approach_soft_cap(0.0f, 1.0f) == 0.0f);

    // Far from target: full power regardless of climb rate.
    CHECK(pb_heater_pid_approach_soft_cap(PB_HEATER_PID_APPROACH_BAND_C, 5.0f) == 1.0f);
    CHECK(pb_heater_pid_approach_soft_cap(10.0f, 5.0f) == 1.0f);

    // THE REGRESSION FIX: within the band but SETTLED (rate at/below the gate) the
    // cap releases to full authority, so the integrator can hold whatever steady
    // duty the enclosure needs and the chamber reaches setpoint.
    CHECK(pb_heater_pid_approach_soft_cap(0.5f, 0.0f) == 1.0f);
    CHECK(pb_heater_pid_approach_soft_cap(0.5f, PB_HEATER_PID_APPROACH_RATE_C_PER_S) == 1.0f);
    CHECK(pb_heater_pid_approach_soft_cap(1.9f, 0.01f) == 1.0f);

    // Within the band AND sprinting up: damped between FLOOR..1.0, deeper nearer
    // target. This is the only regime that shapes duty.
    float cap_near = pb_heater_pid_approach_soft_cap(0.3f, 1.0f);
    float cap_far  = pb_heater_pid_approach_soft_cap(2.7f, 1.0f);
    CHECK(cap_near >= PB_HEATER_PID_APPROACH_FLOOR - 1e-6f && cap_near < 1.0f);
    CHECK(cap_far  > cap_near && cap_far <= 1.0f);   // eases off toward the band edge
    // Deepest damping approaches the floor right at the setpoint.
    CHECK(pb_heater_pid_approach_soft_cap(0.001f, 1.0f)
          <= PB_HEATER_PID_APPROACH_FLOOR + 1e-3f);
}

static void test_ssr_window_and_dwell(void)
{
    const int64_t start = 1000000;
    // 30 % of the 30 s window = 9 s ON.
    pb_heater_pid_state_t state = {0};
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 8999999));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 9000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 29999999));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 30000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.0f, start + 30000001));

    // Minimum-dwell squelch: a sub-2 s ON segment (0.05 * 30 s = 1.5 s) never opens.
    pb_heater_pid_state_t low = {0};
    CHECK(!pb_heater_pid_window_on(&low, 0.05f, start));
    CHECK(!pb_heater_pid_window_on(&low, 0.05f, start + 500000));
    // A sub-2 s OFF remainder (0.97 * 30 s = 29.1 s ON, 0.9 s OFF) holds fully ON.
    pb_heater_pid_state_t high = {0};
    CHECK(pb_heater_pid_window_on(&high, 0.97f, start));
    CHECK(pb_heater_pid_window_on(&high, 0.97f, start + 29500000));
}

static void test_hold_reaches_full_authority(void)
{
    // THE REGRESSION FIX, end to end. A steady 1 C error (measurement held, so the
    // rise rate settles to zero) must NOT be clamped near target: the soft cap
    // releases and the integrator is free to wind to whatever the losses demand.
    // Pre-fix this was pinned at the 0.40 ceiling, parking the chamber ~2 C low.
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;

    CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty)); // prime history
    for (int i = 0; i < 4000; ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));
        CHECK(duty <= 1.000001f);
    }
    CHECK(state.last_approach_cap == 1.0f);  // settled hold → no shaping
    CHECK(duty > 0.99f);                       // free to command full duty at 1 C error

    // Far from target the full 0..1 range is likewise available and converges.
    pb_heater_pid_reset(&state);
    CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    for (int i = 0; i < 4000; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    CHECK(state.last_approach_cap == 1.0f);
    CHECK(duty > 0.99f);
}

static void test_fast_approach_is_damped(void)
{
    // While the chamber sprints up through the band the soft cap damps duty below
    // full to limit overshoot, and the damping deepens as the target nears.
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;

    CHECK(pb_heater_pid_step(&state, 60.0f, 57.0f, true, &duty));   // prime history (error 3)
    // +0.5 C/step = 1.0 C/s, well above the rate gate, and now inside the band.
    CHECK(pb_heater_pid_step(&state, 60.0f, 57.5f, true, &duty));   // error 2.5
    float cap_far = state.last_approach_cap;
    CHECK(cap_far < 1.0f && cap_far >= PB_HEATER_PID_APPROACH_FLOOR - 1e-6f);
    CHECK(duty <= cap_far + 1e-4f);                                  // duty respects the cap

    CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));   // error 1, still rising fast
    float cap_near = state.last_approach_cap;
    CHECK(cap_near < cap_far);                                       // damping deepens toward target
    CHECK(duty <= cap_near + 1e-4f);

    // Then it stops rising (rate collapses to zero): once the EMA-filtered rate
    // decays below the gate, the cap must release so the hold is not clamped.
    for (int i = 0; i < 30; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));
    CHECK(state.last_approach_cap == 1.0f);
}

static void test_rate_filter_rejects_quantization_spikes(void)
{
    // The chamber NTC reports in ~0.1 C steps. A single upward tick makes the raw
    // one-sample rate spike to ~0.2 C/s, but the EMA filter must keep the gate from
    // tripping on an isolated tick amid an otherwise flat hold — otherwise the cap
    // would flicker through the whole hold (the pre-filter behaviour).
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;

    // Settle a flat hold 1 C below target: filtered rate ~0, cap released.
    CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));
    for (int i = 0; i < 40; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));
    CHECK(state.last_approach_cap == 1.0f);

    // One isolated +0.1 C quantization tick (raw rate 0.2 C/s) must NOT trip the gate.
    CHECK(pb_heater_pid_step(&state, 60.0f, 59.1f, true, &duty));
    CHECK(state.rate_filt_c_per_s < PB_HEATER_PID_APPROACH_RATE_C_PER_S);
    CHECK(state.last_approach_cap == 1.0f);

    // A SUSTAINED climb of the same per-step size, however, does build the filtered
    // rate above the gate and engages the cap.
    pb_heater_pid_reset(&state);
    float m = 57.0f;
    CHECK(pb_heater_pid_step(&state, 60.0f, m, true, &duty));
    bool engaged = false;
    for (int i = 0; i < 20 && m < 59.9f; ++i) {
        m += 0.1f;  // +0.2 C/s every step, sustained
        CHECK(pb_heater_pid_step(&state, 60.0f, m, true, &duty));
        if (state.last_approach_cap < 1.0f) engaged = true;
    }
    CHECK(engaged);
}

static void test_safety_inhibition_holds_integral(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;
    for (int i = 0; i < 80; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 55.0f, true, &duty));

    float held_integral = state.controller.integral;
    CHECK(held_integral > 0.0f);

    // Enter the local chamber foldback at 72 C. The governor remains cut at
    // the 67 C boundary and releases only after crossing below it.
    bool local_cut = pb_heater_local_foldback_cut(true, 72.0f, false);
    CHECK(local_cut);
    CHECK(pb_heater_local_foldback_cut(true, 67.0f, local_cut));

    // This is the path used while either local foldback governor forces the
    // heater off: I is pinned across multiple samples, but process-variable
    // history continues to follow every measurement.
    const float held_measurements[] = {54.0f, 53.0f, 54.0f};
    for (size_t i = 0; i < sizeof(held_measurements) / sizeof(held_measurements[0]); ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, held_measurements[i], false, &duty));
        CHECK_NEAR(state.controller.integral, held_integral, 0.000001f);
        CHECK(state.controller.prev_measurement == held_measurements[i]);
    }

    CHECK(duty > 0.0f); // controller demand can exist behind the governor
    bool drive = !local_cut && pb_heater_pid_window_on(&state, duty, 1000000);
    CHECK(!drive);      // local thermal authority dominates PID demand

    local_cut = pb_heater_local_foldback_cut(true, 66.9f, local_cut);
    CHECK(!local_cut);
    CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    CHECK(state.controller.integral > held_integral);
    CHECK(state.controller.integral - held_integral < 0.0031f);
    CHECK(duty <= 1.0f);
}

static void test_invalid_input_and_corrupted_state_fail_safe(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 1.0f;

    CHECK(!pb_heater_pid_step(&state, 60.0f, NAN, true, &duty));
    CHECK(duty == 0.0f);

    state.controller.integral = NAN;
    duty = 1.0f;
    CHECK(!pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    CHECK(duty == 0.0f);
    CHECK(state.controller.integral == 0.0f);
    CHECK(!state.controller.initialized);
}

static void test_source_transition_reset(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;
    CHECK(pb_heater_pid_set_source(&state, false));
    CHECK(state.source_known);
    CHECK(!state.using_external);
    CHECK(pb_heater_pid_step(&state, 55.0f, 50.0f, true, &duty));
    CHECK(state.controller.initialized);
    CHECK(pb_heater_pid_window_on(&state, duty, 1000000));

    CHECK(!pb_heater_pid_set_source(&state, false));
    CHECK(state.controller.initialized); // same source preserves history

    // Switching between the local NTC and Bambu telemetry discards history
    // associated with the physically different process variable.
    CHECK(pb_heater_pid_set_source(&state, true));
    CHECK(!state.controller.initialized);
    CHECK(state.controller.integral == 0.0f);
    CHECK(state.window_start_us == 0);
    CHECK(!state.window_initialized);
    CHECK(state.source_known);
    CHECK(state.using_external);
}

int main(void)
{
    test_known_gains_and_heater_policy();
    test_process_variable_selection();
    test_approach_soft_cap();
    test_ssr_window_and_dwell();
    test_hold_reaches_full_authority();
    test_fast_approach_is_damped();
    test_rate_filter_rejects_quantization_spikes();
    test_safety_inhibition_holds_integral();
    test_invalid_input_and_corrupted_state_fail_safe();
    test_source_transition_reset();
    puts("pb_heater dc_pid host checks: PASS");
    return 0;
}
