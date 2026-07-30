# Changelog

All notable changes to the **DragonBreath** firmware are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are the
firmware release tags (`vX.Y.Z`). The release workflow pulls the matching section
below into the GitHub Release notes.

## [Unreleased]

## [0.8.0] - 2026-07-30

The pluggable-control-source + web-tools release. **Klipper (Moonraker) remains the
default and is unchanged**; everything below is additive and the heater safety model
is untouched and source-independent. Consolidates the 0.8.0-rc1…rc3 pre-releases.

### Added
- **Selectable control source — Klipper / Bambu / Home Assistant.** The device binds
  to exactly one printer/controller, chosen in `/setup` (mutually exclusive).
  - **Home Assistant (validated on hardware).** MQTT client with MQTT-Discovery — a
    climate entity + chamber/element temperature sensors auto-appear in HA — plus a
    retained state topic and command topics mapped to heater target/mode (holds a
    device lease and heartbeats it). Declares Celsius so HA converts °C↔°F correctly
    in both directions. Verified end-to-end (device + HA + Mosquitto).
  - **Bambu LAN — EXPERIMENTAL, not yet validated against a printer.** Read-only
    bed-follow over the printer's on-device LAN MQTT/TLS broker (`bblp` + LAN access
    code, subscribe `device/<serial>/report`, `pushall` on connect, scan
    `bed_temper`) feeding AUTO. Built from the OpenBambuAPI / ha-bambulab spec. Opt-in
    and safe by construction (the source only produces a bed temperature into the
    already-validated safety logic) — shipped for community validation. Select
    "Bambu" in `/setup` to test; please report results.
- **`/diag` diagnostics page.** Live SSE view of the `tools/diag.py` telemetry
  (chamber/element temps, SSR output, mode, fault, running element-temp peak) with a
  trend chart and a client-side CSV download. Read-only; **zero device RAM**.
- **`/console` firmware log page.** Captures the raw `ESP_LOGx` stream into a 16 KB
  RAM ring (boot log included; UART output preserved) served at the auth-gated
  `GET /api/v2/console`; terminal-style view with auto-refresh + `.txt` download.
  Motivated by newer no-USB Panda hardware (and release builds using the UART TX pin
  as the Power LED), where the serial console is otherwise unreachable.

### Changed
- **AUTO / fan-only filtration trigger on the bed SETPOINT, not the measured temp**
  (stock parity): heat/airflow engage as soon as the print *commands* bed ≥ threshold,
  and disengage when the setpoint drops. Applies to Klipper and Bambu; exposed as
  `environment.bed_target_c`.
- **Dashboard status is control-source-aware** — shows "Home Assistant" or
  "Bambu (&lt;serial&gt;)" instead of a misleading "Printer: not connected";
  `control_source` + `bambu_serial` added to the state API.
- **Periodic status heartbeat deduped** (whole-degree temperature compare) so
  `/console` and the serial log stay readable — repeated states collapse to a
  `repeated Nx` tally with a periodic liveness flush.
- **`tools/flash.py` auto-falls-back the baud rate** (460800 → 230400 → 115200) for
  flaky USB-serial links; `--baud` pins a single rate. All backup integrity checks
  (size, ESP magic, on-chip hash verify) are unchanged.

### Fixed
- **`/setup` no longer wipes Wi-Fi on a config-only save.** The Wi-Fi dropdown
  auto-selected a scanned network and submitted it with a blank password, dropping the
  device to AP mode; it now defaults to "keep current Wi-Fi" in STA mode, so switching
  control source (or editing any field) leaves credentials untouched.
- **Password reveal** on `/setup` stays a plain eye (strikethrough = hidden) instead
  of swapping to a monkey emoji.

## [0.8.0-rc3] - 2026-07-28

### Changed
- **Dashboard status is control-source-aware.** In Home Assistant mode the
  "Printer" row (which read a misleading "not connected", since HA has no printer
  to follow) is relabeled **"Source: Home Assistant"**, and the **Controller** row
  shows **"Home Assistant"** instead of the internal "Web UI (ha)". Bambu mode
  labels the row **"Bambu (&lt;serial&gt;)"** (the LAN report carries no friendly
  printer name, so the configured serial is used). Klipper mode is unchanged. The
  active source is exposed as `environment.control_source`, and the Bambu serial as
  `environment.bambu_serial`, in the state API.

