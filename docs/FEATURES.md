# DragonBreath — feature set

Current as of **v0.7.2**. Open ESP-IDF firmware for the BIGTREETECH Panda Breath
(ESP32-C3) chamber heater with Moonraker/Klipper + local web control.

See [`OEM_PARITY.md`](OEM_PARITY.md) for the explicit implemented/planned/
intentionally-changed feature matrix.

## Control modes

The device runs an authoritative on-device state machine (`pb_policy`) — the
single owner of mode/target/lease state. It boots **OFF** and never auto-resumes
heat after a reboot.

| Mode | What it does | How it's triggered |
|---|---|---|
| **Off** | Heater off. | Boot default; `off` command (always accepted). |
| **Manual / Power-On** | Holds the chamber at a set target. Remote sessions take a device-issued lease and must heartbeat to stay alive. | `power_on` command (web "Manual heat", or Klipper `M141`/`SET_HEATER_TEMPERATURE`). |
| **Automatic (follow bed)** | Watches the printer's bed temperature (via Moonraker) and heats the chamber to the target whenever the bed is at/above a threshold; disengages below threshold − 3 °C. Autonomous (no host heartbeat), requires the Moonraker link. | `auto` command / web "Follow printer bed". |
| **Filament drying** | Holds the chamber at a target for a bounded duration (1–12 h), then auto-off. Material presets pre-fill target + duration. | `drying_start` / web "Filament drying". |
| **Fan-only filtration** | Runs the chamber blower with **no heat** to filter/circulate air. Two paths: (a) automatic in AUTO — the blower runs alone once the bed reaches `filter_temp` (default 30 °C), before the heater engages; (b) a mode-independent manual toggle. Enabling manual filtration is idle-only (rejected while heating/cooling); turning it off always works. | `filter` command / web "Filtration" button; AUTO band via `filter_temp`/`filter_auto`. |

