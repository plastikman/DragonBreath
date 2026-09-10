# DragonBreath — feature set

Current as of **v1.1.16**. Open ESP-IDF firmware for the BIGTREETECH Panda Breath
(ESP32-C3) chamber heater with a selectable control source (Klipper/Moonraker, Home
Assistant, Bambu LAN, or PrusaLink) + local web control.

See [`OEM_PARITY.md`](OEM_PARITY.md) for the explicit implemented/planned/
intentionally-changed feature matrix.

## Control modes

The device runs an authoritative on-device state machine (`pb_policy`) — the
single owner of mode/target/lease state. It boots **OFF** and never auto-resumes
heat after a reboot.

| Mode | What it does | How it's triggered |
|---|---|---|
| **Off** | Heater off. | Boot default; `off` command (always accepted). |
| **Manual / Power-On** | Holds the chamber at a set target. Remote sessions take a device-issued lease and must heartbeat to stay alive. | `power_on` command (web "Manual heat", or Klipper `M141`/`M191`/`SET_HEATER_TEMPERATURE`). |
| **Automatic** | For filament-aware Klipper and Bambu sources, follows the active print's configured filament-zone target. For bed-only PrusaLink, heats to the configured target when the bed setpoint reaches its threshold. Waits with heat off if the source or required profile data is unavailable. | `auto` command / dashboard Auto control / front-panel Auto button. |
| **Filament drying** | Holds the chamber at a target for a bounded duration (1–12 h), then auto-off. Material presets pre-fill target + duration. | `drying_start` / web "Filament drying". |
| **Fan-only filtration** | Runs the chamber blower with **no heat** to filter/circulate air. Two paths: (a) the automatic **standing band** — the blower runs alone whenever `filter_auto` is enabled (**off by default**) + the source is connected + the bed **setpoint** reaches `filter_temp` (default 30 °C), independent of mode (even while idle); (b) a mode-independent manual toggle. Enabling manual filtration is idle-only (rejected while heating/cooling); turning it off always works. | `filter` command / web "Filtration" button; standing band via `filter_temp`/`filter_auto`. |