## [0.8.0-rc2] - 2026-07-28

### Changed
- **AUTO / fan-only filtration now trigger on the bed SETPOINT, not the measured
  bed temperature (stock parity).** Heat (and the filtration band) engage as soon
  as the print *commands* a bed at/above the threshold — so the chamber warms
  alongside the bed instead of waiting for the bed to physically reach the
  threshold — and disengage when the setpoint drops (e.g. print end). Applies to
  both the Klipper (Moonraker) and Bambu sources; the bed setpoint is now plumbed
  through the AUTO seam and exposed as `environment.bed_target_c` in the state API.

### Fixed
- **Changing configuration in `/setup` no longer wipes Wi-Fi.** Switching the
  control source (or editing any field) could drop the device to AP mode: the
  browser's Wi-Fi dropdown auto-selected a scanned network and submitted it with a
  blank password, overwriting the saved credentials. The dropdown now defaults to
  "keep current Wi-Fi" in STA mode, so a config-only save leaves Wi-Fi untouched.
- **Password "show" toggle no longer swaps to a monkey emoji.** The reveal button
  stays a plain eye and indicates state with a strikethrough (struck = hidden).

## [0.8.0-rc1] - 2026-07-28

### Added
- **Selectable control source (Klipper / Bambu / Home Assistant).** The device now
  binds to exactly one printer/controller, chosen in `/setup` under a new "Control
  source" card. Klipper (Moonraker) remains the default and is unchanged. A new
  `pb_source` selector persists the choice; `app_main` starts only the selected
  client and feeds the existing AUTO seam (`pb_policy_set_env`) — the heater safety
  model is unchanged and fully source-independent.
- **Home Assistant integration (tested).** New `pb_ha` MQTT client connects to your
  broker, publishes MQTT Discovery (a climate entity + chamber/element temperature
  sensors auto-appear in HA), publishes retained state, and maps HA commands to
  heater target/mode — holding a device lease and heartbeating it like any remote
  controller. The climate entity declares Celsius (`temp_unit:C`) so HA converts
  °C↔°F correctly in both display and setpoints. Validated end-to-end on real
  hardware (device + Home Assistant + Mosquitto).
- **Bambu LAN integration (experimental — untested against a printer).** New
  `pb_bambu` MQTT-over-TLS client reads a Bambu printer's bed temperature over its
  on-device LAN broker (`mqtts://<ip>:8883`, `bblp` + LAN access code, subscribe
  `device/<serial>/report`, `pushall` on connect) so AUTO can follow a Bambu print.
  Read-only — no control commands are ever sent to the printer. Built from the
  OpenBambuAPI / ha-bambulab spec; **not yet validated on real hardware** — select
  "Bambu" in `/setup` to test. See `plans/control-source-bambu-ha.md`.

### Changed
- **`/setup` no longer forces Wi-Fi re-entry to change configuration.** In STA mode,
  saving with the Wi-Fi fields left blank keeps the existing credentials and just
  applies the control-source/config change. Wi-Fi is still required during initial
  AP provisioning.

## [0.7.2] - 2026-07-28

### Changed
- **`/setup` and `/fw` now match the dashboard theme (light + dark).** The
  captive-portal / provisioning / OTA pages were dark-only with their own palette;
  they now share the dashboard's `light-dark()` tokens, follow the device theme,
  and honor a pinned dashboard choice (same-origin `localStorage` `db_theme`). The
  `/fw` update page is header-free (the product header stays on `/setup` and the AP
  captive portal), its warnings ("Do not power off during the update") are bold +
  red, the SHA-256 line wraps instead of overflowing the card, and text no longer
  breaks mid-word.

### Fixed
- **Settings → Maintenance showed "--" for Firmware / Device ID / Boot ID.** The
  v0.7.1 header rework removed the brand element, but the `/api/v2/info` handler
  still set `$('brand').title`, throwing a null-reference that aborted the callback
  before it populated the Maintenance fields. Removed the stale brand-tooltip write.
- **Desktop no longer reflows or hides controls when the window/panel is narrow.**
  The dashboard is embedded as an iframe in Mainsail/Fluidd, where container/width
  queries measure the *panel*, not the desktop window — so a thin desktop panel
  wrongly triggered the compact tile (which hid the Quick-controls) and, before
  this, the phone stack. **All** responsive reflow — both the compact tile and the
  vertical stack — is now gated on `pointer: coarse` (an actual touch device), so
  desktop (mouse) keeps the full layout at any width and controls never disappear.
  Mobile is unchanged.