Commands are **revision-aware** (a stale writer can't clobber newer state) and
**idempotent** (request-ID replay cache); `off`/`drying_stop`/`filter` are always
accepted and never cached.

AUTO and filament drying are implemented in the policy/API; drying is validated
end-to-end on hardware. The AUTO dashboard control is still tracked as partial
until its user-facing feedback is fully validated; see
[`OEM_PARITY.md`](OEM_PARITY.md).

## Safety

Defense-in-depth — see [`SAFETY.md`](SAFETY.md) for the full model.

- **Hardware backstops (independent of firmware):** a bonded thermal cutoff in
  the PTC mains lead + PTC self-limiting physics bound the worst case even with a
  welded SSR or a firmware bug.
- **Firmware soft cutoffs:** 105 °C PTC over-temp and 85 °C chamber over-temp
  (both **fixed, non-configurable**); sensor-fault fail-closed (a bad thermistor
  latches the heater off).
- **Element foldback limiter:** below the 105 °C cutoff, the SSR is cut with
  hysteresis (per-Rref: 82 kΩ → 102 °C / 33 kΩ → 99 °C, or a user `fb_cut`
  override 90–104 °C) so a hot element holds under the hard cutoff instead of
  tripping it. Can only remove power; never exceeds 104 °C; the 105 °C cutoff is
  unaffected.
- **Per-Rref board detection:** the thermistor reference resistor (82 kΩ V1.0.1 /
  33 kΩ V1.0) is read from a strap at boot with a dual-pull check that fails safe
  to the conservative 33 kΩ if the pin floats; exposed as `rref_kohm` at `/info`.
- **Fan-only filtration never heats:** the filtration blower drives only the fan,
  never the SSR, and manual enable is idle-only (rejected while heating/cooling).
- **Comms-loss watchdog:** if the controlling client goes silent while heating,
  the heater latches off. Runtime-configurable within **10 s – 5 min** (never
  disabled or extended past 5 min).
- **Boot-OFF / no auto-resume**, single-writer SSR, latched faults require an
  explicit clear, and a permanent inhibit if the control-loop watchdog can't be
  armed (reboot-only).

## Configurable settings (persisted to NVS)

- **Max-target ceiling** — default 70 °C, hard-capped at 70 °C. No API/UI path
  can command heat above it.
- **Comms-watchdog timeout** — default 5 min, clamped to 10 s – 5 min.
- **Cooldown-fan release temp** (`cool_release`) — default 40 °C, range 30–65 °C;
  the residual-heat purge releases here (engages one 3 °C band above it).
- **Element-foldback cut** (`fb_cut`) — default auto (per-Rref); override 90–104 °C.
  Advanced/experts-only — see `tools/diag.py`. Never exceeds 104 °C.
- **AUTO filtration band** — `filter_temp` (default 30 °C, 20–60 °C) + `filter_auto`
  (on/off) control the fan-only filtration band in AUTO mode.

Exposed via `GET`/`POST /settings` and the web UI's Settings cards. The fixed
over-temp cutoffs are not settable.

## Status LEDs

Four front-panel LEDs, matching the stock panel (direct active-high GPIO):

- The driver supports OFF, solid, fast/slow blink, and pulse-code patterns on
  all four mapped outputs.
- All four are driven from the authoritative policy snapshot, so the panel alone
  tells you the active mode:
  - **Power** (GPIO21, release builds) is the device-health light — solid
    whenever the device is up, blinking on a latched fault.
  - **On** (GPIO5) is solid in manual POWER_ON.
  - **Auto** (GPIO6) is solid while AUTO is engaged and slow-blinks while AUTO is
    armed but waiting (no Moonraker link, or the bed is below the threshold).
  - **Dry** (GPIO4) is solid while drying.
- A fault forces the mode OFF, so the mode LEDs go dark and Power blinks.
- GPIO21 is also console TX. Development builds leave it as serial output;
  release builds enable `CONFIG_PB_POWER_LED` and use it for the Power LED.

## Front-panel buttons

The four active-low inputs (Power GPIO9, Auto GPIO8, On GPIO10, Dry GPIO2) are
polled at 10 ms with 20 ms debounce and short/long-press detection (`pb_buttons`):

- A **short press** toggles that button's labeled mode, arming it from the
  remembered parameters (last-used target/threshold/hours); pressing it again, or
  pressing Power, returns to OFF.
- A **2 s long-press** on any button latches a **panic-off** — heater off, mode
  OFF, remote lease invalidated. It is not a safety-rated emergency stop (see
  [`SAFETY.md`](SAFETY.md)); the hardware over-temp backstops remain the
  emergency layer.
- A **long-press on Power while faulted** attempts a fault clear instead, which
  only holds if the underlying condition has recovered.

Every action is attributed to the panel (source `button`) and appears in the
dashboard and Klipper. A button held at power-on is ignored until released, since
Power/Auto/Dry are ESP32-C3 strapping pins — **don't hold a button while the
board boots**. The real-Panda functional checklist and the scripted devboard-HIL
button scenario both passed on physical hardware (2026-07-24).

## Fan

AC blower switched by a TRIAC held **on/off** (never phase-angle PWM'd), synced
to the mains zero-cross detector. Airflow follows the heater; **post-print
cooldown** keeps the blower running after a heating session until the chamber
falls below the configurable release temp (`cool_release`, default 40 °C), gated
on a heat-this-session flag so it never auto-starts on temperature alone. The
blower also serves **fan-only filtration** (heater untouched) — automatic in AUTO
once the bed reaches `filter_temp`, and via the manual `filter` command / dashboard
toggle. Fault airflow is always forced on regardless.

## Control API (port 80)

Versioned JSON API — full contract in [`api-v2.md`](api-v2.md):

- `GET /api/v2/info` · `state` · `health` — identity, authoritative snapshot,
  diagnostics (no side effects).
- `GET /api/v2/events` — Server-Sent Events push (state transitions + telemetry);
  replaces polling.
- `POST /api/v2/command` · `heartbeat` — auth-gated, revision-aware control +
  exact-lease heartbeats.
- `GET`/`POST /settings` — runtime safety settings.
- `POST /update` — authenticated app-image OTA (rejects foreign images; refused
  while heating).

**Security:** every mutating route requires an `X-DragonBreath-Auth` header
(CSRF hardening for a trusted LAN; optional NVS control token for real per-client
auth). CORS is never enabled; reads never energize the heater or feed the
watchdog.

## Web UI (served by the device)

- **Live dashboard** — SSE-driven status (chamber/element temps + trend chart,
  mode, target, fan + reason, controller/link) with Manual / Automatic / Dry /
  Filtration controls and a Settings screen. Responsive and **light/dark themed**:
  full layout on desktop at any width; stacks vertically on touch devices.
- **Captive provisioning** (`/setup`, Wi-Fi + Moonraker in AP mode) and **OTA**
  (`/fw`) pages match the dashboard theme (light/dark). The OTA page streams the
  image with a live %, then returns to the dashboard once the device reboots.
- mDNS: reachable at **`dragonbreath.local`**.

## Klipper / Moonraker integration

[`dragonbreath-klipper`](https://github.com/plastikman/dragonbreath-klipper) is
the host-side helper (Klipper `extras`): a `[heater_generic dragonbreath]` exposes
the chamber as a heater for `M141` / `M191` and Fluidd, and a
`[output_pin dragonbreath_filter]` exposes the fan-only filtration blower as an
on/off toggle (binary — the blower can't modulate). Speaks API v2 (SSE +
exact-lease heartbeats, reactor-safe). Deploy lockstep with the firmware.

The dashboard can also be embedded in the Fluidd/Mainsail printer view via a
Moonraker `[webcam]` `iframe` — see the DragonBreath README.

DragonBreath also reads printer/bed state directly from Moonraker for AUTO mode.
The stock firmware can likewise obtain bed temperature from Moonraker in
Klipper mode, or from a Bambu printer over Bambu MQTT. Stock v1.0.4 additionally
exposes Panda Breath state/control through Home Assistant MQTT. DragonBreath
intentionally implements the Klipper/Moonraker path only; none of these stock
paths require a vendor cloud.

## Platform / release

- ESP32-C3-MINI-1; shares the [OpenVent](https://github.com/justinh-rahb/OpenVent)
  core (Wi-Fi, captive portal, Moonraker client), vendored locally (see VENDORING.md).
- **OTA with rollback** (bad image reverts on next boot).
- **Reproducible CI releases** — tag `v*` → factory image, OTA image, install
  bundle, `manifest.json` (source SHA, ESP-IDF version, per-artifact SHA-256),
  and `SHA256SUMS.txt`.
- **CI static analysis + tests** — cppcheck over `components/`+`main/` and
  `-Wall -Wextra -Werror` on every first-party component, plus host/simulation
  tests (heater safety-trip ladder, NTC classify, policy state machine incl. the
  AUTO filtration band, persistent fault-latch) and the dev-board HIL build.
- Hardware reverse-engineered + validated on a **V1.0.1** board; the **V1.0**
  board (33 kΩ Rref) is confirmed in the field. Verify the pinout on other
  revisions before flashing.
