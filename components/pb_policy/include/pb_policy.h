// SPDX-License-Identifier: MIT
// pb_policy — the sole owner of DragonBreath control state.
//
// Network transports, the dashboard, Moonraker integration, and physical
// buttons submit commands here.  They never write pb_heater or pb_fan directly.
// The policy owns mode transitions, targets, controller leases, deadlines, and
// the canonical snapshot consumed by API v2.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pb_ntc.h"
#include "pb_buttons.h"

#define PB_POLICY_REVISION_ANY UINT32_MAX
#define PB_POLICY_LEASE_ID_LEN 32
#define PB_POLICY_OWNER_LEN    31

// Remote POWER_ON leases follow pb_heater's runtime-configurable comms deadman,
// so the advertised lease runway and the hardware watchdog cannot disagree.
#define PB_POLICY_LOCAL_POWER_MAX_MS  (12U * 60U * 60U * 1000U)

typedef enum {
    PB_MODE_OFF = 0,
    PB_MODE_POWER_ON,
    PB_MODE_AUTO,
    PB_MODE_DRYING,
} pb_mode_t;

typedef enum {
    PB_SOURCE_BOOT = 0,
    PB_SOURCE_WEB,
    PB_SOURCE_KLIPPER,
    PB_SOURCE_BUTTON,
    PB_SOURCE_SAFETY,
    PB_SOURCE_WATCHDOG,
} pb_source_t;

typedef enum {
    PB_POLICY_OK = 0,
    PB_POLICY_INVALID,
    PB_POLICY_REVISION_CONFLICT,
    PB_POLICY_FAULT_LATCHED,
    PB_POLICY_INHIBITED,
    PB_POLICY_STALE_LEASE,
    PB_POLICY_PERSIST_FAILED,   // action succeeded in RAM path but NVS persist failed -> HTTP 500
    PB_POLICY_BUSY,             // rejected: heater is heating / cooling down (e.g. manual fan is idle-only)
} pb_policy_result_t;

typedef struct {
    char id[PB_POLICY_LEASE_ID_LEN + 1];
} pb_policy_lease_t;

// Remembered mode parameters: the values a mode is re-armed with when the
// caller does not supply its own (front-panel buttons) and the values the UI
// pre-fills from.  These are the ONLY policy state that survives a reboot --
// never a mode, target, deadline, heating flag, or lease.  See
// pb_policy_load_params(). Target parameters are clamped to the heater's live
// runtime-configured maximum when loaded.
typedef struct {
    float manual_target_c;        // last accepted POWER_ON target
    float auto_target_c;          // last accepted AUTO target
    float auto_bed_threshold_c;   // last accepted AUTO bed threshold
    float dry_target_c;           // last accepted drying target
    uint8_t dry_hours;            // last accepted drying duration
    float filter_temp_c;          // AUTO fan-only band: blower runs alone at bed>=this
    bool filter_auto_enable;      // enable the AUTO fan-only (filtration) band
} pb_policy_params_t;

typedef struct {
    uint32_t state_revision;
    pb_mode_t mode;
    pb_source_t source;

    float requested_target_c;
    float effective_target_c;
    bool heater_demand;
    bool heater_output;

    uint8_t requested_fan_percent;
    uint8_t effective_fan_percent;
    bool thermal_purge;

    float chamber_c;
    float ptc_c;
    pb_ntc_status_t chamber_status;
    pb_ntc_status_t ptc_status;

    bool moonraker_connected;
    float bed_c;
    bool auto_engaged;
    bool auto_filtering;          // AUTO fan-only band active (blower on, no heat)
    float auto_bed_threshold_c;
    pb_policy_params_t params;

    bool drying;
    uint32_t drying_remaining_s;

    bool lease_active;
    char lease_id[PB_POLICY_LEASE_ID_LEN + 1];
    char lease_owner[PB_POLICY_OWNER_LEN + 1];
    uint32_t lease_expires_ms;

    bool fault_latched;
    bool inhibited;
    char fault_reason[64];
} pb_policy_snapshot_t;

esp_err_t pb_policy_init(void);

// Load the remembered mode parameters from NVS (namespace app_nvs) and start the
// persistence worker.  MUST be called AFTER nvs_init() -- pb_policy_init() only
// installs conservative defaults, since NVS is not up that early.  Values are
// clamped on read, and loading NEVER changes the mode or arms a target: the
// device still boots OFF.
void pb_policy_load_params(void);

void pb_policy_get_params(pb_policy_params_t *out);

// Write any pending parameter change to NVS.  Returns true if a commit was
// attempted.  The persistence worker calls this in a loop; it is exposed so the
// host test can drive persistence synchronously without a scheduler.
bool pb_policy_persist_pending(void);

// Remote WEB/KLIPPER POWER_ON commands receive a device-issued lease.  A lease
// is RAM-only, changes on every accepted heat command, and is invalidated by
// OFF, another mode command, safety/watchdog action, or reboot.
pb_policy_result_t pb_policy_set_power_on(
    float target_c,
    pb_source_t source,
    const char *owner,
    uint32_t expected_revision,
    pb_policy_lease_t *lease_out);