- **Version footer left-aligned** so it tracks the content's left edge instead of
  centering across the full main width (which drifted away from the ~540 px control
  card on wide screens).
- **Status rows no longer truncate in a narrow card.** The Printer line
  ("connected · bed 26 °C") was ellipsis-clipped; values now wrap, and in the
  compact/touch view each row stacks (label above value) so the full line shows.

## [0.7.1] - 2026-07-28

### Changed
- **Dashboard UI: reclaimed header space + mobile-friendly layout** (presentation
  only — no firmware/behavior change). Removed the topbar (the "DragonBreath"
  brand) and the per-page heading row that wasted vertical space; the mode name is
  now folded into each card's intro line ("Manual heat: …", "Automatic mode: …",
  "Dry cycle: …"), connection status moved to a dot at the top of the rail, and a
  small **"DragonBreath &lt;version&gt;"** footer (live firmware version) replaces the
  header brand. On narrow (phone) viewports the dashboard now **stacks vertically —
  graph, then status, then controls — and scrolls**, instead of the cramped
  two-column compact that also hid the controls; the wide/short Fluidd embed tile
  keeps its container-query compact layout.

## [0.7.0] - 2026-07-28

### Added
- **Phase E hardening — CI static-analysis gate + broader host/simulation fault
  coverage.** No firmware runtime behavior change; this is tests, CI, and small
  behavior-preserving refactors only.
  - **cppcheck static analysis** runs on every PR over `components/` + `main/`,
    failing the build on any error/warning/performance/portability finding. The one
    unmodellable false positive (a GNU `asm()` label on an extern binary-blob symbol
    in `pb_portal`) is suppressed inline with justification; the genuine finding it
    surfaced — a `%u`/`int` format-type mismatch in the Moonraker WebSocket URI — was
    fixed with a value-identical cast.
  - **Warnings-as-errors for our own code.** `-Wall -Wextra -Werror` is applied
    PRIVATE to every first-party `pb_*` component (never to ESP-IDF's own
    components), enforced by the default, HIL, and release builds.
  - **Two new host tests** wired into CI as required steps:
    `pb_heater_safety_host_test` covers the heater safety-trip priority ladder
    (PTC/chamber over-temp, fail-closed on a non-OK sensor while armed, comms-loss
    watchdog); `pb_ntc_status_host_test` covers the NTC open/short/rail fail-status
    mapping. The existing `pb_buttons` host test is now also run in CI.
  - **Two safety decisions factored into pure, unit-tested inlines** (behavior
    identical, single authoritative definition): `pb_heater_eval_trip()` — the exact
    ladder `pb_heater_tick()` runs — and `pb_ntc_classify()` — the exact
    open/short/rail classifier `pb_ntc_read()` runs.

### Fixed
- **HIL dev-board builds were silently broken.** Both dev-board HIL profiles
  failed to link (`undefined reference to pb_ntc_rref_kohm`) because that getter
  lived only in the real-ADC branch of `pb_ntc.c`, but CI still reported success —
  the HIL build step chained `idf.py` commands without `set -e`, so a failed link
  was masked by the subsequent compile-out check. The HIL NTC backend now provides
  a `pb_ntc_rref_kohm()` (nominal 82 kΩ; the HIL profile has no Rref strap), and the
  HIL build step runs with `set -e` so a build failure fails CI. Pre-existing since
  the Rref-strap work; surfaced by the PR #42 review.

## [0.6.5] - 2026-07-28

