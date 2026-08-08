# MQTT-Klipper mode — implementation design (targets v1.0.5)

Status: **Design, pre-implementation.** Grounds [RFC #66](https://github.com/plastikman/DragonBreath/pull/66)
(`plans/mqtt-klipper-integration-mode.md`) in the *verified* Moonraker/Klipper wire
contract and this firmware's existing control-source architecture. No code yet — this
is the doc to review before Phase 2.

This is the portable, plugin-free Klipper integration for **locked/managed** setups
(user can edit `moonraker.conf` + `printer.cfg` + broker, but cannot install a
`klippy/extras/` module). The native `dragonbreath-klipper` extra remains the
preferred path everywhere it can be installed.

---

## 0. What the documentation research changed

Four doc sweeps (Moonraker MQTT core, `[sensor]`/`[power]`, Klipper macros + printer
API, reference integrations) corrected several RFC assumptions. The design below
uses the **verified** facts. The corrections that matter:

| # | RFC assumed | Verified reality | Impact |
|---|---|---|---|
| 1 | availability at `{instance}/mqtt/...` | **`{instance}/moonraker/status`**, retained `{"server":"online"\|"offline"}` (also the LWT) | our clean "Klipper stack up?" signal |
| 2 | split-status payload = raw scalar | **`{"eventtime":…,"value":…}` JSON, retained** | parser reads `.value`; retained ⇒ arming must ignore it (see §6) |
| 3 | (unstated) | combined `.../klipper/status` topic is **non-retained**; split topics **are** | we require `publish_split_status: True` |
| 4 | object name sanitized | **literal space** kept: `.../gcode_macro DRAGONBREATH/…` | topic filters/parser handle the space |
| 5 | (unstated) | API writes need duplicate protection via **`mqtt_timestamp`** or QoS 0/2 | optional writeback uses QoS 0; responses are ignored |
| 6 | M191 "normally blocking" | **M141/M191 don't exist natively in Klipper**; a macro cannot block on an external value; `TEMPERATURE_WAIT` needs a *hardware* Klipper sensor; **no MQTT-fed Klipper sensor exists** | M141 shim only; blocking M191 impossible by construction |
| 7 | (unstated) | `SET_GCODE_VARIABLE` is **in-memory only** (resets on FIRMWARE_RESTART) | device re-publishes desired-state + re-asserts `seq` on reconnect |
| 8 | (unstated) | Moonraker `[sensor]` is **invisible to Klipper macros** | dashboard temp and macro-visible temp are **two paths** (sensor + writeback) |
| 9 | (unstated) | keepalive fixed at 60 s; `state_timeout` (power) default 2 s | our own dead-man timer is the real safety clock, not MQTT keepalive |

Moonraker Jinja uses **single-brace expressions** `{ }` and `{% %}` statements (not
Klipper's `{{ }}`); `set_result()` takes exactly `(name, number)`; power
`state_response_template` must resolve to `on`/`off`/`discard`.

---

## 1. Core invariant: exactly one control source, ever

The firmware already enforces this structurally, and we keep it that way:

- A single NVS enum `ctl_src` (`pb_source`) is read once at boot. `app_main` runs one
  `switch(ctl_src)` that starts **exactly one** client; the per-tick loop polls that
  same single branch. `/setup` is one `<select>`. There is no path that runs two MQTT
  clients or two policy writers.
- MQTT-Klipper is a **new 5th value** in that same exclusive selector, so choosing it
  deselects Bambu/HA/native-Klipper automatically.
- Care-points (mechanical): add `PB_SRC_KLIPPER_MQTT`, renumber `PB_SRC_NONE`, keep
  every `<= PB_SRC_NONE` range clamp correct (`pb_source.c`, portal save).

### The two "Klipper" sources — disambiguation

There are now two Klipper integrations, and they are **mutually exclusive**:

- **Klipper (Moonraker)** — `PB_SRC_KLIPPER`, today's default. Device follows the bed
  setpoint over the Moonraker WebSocket and engages AUTO. Pairs with the native
  `dragonbreath-klipper` extra when installed.
- **Klipper (MQTT)** — `PB_SRC_KLIPPER_MQTT`, new. Device is *commanded* over MQTT
  (target/mode/arm) with the RFC's arming+heartbeat safety contract.

`/setup` labels them distinctly and the generated config bundle carries a "do **not**
also run the native extra" note, so the deployment-level duplicate (two things driving
the chamber) is prevented by documentation + the single-selector.

---

## 2. Firmware shape: `pb_klipper_mqtt` is a **controller** (HA-shaped)

Unlike Bambu (read-only bed-follow), MQTT-Klipper *receives a desired target and
arms heat*, so it mirrors **`pb_ha`**: it holds a `pb_policy` lease and heartbeats it,
rather than feeding the AUTO seam via `pb_policy_set_env`.

New component `components/pb_klipper_mqtt/` (CMake REQUIRES `nvs_flash mqtt esp_timer
pb_policy`, `+esp-tls` for mqtts):

```
pb_klipper_mqtt_start(void)                 // boot: connect, subscribe
pb_klipper_mqtt_tick(void)                  // per-tick: apply desired-state / lease
pb_klipper_mqtt_get_status(status_t *out)   // for /state JSON + dashboard
pb_klipper_mqtt_set_config / get_config / clear_config
```

Policy integration reuses existing seams — **no `pb_policy` signature change**:
`pb_policy_set_power_on(target, PB_SOURCE_WEB, "klipper-mqtt", …, &lease)` when armed
and fresh; `pb_policy_heartbeat(&lease)` on each fresh heartbeat; `pb_policy_set_mode_off`
when disarmed/timed-out. The **`pb_heater` comms dead-man** (`notify_link_alive`, tied
to lease TTL) is the hardware backstop that forces heat off if we stop heartbeating.

---

## 3. Topic map

`INST` = user-set `instance_name` (required; the hostname default is non-deterministic).
`DB` = device topic base (default `dragonbreath`; advanced-configurable).

**Device subscribes:**

| Topic | Source | Purpose |
|---|---|---|
| `INST/klipper/state/gcode_macro DRAGONBREATH/#` | Moonraker split-status (retained) | desired-state fields |
| `INST/klipper/state/gcode_macro DB_LINK/#` | Moonraker split-status (retained) | heartbeat counter |
| `INST/moonraker/status` | Moonraker (retained/LWT) | Klipper-stack availability |
| `DB/power/set` | Moonraker `[power]` command | master enable on/off |

**Device publishes:**

| Topic | Retain | Payload |
|---|---|---|
| `DB/telemetry` | no | versioned JSON telemetry (§4) |
| `DB/power/state` | **yes** | `on`/`off` (Moonraker inits w/o query; 2 s `state_timeout`) |
| `DB/status` | **yes** (LWT) | `online` / `offline` (device availability) |
| `INST/moonraker/api/request` | no | JSON-RPC `printer.gcode.script` writeback (§5) |

**All device-published *application* messages are non-retained** except `power/state`
and `status` (which are device-availability/init, never arming inputs).

---

## 4. Payload schemas (versioned)

Every device JSON payload carries `"v": 1` (schema version; bump on breaking change).

**`DB/telemetry`** (non-retained, ~2 s or on-change):
```json
{"v":1,"chamber_temperature":42.3,"element_temperature":80.1,"humidity":35.0,
 "mode":"heat","target":55.0,"armed":true,"seq_ack":7,"fault":""}
```
Consumed by Moonraker `[sensor type: mqtt]` for the dashboard (parses the temp fields
via `set_result`) and available to any observer; `seq_ack` closes the desired-state
loop (device echoes the `seq` it last applied).

**Desired-state** lives on **`gcode_macro DRAGONBREATH`** (lower-case variable names,
Klipper rule):
```
variable_seq: 0            # monotonic; bumped LAST after all fields written
variable_target: 0.0       # chamber °C
variable_mode: "off"       # "off" | "heat"
variable_fan: 0            # 0..100
variable_armed: 0          # 0 | 1  (explicit arm)
variable_purge_nonce: 0    # one-shot actions (increment, not bool)
# device-written back (for macro logic):
variable_temperature: -1.0
variable_humidity: -1.0
variable_fault: ""
```
Delivered as retained split-status: topic `.../gcode_macro DRAGONBREATH/seq` etc.,
payload `{"eventtime":…,"value":7}`. Device reads `.value`.

**Heartbeat** on **`gcode_macro DB_LINK`**: `variable_heartbeat` incremented every 5 s
by a self-rescheduling `[delayed_gcode]` (doc-idiomatic; body stays motion-free).

---

## 5. Desired-state → arming state machine (the safety core)

> **Invariant:** DragonBreath never energizes heat merely because it received desired
> state. It requires a fresh coherent **`seq` update observed live** *and* a
> **fresh heartbeat**.

Because split-status is **retained**, a reconnecting device is handed `armed=1`,
`target=55`, `seq=7`, and the last `heartbeat` value **immediately** — all potentially
stale. So arming gates on *liveness observed after connect*, not on received values:

State per (re)connect starts **DISARMED**, and:

1. **Connect snapshot** — the first value received for each field (target/mode/seq/
   armed/heartbeat) is recorded as the initial snapshot. An `armed=1` *in the snapshot*
   is not permission to arm; the snapshot `heartbeat` is **not** proof of liveness.
2. **Liveness** — becomes true only after we observe the `heartbeat` value **change**
   at least once post-connect, and stays true while the last change was `< 15 s` ago
   (3 × 5 s cadence). Loss of liveness ⇒ force heat off, latch `comms_lost`.
3. **Arm** — heat is driven only when **all** hold:
   - `seq` advanced beyond last-applied (a coherent new desired-state — `seq` is
     written last, so all other fields are consistent when it changes);
   - `mode == "heat"` and `armed == 1`;
   - liveness is currently true.
4. **Apply** — on a qualifying update: `pb_policy_set_power_on(target, …, &lease)`;
   publish `seq_ack = seq`. While armed+live, `pb_policy_heartbeat(&lease)` on each
   fresh heartbeat (period `< pb_heater` comms timeout).
5. **Disarm / recover** — `armed→0`, `mode→off`, liveness lost, Moonraker `offline`,
   or master-power off ⇒ `pb_policy_set_mode_off`. Recovery re-enters at DISARMED and
   requires a **new** live `seq` update (a retained `armed=1` after reconnect never
   re-arms on its own).

`variable_fan` and `purge_nonce` are reserved protocol fields in 1.0.5; they are parsed
and retained-safe but do not yet drive product actions. Thermal cutoffs, max-temp, the
`pb_heater` watchdog and fail-off all remain inside firmware and are unchanged — this
mode cannot weaken them.

---

## 6. Device→Klipper writeback (macro-visible temperature)

Moonraker `[sensor]` values are invisible to macros (§0 #8), so for macro logic the
current implementation can write chamber temperature back into a `DRAGONBREATH`
variable via the API bridge:
```json
{"jsonrpc":"2.0","method":"printer.gcode.script","id":<uniq>,
 "params":{"script":"SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=temperature VALUE=42.3"}}
```
The firmware publishes this request at QoS 0 every ~2 s and does not consume the API
response. QoS 0 follows Moonraker's duplicate-execution guidance without requiring an
`mqtt_timestamp`. Humidity and fault-variable writeback remain future protocol work.
This path is **optional** (`[sensor]` telemetry covers the dashboard); enable it only
when a customer's macros need the live chamber temperature.

---

## 7. M141 shim; M191 policy

- **`M141`** (set chamber temp, non-blocking) — shim macro writes `target` + bumps
  `seq`; that's the whole control input during a print (filament start-gcode). Klipper
  has no native M141, so we *provide* it (no `rename_existing`).
- **`M191`** (set + wait) — a correct blocking wait is **impossible** in this mode
  (§0 #6). **DECIDED:** default to a **non-blocking alias** — `M191` sets the target
  (like `M141`) and prints a one-line `action_respond_info` warning that it does *not*
  wait. Rationale: M191 behavior cannot affect safety (arming/heartbeat/watchdog govern
  heat regardless); a hard error would *abort* any print whose start-gcode emits M191,
  whereas the alias degrades gracefully (worst case: cooler first layers). The strict
  hard-error variant ships **commented-out directly below** in the generated
  `printer.cfg`, so a user who wants "refuse to print without a chamber wait" flips it
  by uncommenting — no firmware toggle, since this is Klipper-side config the user owns.
  The iHeater "`start_offset` early-release" trick does **not** save us — it still needs
  a real Klipper `temperature_sensor`, which MQTT mode doesn't provide.

  ```
  [gcode_macro M191]
  gcode:
    {action_respond_info("M191: MQTT mode sets target but does NOT block.")}
    M141 S{params.S|default(0)}
  # --- Strict alternative: refuse to print without a real chamber wait ---
  # [gcode_macro M191]
  # gcode:
  #   { action_raise_error("M191 unsupported in MQTT mode — use M141") }
  ```
- **Optional `printer.emergency_stop`** on a latching critical enclosure fault: keep
  **disabled by default** until its trigger policy is specified and tested.

---

## 8. `/setup` UX — single source of truth → generated config

Design principle: collect the *minimum* free-form inputs; **derive** every topic;
**emit** the exact Klipper-side config so the two ends can't drift.

**Fields (new `data-src` group, mirrors the Bambu/HA groups):**
- Broker `address`, `port` (1883/8883), `username`, `password`, `TLS` toggle.
- `instance_name` (**required** — topics are non-deterministic otherwise).
- `device topic base` (default `dragonbreath`; advanced/collapsible).
- (advanced) QoS.

**Setup validator** (RFC phase 3, made *generative*): refuses to enable the mode
without broker address **and** credentials; warns if `instance_name` is blank.

**Generated bundle** (rendered from the fields above, shown on `/setup` for copy-paste
and downloadable): `moonraker.conf` (`[mqtt]` with `publish_split_status: True` +
`status_objects` for both macros, `[sensor type: mqtt]` for the dashboard temp,
`[power type: mqtt]` for the master enable), `printer.cfg` (the `DRAGONBREATH` +
`DB_LINK` macros, the `DB_HEARTBEAT` delayed_gcode, `M141`/`M191` shims), and the
`mosquitto` ACL (least-privilege, per RFC — *not* presented as optional). The bundle
header states native-extra mutual exclusion.

---

## 9. Reference patterns adopted (from the integration survey)

- **`off_when_shutdown: True`** on the `[power]` device — the one free Klipper-side
  kill (heater off if Klippy dies), complementing our dead-man timer.
- **Relaxed `verify_heater`** guidance in docs (chamber heats slowly; hotend defaults
  false-trip) — though in MQTT mode the element is *ours*, not a Klipper heater, so
  this is advisory for any user who also mirrors a Klipper heater.
- **Non-motion heartbeat body** (`SET_GCODE_VARIABLE`/`action_respond_info` only).
- **`seq`-written-last** coherent-update discipline; **`purge_nonce`** for repeatable
  one-shots.
- **`discard` via `last_request`/`response_count`** in the power `state_response_template`
  only if we ever echo command+state (we publish state once per real change, so likely
  unneeded — noted to avoid toggle flap).

---

## 10. Decisions (locked — schema frozen)

1. **M191 behavior:** **non-blocking alias + warning** (default), strict hard-error
   variant shipped commented-out in the generated `printer.cfg` (§7). ✔ locked
2. **Device topic base:** **fixed `dragonbreath`, advanced-editable** in `/setup` (§8). ✔ locked
3. **Writeback (§6) on by default:** **off** — dashboard via `[sensor]` suffices;
   opt-in toggle for macro-driven setups. ✔ locked
4. **`emergency_stop` escalation:** **disabled by default** for 1.0.5, pending a
   specified+tested trigger policy. ✔ locked
5. **Expose DragonBreath as a Klipper `[heater_generic]`/`[temperature_sensor]`?**
   Out of scope for MQTT-only (needs a plugin/MCU); it's the only route to a real
   blocking M191 — noted as a future "native+" path, **not 1.0.5**.

---

## 11. Implementation phases (1.0.5 = phases 1–3; phase 4 gates the final tag)

1. **Schema freeze** — ✅ done (this doc; decisions locked §10).
2. **Firmware** — ✅ done. `pb_klipper_mqtt` (controller, HA-shaped) reusing the
   lease/policy + `pb_heater` watchdog seams; NVS-safe enum; `/setup` group +
   validator; `pb_source`/dashboard surface. Arming core host-tested (26 cases:
   retained-snapshot-doesn't-arm, arm-needs-new-seq+live-heartbeat, seq coherence,
   heartbeat-timeout → comms-lost, re-arm-after-recovery, prompt disarm, purge nonce).
   Builds clean; on-device smoke test of `/setup` + `/km-config` passed.
3. **Config artifacts** — ✅ generative: `GET /km-config` renders `moonraker.conf` +
   `printer.cfg` + Mosquitto ACL filled in from the saved settings (single source of
   truth), incl. the M141 shim + non-blocking-M191 alias. TODO: a short `docs/`
   page framing MQTT mode as the compatibility path (polish).
4. **Hardware validation on a real locked Klipper install** (Mainsail + Fluidd):
   dashboard readings, control, queue behavior mid-print, loss/recovery, and the proof
   that heat stays off until an explicit live re-arm. **PENDING — gates the `v1.0.5`
   tag.** (Bench has no locked-Klipper broker; needs a real setup.)
5. Docs polish; retain native module as recommended path.