pb_policy_result_t pb_policy_set_auto(
    float target_c,
    float bed_threshold_c,
    pb_source_t source,
    uint32_t expected_revision);

pb_policy_result_t pb_policy_start_drying(
    float target_c,
    uint8_t hours,
    pb_source_t source,
    uint32_t expected_revision);

// OFF is deliberately unconditional: stale state must never prevent a caller
// from making the device safer.
void pb_policy_set_mode_off(pb_source_t source);
void pb_policy_stop_drying(pb_source_t source);

// Manual filtration blower: run the blower fan-only (0 = off, 1..100 = on; the
// hardware fan is on/off, so any non-zero runs it). Heater is untouched.
// Independent of mode — not cleared by OFF, additive over any heat airflow — and
// safe (no heat path), so it is accepted regardless of revision. Cleared on reboot.
// ENABLE is idle-only: turning it ON (percent > 0) returns PB_POLICY_BUSY while the
// heater is heating or the cooldown purge is running, so a manual toggle can't
// disturb the heat cycle. Turning it OFF (percent == 0) is always allowed, so
// "Stop" can clear a lingering filtration request mid-cycle.
pb_policy_result_t pb_policy_set_fan(uint8_t percent, pb_source_t source);

// AUTO fan-only filtration band (persisted): when enabled, AUTO runs the blower
// alone once the printer bed reaches filter_temp_c, before the heater engages at
// the (higher) auto bed threshold — mirroring the stock Panda's filtration band.
// Fan-only, so it never adds heat. Persists across reboot (a config setting).
#define PB_POLICY_FILTER_TEMP_MIN_C  20.0f
#define PB_POLICY_FILTER_TEMP_MAX_C  60.0f
pb_policy_result_t pb_policy_set_filter_config(float filter_temp_c, bool enable);
float pb_policy_get_filter_temp_c(void);
bool  pb_policy_get_filter_auto_enable(void);

// Update printer environment used by AUTO.  This is observer input, not a
// control command, and therefore never creates or refreshes a control lease.
void pb_policy_set_env(float bed_c, bool moonraker_connected);

// Refresh exactly the active lease.  A stale/superseded lease cannot keep heat
// alive.
pb_policy_result_t pb_policy_heartbeat(const pb_policy_lease_t *lease);

// Clearing a fault changes authoritative state and therefore requires the
// caller's observed revision. Unlike OFF, stale state must not clear a newer
// fault or safety transition.
pb_policy_result_t pb_policy_clear_fault(
    pb_source_t source,
    uint32_t expected_revision);

// Periodic control tick (call at ~1-2 Hz).  Computes the effective target,
// applies the heater/fan outputs, enforces deadlines, and synchronizes safety
// trips back into the authoritative state.
void pb_policy_tick(void);

// Pure, session-gated residual-heat purge decision (exposed for host testing).
// Returns whether the cooldown fan should run and updates *heated_this_session.
// It purges only after heat ran this session (never on temperature alone), with
// hysteresis around the user-configurable "cool down to" temperature `release_c`:
// engage at >= release_c + PB_PURGE_HYSTERESIS_C on either sensor, release only
// once BOTH are known below release_c.
bool pb_purge_decide(bool heat, bool *heated_this_session,
                     bool chamber_ok, float chamber_c,
                     bool ptc_ok, float ptc_c, bool prev_cooldown,
                     float release_c);

// Front-panel button handler.  Short presses toggle the button's labeled mode
// (re-arming from the remembered parameters); a 2 s long press latches a
// "panic-off", except a long press on Power while faulted attempts a fault
// clear.  Every button action is source=BUTTON, invalidates the remote lease,
// and bumps the revision -- so a physical action always wins over stale remote
// ownership.  Runs on the button task; must not be called holding any policy
// lock.
void pb_policy_on_button(pb_button_id_t id, pb_button_event_t ev);

// Latch heater + policy OFF from any task and attribute it to `source`.  Unlike
// the tick's generic safety sync, this stamps the transition itself (so a button
// panic-off reports source=BUTTON, not SAFETY) and invalidates the lease
// immediately.  The registered wake callback fires afterward so the control task
// drops the SSR without waiting for the next periodic tick.
void pb_policy_request_panic_off(pb_source_t source, const char *reason);

// Callback invoked (outside any policy lock) whenever an accepted control
// command or safety transition needs the control task to run a tick promptly.
// Keep it ISR-light: a single task notification. Optional; if unset, the change
// is picked up on the next periodic tick.
typedef void (*pb_policy_wake_fn)(void);
void pb_policy_set_wake_cb(pb_policy_wake_fn fn);

void pb_policy_get_snapshot(pb_policy_snapshot_t *out);
pb_mode_t pb_policy_get_mode(void);
const char *pb_policy_mode_str(pb_mode_t mode);
const char *pb_policy_source_str(pb_source_t source);
const char *pb_policy_result_str(pb_policy_result_t result);