### Added
- **Fan-only chamber filtration (two ways).** The chamber blower can now run
  fan-only — heater untouched — to filter/circulate chamber air:
  - **Automatic in AUTO mode (stock-like).** When enabled (default **on**), AUTO
    runs the blower alone once the printer bed reaches a configurable **filter
    temperature** (default **30 °C**, range 20–60 °C), filtering before the heater
    engages at the higher auto bed threshold. Hysteresis mirrors the engage band;
    it fails to no-airflow if disabled or Moonraker drops. Set in **Settings →
    Filtration (auto mode)** / `GET/POST /settings?filter_temp=&filter_auto=`.
  - **Manual, out-of-band.** A new `filter` API v2 command runs the blower fan-only
    **independent of mode** — not cleared by OFF, additive over any heat airflow,
    and safe (no heat path). Surfaced as an **on-only Filtration button** on the
    dashboard — like the temperature presets, a click activates it and it lights up
    (turns blue) while active; **Stop** is the single control that turns it off.
    **Enabling** it is idle-only: while the heater is heating or the cooldown purge
    is running, turning filtration *on* is rejected (`heater_busy`, HTTP 409) and the
    dashboard button dims, so a status-page toggle can't disturb an active heat cycle.
    Turning it *off* is always allowed, and **Stop stops all** — the dashboard Stop
    button clears filtration alongside the heater (and is clickable whenever
    filtration is on), so filtration never lingers or "comes back" after a heat cycle.
  - State adds `environment.auto_filtering` and `params.filter_temp_c` /
    `params.filter_auto_enable`; the manual fan drives the existing (previously
    unused) `requested_fan_percent` blower path. The heater and every over-temp
    cutoff are unaffected.

## [0.6.4] - 2026-07-27

### Added
- **Configurable element-foldback cut (advanced).** The soft over-temp foldback cut
  temperature is now user-settable in **Settings → Foldback cut** (`GET/POST
  /settings?fb_cut=`), bounded to **90–104 °C**. It defaults to **auto** — the board's
  per-Rref value (99 °C on 33 kΩ / V1.0, 102 °C on 82 kΩ / V1.0.1) — and the slider shows
  that default; setting it back to the default clears the override (0 = auto). Lower it if
  the element runs hot/slow, raise it for more chamber temperature. This only shifts where
  the **soft** foldback engages; the fixed **105 °C hardware cutoff is unaffected** and the
  override can never exceed 104 °C, so it can't defeat over-temp protection.

## [0.6.3] - 2026-07-27

### Added
- **Element-temperature foldback limiter (hysteresis).** Instead of driving the heater
  full-power into the 105 °C PTC-element cutoff and hard-faulting, the SSR is now cut off
  when the element reaches a per-board **cut** point and held off until it cools below the
  **resume** point, so the element repeatedly cools back down instead of pinning against
  the cutoff — the chamber keeps warming and a hot/marginal install no longer trips into
  the "clear → trip again" loop. Thresholds are selected by the board's Rref strap:
  **82 kΩ (V1.0.1) cut 102 / resume 99**; **33 kΩ (V1.0, runs the element hotter) cut 99
  / resume 96** (a floating strap defaults to the conservative 33 kΩ pair). (An earlier proportional-duty ramp proved too gentle on a hot-running board: ~50 %
  duty at 102–103 °C kept feeding the element so it ratcheted to ~104.8 °C; the hysteresis
  forces a genuine cool-down cycle instead.) **Safety is unchanged:** the foldback can
  only ever *remove* power, and the hard 105 °C cutoff remains the first, unconditional,
  latching check — if the element still reaches it (welded SSR, runaway, sensor fault) it
  latches off exactly as before. The pure hysteresis decision is covered by host tests.

### Changed
- **OTA update page shows upload progress and returns to the dashboard automatically.**
  The firmware-update page (`/fw`) now streams the image via `XMLHttpRequest` and shows a
  live **percent complete** while uploading/flashing (the device writes bytes as it
  receives them, so the upload % tracks the flash), then — once the image is accepted —
  **polls the rebooting device and redirects to the main screen** when it comes back on
  the new firmware (with a ~2-minute fallback). A connection drop after the upload
  finishes is treated as the expected reboot; a drop mid-upload is reported as a failure
  so it can be retried.
- **Dashboard temperature chart overlays chamber + PTC element with labeled axes.** The
  trend now draws **both** the chamber and the PTC-element temperature on one shared,
  auto-scaled graph with a **Y axis** (°C max/min) and an **X axis** (time span back from
  "now"), plus a legend. All from the existing SSE telemetry; no API change.