Commands are **revision-aware** (a stale writer can't clobber newer state) and
**idempotent** (request-ID replay cache); `off`/`drying_stop`/`filter` are always
accepted and never cached.

**Control source (choose one).** The device binds to exactly one printer/controller
at a time, selected on `/setup`: **Klipper/Moonraker** (default, hardware-validated),
**Klipper MQTT** (for managed installs that cannot add a Klippy extra),
**Home Assistant** (native MQTT Discovery — climate entity + chamber/element sensors;
validated against a live HA instance), **Bambu LAN** (read-only, follows the
loaded filament's zone profile; validated on a real printer), or **PrusaLink**
(bed-follow). They are mutually exclusive and reported as
`environment.control_source` in the state API. The heater safety model is
identical and source-independent.

For Klipper users, AUTO and slicer-issued `M141`/`M191` are alternative workflows.
Positive targets from those commands request a manual POWER_ON session and replace AUTO. OrcaSlicer also
emits a blocking `M191` before Machine start G-code when chamber control is enabled,
before the normal bed command. See [`USING_THE_HEATER.md`](USING_THE_HEATER.md) for
the two supported workflows and a bed-plus-chamber preheat macro.

AUTO and filament drying are implemented and validated end-to-end on hardware;
see [`OEM_PARITY.md`](OEM_PARITY.md).

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
- **Filtration band** — `filter_temp` (default 30 °C, 20–60 °C) + `filter_auto`
  (on/off, **default off** — opt-in) control the fan-only filtration band. It is a
  **standing** band (stock-shaped): once enabled, whenever Moonraker is connected the
  blower runs alone once the print's bed **setpoint** reaches `filter_temp` —
  independent of mode, so it filters even while idle. AUTO heat engagement is
  separate and uses either the active filament zone or, for a bed-follow source,
  its configured bed threshold. (Off-by-default diverges from stock; see
  [`OEM_PARITY.md`](OEM_PARITY.md).)

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

- `GET /api/v2/info` · `state` · `health` · `logs` · `calibration` — identity,
  authoritative snapshot, diagnostics, event ring, calibration (no side effects).
- `GET /api/v2/events` — Server-Sent Events push (state transitions + telemetry);
  replaces polling.
- `GET /api/v2/console` — **auth-gated** `text/plain` snapshot of the firmware
  `ESP_LOGx` ring (backs the `/console` page; the one read that needs the header
  since raw logs aren't world-readable).
- `POST /api/v2/command` · `heartbeat` — auth-gated, revision-aware control +
  exact-lease heartbeats.
- `GET`/`POST /settings` · `POST /api/v2/calibration` · `token` · `restart` ·
  `factory-reset` · `boot-inactive` — runtime safety settings, sensor calibration,
  control token, and maintenance (restart / factory reset / boot the inactive OTA slot).
- `POST /update` — authenticated app-image OTA (accepts a DragonBreath **or** a stock
  `panda_breath` image, for revert; refused while heating).

**Security:** every mutating route requires an `X-DragonBreath-Auth` header
(CSRF hardening for a trusted LAN; optional NVS control token for real per-client
auth). CORS is never enabled; reads never energize the heater or feed the
watchdog.

## Web UI (served by the device)

- **Live dashboard** — SSE-driven status (chamber/element temps + trend chart,
  mode, target, fan + reason, controller/link) with Manual / Automatic / Dry /
  Filtration controls and a Settings screen. Responsive and **light/dark themed**:
  full layout on desktop at any width; stacks vertically on touch devices.
- **Setup / provisioning** (`/setup`) — Wi-Fi + the control-source selector
  (Klipper/Moonraker, Klipper MQTT, Bambu, HA and their per-source fields); also the AP captive-portal root when
  unprovisioned. **OTA** (`/fw`) streams the image with a live %, then returns to the
  dashboard once the device reboots. Both match the dashboard theme (light/dark).
- **Klipper-MQTT config generator** (`/km-config`) — emits matching
  `moonraker.conf`, `printer.cfg`, and least-privilege Mosquitto ACL blocks from the
  saved setup values, while never echoing the stored broker password.
- **`/diag`** — read-only live telemetry (chamber/PTC temps, SSR output, mode, fault,
  running element-temp peak) over SSE, with a trend chart and a client-side CSV
  download. Zero device RAM.
- **`/console`** — terminal-style viewer of the firmware `ESP_LOGx` ring (boot log
  included) with auto-refresh and `.txt` download; useful on no-USB hardware where the
  serial console is otherwise unreachable. Backed by auth-gated `GET /api/v2/console`.
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

For AUTO mode, DragonBreath reads print/material state from the **control source**
selected on `/setup`. Klipper/Moonraker and Bambu are filament-aware; PrusaLink is
bed-follow because it does not report filament type. Home Assistant and Klipper-MQTT
drive target/mode commands instead of supplying AUTO environment data. None of
these require a vendor cloud (Bambu uses the printer's on-device LAN broker).

## Platform / release

- ESP32-C3-MINI-1; consumes Wi-Fi, captive provisioning/recovery, UI, Moonraker,
  MQTT transport, and logging components from a pinned
  [`dragon-core`](https://github.com/justinh-rahb/dragon-core) revision. OpenVent
  lineage and MIT provenance are recorded in VENDORING.md.
- **No-USB install/revert** on the stock partition layout — installs and reverts
  through the stock firmware's own OTA updater; **dual-slot OTA with rollback** (bad
  image reverts on next boot), plus a "boot inactive slot" one-click revert to stock.
- **Reproducible CI releases** — tag `v*` → the **app image** (`dragonbreath-<ver>.bin`),
  `manifest.json` (source SHA, ESP-IDF version, per-artifact SHA-256), and
  `SHA256SUMS.txt`. (App-only since v1.0.0; the full-image/USB machinery is kept in the
  repo for recovery only.)
- **CI static analysis + tests** — cppcheck over `components/`+`main/` and
  `-Wall -Wextra -Werror` on every first-party component, plus host/simulation
  tests (heater safety-trip ladder, NTC classify, policy state machine incl. the
  AUTO filtration band, persistent fault-latch) and the dev-board HIL build.
- Hardware reverse-engineered + validated on a **V1.0.1** board; the **V1.0**
  board (33 kΩ Rref) is confirmed in the field. Verify the pinout on other
  revisions before flashing.
