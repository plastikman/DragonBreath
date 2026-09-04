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

static void test_approach_and_ssr_window(void)
{
    CHECK(pb_heater_pid_approach_max_duty(-0.1f) == 0.0f);
    CHECK(pb_heater_pid_approach_max_duty(0.0f) == 0.0f);
    CHECK(pb_heater_pid_approach_max_duty(0.1f) == 0.40f);
    CHECK(pb_heater_pid_approach_max_duty(1.99f) == 0.40f);
    CHECK(pb_heater_pid_approach_max_duty(2.0f) == 0.70f);
    CHECK(pb_heater_pid_approach_max_duty(4.99f) == 0.70f);
    CHECK(pb_heater_pid_approach_max_duty(5.0f) == 1.0f);

    pb_heater_pid_state_t state = {0};
    const int64_t start = 1000000;
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 2999999));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 3000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 9999999));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 10000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.0f, start + 10000001));
}

static void test_approach_caps_prevent_integral_windup(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;

    // A fixed 1 C error remains under the 40% approach
    // ceiling for thousands of samples without storing demand behind that cap.
    for (int i = 0; i < 4000; ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, 59.0f, true, &duty));
        CHECK(duty <= 0.400001f);
    }
    float integral_at_40 = state.controller.integral;
    float raw_at_40 = PB_HEATER_PID_KP * 1.0f + integral_at_40;
    CHECK(integral_at_40 > 0.29f && integral_at_40 < 0.301f);
    CHECK(raw_at_40 <= 0.400001f);

    // Relaxing the active ceiling does not reveal a hidden integral jump.
    CHECK(pb_heater_pid_step(&state, 60.0f, 56.0f, true, &duty));
    CHECK_NEAR(state.controller.integral, integral_at_40, 0.000001f);
    CHECK(duty <= 0.700001f);

    pb_heater_pid_reset(&state);
    for (int i = 0; i < 4000; ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, 56.0f, true, &duty));
        CHECK(duty <= 0.700001f);
    }
    float raw_at_70 = PB_HEATER_PID_KP * 4.0f + state.controller.integral;
    CHECK(state.controller.integral > 0.29f && state.controller.integral < 0.301f);
    CHECK(raw_at_70 <= 0.700001f);

    // Outside the approach bands, the original full 0..1 controller range is
    // still available and converges against that unchanged ceiling.
    pb_heater_pid_reset(&state);
    for (int i = 0; i < 4000; ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
        CHECK(duty <= 1.000001f);
    }
    float raw_uncapped = PB_HEATER_PID_KP * 6.0f + state.controller.integral;
    CHECK(state.controller.integral > 0.39f && state.controller.integral < 0.401f);
    CHECK(raw_uncapped <= 1.000001f);
    CHECK(duty > 0.99f);
}

static void test_approach_cap_contraction_normalizes_stored_demand(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;

    // Accumulate meaningful integral in the full-output band without resetting
    // the controller between the approach bands the real chamber traverses.
    for (int i = 0; i < 4000; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    CHECK(state.controller.integral > 0.39f);
    CHECK(duty > 0.99f);

    // Tightening to 70% back-calculates I so stored P+I matches the new range.
    CHECK(pb_heater_pid_step(&state, 60.0f, 56.0f, true, &duty));
    float stored_at_70 = PB_HEATER_PID_KP * 4.0f + state.controller.integral;
    CHECK(stored_at_70 <= 0.700001f);
    CHECK(duty > 0.05f && duty <= 0.700001f);

    // Tightening again to 40% performs the same normalization with no reset and
    // no negative-output kick from the retained derivative history.
    CHECK(pb_heater_pid_step(&state, 60.0f, 58.5f, true, &duty));
    float stored_at_40 = PB_HEATER_PID_KP * 1.5f + state.controller.integral;
    CHECK(stored_at_40 <= 0.400001f);
    CHECK(duty > 0.05f && duty <= 0.400001f);

    for (int i = 0; i < 4000; ++i) {
        CHECK(pb_heater_pid_step(&state, 60.0f, 58.5f, true, &duty));
        float stored = PB_HEATER_PID_KP * 1.5f + state.controller.integral;
        CHECK(stored <= 0.400001f);
        CHECK(duty >= 0.0f && duty <= 0.400001f);
    }

    // Relaxing the cap reveals no stored-demand jump: only the next ordinary
    // integration increment is permitted.
    float integral_at_40 = state.controller.integral;
    CHECK(pb_heater_pid_step(&state, 60.0f, 56.0f, true, &duty));
    CHECK(state.controller.integral > integral_at_40);
    CHECK(state.controller.integral - integral_at_40 < 0.003f);
    CHECK(duty <= 0.700001f);
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
    test_approach_and_ssr_window();
    test_approach_caps_prevent_integral_windup();
    test_approach_cap_contraction_normalizes_stored_demand();
    test_safety_inhibition_holds_integral();
    test_invalid_input_and_corrupted_state_fail_safe();
    test_source_transition_reset();
    puts("pb_heater dc_pid host checks: PASS");
    return 0;
}