### Fixed
- **Rref strap misread on a floating GPIO19 (both NTC readings ~15 °C off).** The
  thermistor divider's reference resistor (82 kΩ vs 33 kΩ) is selected by a strap on
  GPIO19, read once at boot — previously with both internal pulls disabled. On a board
  that doesn't firmly drive the pin it **floats** and can latch the wrong value,
  **differently on a cold power-on vs a warm OTA reboot** — silently choosing the wrong
  Rref and shifting **both** the chamber and PTC readings by ~15 °C (observed on a V1.0
  board: 27 °C after a USB flash, 12 °C after an OTA update; a couple of hard resets read
  27 °C again). The strap is now sampled under an internal pull-up **and** an internal
  pull-down: a firmly strapped pin reads identically both ways and is trusted, while a
  floating pin disagrees and falls back to the **fail-safe 33 kΩ** default (a smaller
  Rref biases readings *warm*, so the fixed 105 °C/85 °C over-temp cutoffs trip early
  rather than late). The resolved value is exposed at `GET /api/v2/info` as `rref_kohm`
  for diagnostics. Firmly-strapped boards are unaffected (V1.0.1 bench verified: firm →
  82 kΩ, reading unchanged).

## [0.6.2] - 2026-07-26

### Added
- **Configurable cooldown-fan "cool down to" temperature.** The residual-heat purge
  (the fan that keeps running after a heat session until things cool off) previously
  released at a fixed 40 °C. That temperature is now a persisted, user-settable
  slider in **Settings → Cooldown fan** (range 30–65 °C, default 40 °C, also on
  `GET/POST /settings?cool_release=`). Raise it for a hot ambient where the chamber
  and heater element can't fall back to 40 °C — otherwise the purge fan would run
  indefinitely. The engage point stays one 3 °C hysteresis band above the release
  temperature. This is a comfort/wear setting, not a safety cutoff: the fixed
  105 °C PTC / 85 °C chamber over-temp trips and their fault-driven airflow are
  independent and unchanged.

## [0.6.1] - 2026-07-24

### Changed
- **Vendored the OpenVent shared core locally (no more submodule).** The three
  board-agnostic components that were built from the `external/OpenVent` git
  submodule (`pv_evlog`, `pv_wifi`, `pv_moonraker`) are now first-party components
  in `components/`, renamed `pb_evlog` / `pb_wifi` / `pb_moonraker` to match the
  rest of the codebase. The submodule, `.gitmodules`, and `EXTRA_COMPONENT_DIRS`
  are removed; CI no longer checks out submodules; the release manifest records
  the vendored-core provenance instead. `git clone` no longer needs
  `--recurse-submodules`. Derived from OpenVent (MIT) — see
  [VENDORING.md](VENDORING.md). No firmware behavior change.

## [0.6.0] - 2026-07-24

### Added
- **Persistent safety-fault latch (B2).** A hazard-driven safety trip (PTC/chamber
  over-temp, sensor fault while heating, comms-loss watchdog) now survives a power
  cycle: it is written to NVS on the latching transition and restored at boot, so a
  device that tripped before losing power comes back up **heater-OFF with commands
  inhibited** instead of silently ready to heat. NVS is loaded **before** the
  control task starts, and a failed persist is **retried** (a pending flag keeps
  trying until the commit lands) so an early or transiently-failed write can't lose
  the latch across a reboot. The persisted cause is a stable numeric code (a live
  reason string still carries session detail). Boot restore is **fail-safe** — if
  the stored state cannot be read reliably the device comes up latched. Clearing a
  fault is **persist-first**: if the NVS clear fails the latch is kept and the API
  returns HTTP 500 (`persist_failed`) rather than falsely reporting it cleared.
  User panic-off and the permanent inhibit are not persisted (documented
  reboot-clears behavior); active mode/target/deadline/lease still never persist.

### Changed
- **Residual-heat purge hardened.** The post-heat cooldown fan now uses hysteresis
  (engage at ≥ 40 °C, release only once **both** the chamber and PTC sensors are
  below 37 °C) instead of a single chamber-only threshold, and it also runs when a
  sensor reading is **unknown** right as heating ends (can't confirm cool → fail
  safe). It remains strictly session-gated: the fan never starts on temperature
  alone, and a power-cycle-while-hot does **not** spin it.

## [0.5.2] - 2026-07-24

### Fixed
- **Live UI could silently stop updating (SSE slot leak).** The device caps
  concurrent SSE event streams at 2, but a client that disconnected without a
  clean close (tab killed, Wi-Fi blip, or a Moonraker-link flap churning the
  network) held its slot for *minutes* — so after a couple of such drops every new
  stream got `503` and the dashboard/Klipper live view froze even though the device
  itself was fine and responsive. TCP keepalive is now enabled on the HTTP server
  so a vanished peer is detected in ~25 s, and the SSE task additionally checks for
  a peer FIN/RST every loop and frees the slot immediately (~0.3 s on the bench).
  Normal (non-SSE) request latency was already healthy (~20 ms) and is unchanged.
- **Dashboard shows fan and controller status again (regression from v0.5.0).**
  The rewritten dashboard dropped the old at-a-glance detail, so you could no
  longer tell what the fan was doing or who was in control. The System status card
  now shows **Fan** (off, or on with the reason — heating / cooldown purge /
  manual / safety airflow — and %), **Controller** (command source + lease owner),
  **Auto** (engaged or waiting, with the bed threshold, when armed), **Drying**
  (time remaining, when active), **Printer** (Moonraker link + bed temperature),
  and **Fault**. All of it comes from the existing SSE `/api/v2/state` stream — no
  API or firmware-behavior change.

## [0.5.1] - 2026-07-24

### Fixed
- **Web UI no longer sprawls on large / 4K displays.** The SPA is built to fill
  its Fluidd/Mainsail iframe, so opening it standalone on a big monitor stretched
  every element edge-to-edge. It is now bounded to a centered panel (max
  1200×820) on large viewports while still filling a small embed.
- **Control-screen action button no longer floats over the last control.** On
  Manual/Auto/Dry the primary action (e.g. "Start drying") used a sticky footer
  that overlapped the final control (target/duration) when the screen was tight.
  The action now sits in normal flow directly below the controls.
- **Firmware-update (`/fw`), Wi-Fi setup (`/setup`), and the AP captive portal
  match the new UI.** These pages still used the old stock-BIQU blue banner; they
  now use the charcoal palette and dragon mark of the main app.

## [0.5.0] - 2026-07-24

A ground-up **responsive, touch-first Web UI** (issue #19), delivered as a
self-contained gzip-embedded single-page app that reads cleanly from a phone up
to a desktop and embeds in the Fluidd/Mainsail panel. The rewrite also lands the
settings/maintenance/calibration surface that backs it, sensor calibration, and
a round of safety hardening on the mutating endpoints.

### Added
- **Touch-first single-page UI (`pb_portal/www/app.html`).** One embedded,
  gzip-compressed SPA replaces the old status page: a live **Dashboard**
  (chamber/PTC temps, mode, trend, stop/clear) and dedicated **Manual**,
  **Auto**, and **Dry** control screens with sticky primary actions, plus a
  **Settings** screen. State streams over `GET /api/v2/events` (SSE) and the UI
  pre-fills every control from the device's remembered `params`. (#19, #21)
- **Settings, maintenance, and diagnostics endpoints.** New v2 API surface behind
  the auth header: `GET/POST /api/v2/calibration`, `POST /api/v2/restart`,
  `POST /api/v2/factory-reset`, `GET /api/v2/logs` (a heap snapshot of the event
  ring), and `POST /api/v2/token` to set/clear the control token.
- **Sensor calibration (bounded).** Per-sensor chamber/PTC offsets, clamped to
  **±5 °C** on both set and NVS load and applied only to good readings, so a
  stored or requested offset can never move a safety cutoff by more than the
  documented bound. See [`docs/SAFETY.md`](docs/SAFETY.md).
- **Configurable status LEDs.** LEDs can be enabled/disabled from Settings
  (persisted), reported in `/settings`.
- **Brand mark & favicon.** An angular dragon-head logo that inherits the theme
  and stands alone in the compact header, plus a theme-adaptive favicon — an
  inline SVG `<link>` and a real 32×32 PNG served at `/favicon.ico` (registered
  before the SPA catch-all) so browser tabs show the mark.
- **Light/dark and accessibility.** An Auto/Light/Dark toggle persisted to the
  browser, `focus-visible` outlines on all controls, a `prefers-reduced-motion`
  path, and an `aria-label`/`title` on the connection indicator so state is not
  conveyed by colour alone.

### Fixed
- **Prompt control feedback across every command source.** Accepted front-panel,
  Web UI, Klipper, and HIL mode commands now wake the full control task
  immediately instead of waiting up to 500 ms for the periodic tick, so panel
  LEDs and outputs track authoritative state promptly. Rejected button commands
  now log the actual policy result, and remembered mode targets are clamped to
  the runtime-configured heater maximum as soon as they load from NVS. (#20)
- **Mutating endpoints fail closed and honestly.** `factory-reset` now captures
  `nvs_open`/`nvs_erase_all`/`nvs_commit` results and returns HTTP 500 without
  rebooting unless the erase and commit both succeed. Calibration and the LED
  toggle now persist first and apply the in-RAM change only after a successful
  commit, returning HTTP 500 `persist_failed` instead of reporting a save that
  did not happen. Restart, factory-reset, **and** OTA (`/update`) now refuse
  whenever the device is in any armed mode (`mode != OFF`), matching the
  documented contract for armed Auto waiting below the bed threshold.

### CI
- The calibration clamp safety test (`run_ntc_calibration_host_test.sh`) is now a
  required host-test step, and its load-path clamp is exercised with out-of-range
  stored NVS values so the ±5 °C invariant is verified in CI.

## [0.4.0] - 2026-07-24

Phase C — the physical front panel comes alive. All four buttons and all four
status LEDs are wired to the authoritative control state, so the device is fully
operable and legible from the panel alone, with a long-press panic-off. Also
lands the mode-parameter persistence that v0.3.0 documented but never shipped,
plus the serial hardware-in-the-loop harness and an explicit OEM-parity matrix.

### Added
- **Front-panel buttons (`pb_buttons`).** All four buttons (Power, Auto, On, Dry)
  are polled at 10 ms with 20 ms debounce and short/long-press detection. A short
  press toggles that button's labeled mode, arming it from the remembered
  parameters; a 2 s long-press latches a **panic-off**. A long-press on Power
  while a fault is latched attempts a fault clear instead. Every button action is
  attributed to the panel, invalidates any remote control lease, and appears in
  both the dashboard and Klipper. A button held at power-on (or a shorted line)
  is ignored until it releases — Power/Auto/Dry are ESP32-C3 strapping pins, so
  **do not hold a front-panel button while the board boots**. The debounce /
  long-press state machine is split into a dependency-free `pb_buttons_sm` unit
  and host-tested directly.
- **Long-press panic-off.** `pb_heater_request_panic_off()` latches the heater
  off from any task without touching the SSR GPIO, and the policy drives the full
  OFF transition (attributed to the button, lease invalidated) then wakes the
  control task by notification so the SSR drops on the very next scheduling rather
  than at the next periodic tick. It is **not** a safety-rated emergency stop —
  see [`docs/SAFETY.md`](docs/SAFETY.md).
- **Remembered mode parameters (persisted).** The last accepted manual target,
  automatic target and bed threshold, and drying target and duration are now
  stored in NVS and reported as `params` in `GET /api/v2/state`, so the UI
  pre-fills from the device and a mode can be re-armed without re-entering
  values. Closes a gap left by v0.3.0, which documented this persistence but
  never implemented it. Writes are serialized through a single worker task and
  record the clamped value the device actually applied. Parameters remain the
  **only** policy state that survives a reboot — the active mode, target,
  deadline, and lease still do not, so the device always boots OFF.
- **Serial hardware-in-the-loop harness (`pb_hil` / `tools/hil.py`).** A
  line-delimited JSON console for injecting chamber/PTC readings, sensor faults,
  printer environment, and zero-cross events, and for reading back heater demand,
  fan state, LEDs, mode, lease, and fault state. Ships with an isolated ESP32-C3
  dev-board target whose mains GPIO is **compiled out**, so it is structurally
  incapable of energizing Panda hardware, plus scripted scenarios, console
  capture, and JSON pass/fail reports. A non-heating UART profile for the real
  Panda is documented as the pre-release qualification gate. (#14)
- **OEM parity matrix (`docs/OEM_PARITY.md`).** Tracks every user-visible stock
  Panda Breath behavior as implemented, partial, planned, intentionally changed,
  intentionally omitted, or unverified — so deliberate deviations are
  distinguishable from gaps. (#12)

### Changed
- **Front-panel LEDs now show the active mode.** All four outputs are driven from
  the authoritative policy snapshot instead of Power and On duplicating a single
  "heating" signal: Power is solid whenever the device is up and blinks on fault,
  while On, Auto, and Dry each light for their own mode. Auto slow-blinks when
  armed but not engaged (no Moonraker link, or bed below the threshold), so the
  panel distinguishes "waiting" from "heating". Power remains release-only —
  GPIO21 is also the serial console TX.

### Fixed
- **Automatic and drying controls work from the dashboard.** v0.3.0 shipped both
  modes in the state machine and API but the UI could not reliably drive them.
  The cards now submit correctly, their input bounds match the policy's own
  limits, and automatic status refreshes from live state rather than the value
  last typed. (#13)
- **`docs/HARDWARE.md` GPIO map corrected.** It still described buttons on GPIO7
  and GPIO0 — those are the zero-cross detector and the chamber NTC. The table now
  matches the bench-probed map already in `pb_board.h` (buttons on 9/8/10/2, Power
  LED on 21) and documents the strapping-pin caveat.
- **Wrap-safe control-loop scheduling.** The notify-aware control tick compared
  FreeRTOS tick counts with an unsigned deadline test, which inverts across the
  32-bit tick wrap (~497 days at 100 Hz) and would let the loop burst-tick for up
  to one period. Now uses signed tick deltas, so the "absolute deadline, never
  accelerates" invariant holds across a wrap.

## [0.3.0] - 2026-07-23

Iteration-2 core: an authoritative device-side control state machine, a
versioned API, configurable safety settings, and real front-panel status LEDs.

### Added
- **Configurable safety settings (persisted).** Runtime-settable max-target
  ceiling (default 70 °C, hard-capped at 70) and comms-watchdog timeout
  (default 5 min, clamped to 10 s–5 min), stored in NVS and exposed via
  `GET`/`POST /settings` and an Advanced/Safety card in the web UI. The fixed
  105 °C PTC / 85 °C chamber cutoffs remain non-configurable. (#10)
- **Real status LEDs.** The three mode LEDs (Auto/On/Dry) plus the **Power LED**
  (GPIO21) are now driven to match the stock panel: Power/On solid while heating,
  blink on a latched fault, off at idle. Because GPIO21 is the console-TX pin, the
  Power LED is enabled only in release builds (`CONFIG_PB_POWER_LED`, set via
  `sdkconfig.release`); dev builds keep the serial console. (#10)
- **Authoritative control state machine (`pb_policy`).** A single device-side
  owner of mode/target/lease state (Off / Power-On / Auto / Dry), with
  lease-based remote ownership, revision-aware commands, and a boot-OFF /
  no-auto-resume safety posture. (#9)
- **API v2 (`/api/v2/*`).** Snapshot-authoritative `state`/`info`/`health`, an
  SSE `events` stream (push instead of polling), and auth-gated `command` /
  `heartbeat` with request-ID idempotency and exact-lease heartbeats. (#11)

### Changed
- **BREAKING (alpha API):** the alpha routes (`/status`, `/target`, `/heartbeat`,
  `/reset`) are removed in favor of API v2. Requires the matching
  **dragonbreath-klipper v2 helper** — flash firmware ≥ v0.3.0, then restart
  klippy. Version mismatches fail safe (the chamber heater simply doesn't engage).

## [0.2.0] - 2026-07-23

### Added
- **In-UI update notification** — on official (tagged) builds, the device `/fw`
  page checks GitHub for a newer release and shows a download link + expected
  SHA-256; you verify and flash it via the existing uploader (fully browser-side).
- **Reproducible release pipeline** — pushing a `v*` tag builds in CI (pinned
  ESP-IDF v5.3.5) and publishes a GitHub Release with a single-file
  `-factory.bin` (first install, flash @ 0x0), the `.bin` application image (OTA),
  a complete `-install-bundle.zip` (flasher + components + `FLASHING.txt`), a
  `manifest.json` (source SHA, ESP-IDF version, submodule, per-artifact SHA-256),
  and `SHA256SUMS.txt`.
- **Post-print fan cooldown** — after a print, the blower keeps running until the
  chamber cools below 40 °C, then stops. Gated on a heat-this-session flag, so it
  never auto-starts on temperature alone (a reboot-while-hot leaves the fan off).

### Changed
- **Renamed OpenBreath → DragonBreath** across the firmware: build/project
  identity and OTA image-identity gate, HTTP auth header `X-DragonBreath-Auth`,
  Wi-Fi AP SSID `DragonBreath_XXXX` (with migration of the legacy default), mDNS
  hostname `dragonbreath.local` (app-layer override; shared OpenVent core
  untouched), web-UI title (🐉) and strings, logs, and docs. "Panda Breath"
  remains only as the underlying-hardware descriptor.

### Migration
- The OTA image-identity gate now requires `project_name == dragonbreath`, so a
  device on pre-rename firmware must be **USB-reflashed once** to cross over; OTA
  works normally afterward.
