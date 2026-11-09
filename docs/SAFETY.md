# Safety model

The Panda Breath heats a chamber with a **PTC ceramic element** switched by a
**solid-state relay**. Custom firmware controls that heater, so the safety story
matters. The physical protections include a **bonded thermal protector** and the PTC
element's self-limiting behavior; neither is controlled by this firmware. Their
presence does not establish a measured maximum chamber temperature or a complete
fault-safety envelope for every installation. The firmware provides additional
software interlocks.

## Layer 1 (hardware) — bonded thermal protector
A cylindrical thermal protector (bimetal KSD-type disc or one-shot thermal fuse)
is reported in series with the PTC's mains lead and bonded to the heater frame
(maintainer inspection referencing photo IMG_7631; the photo is not included
in this repository). It is reportedly upstream of the SSR; if that placement
is correct, a properly functioning protector can interrupt power even if the
SSR fails shorted.
Its exact part number, rated trip point, tolerances and behavior under the full
fault envelope are not established by the firmware's temperature constants.

## Layer 2 (physics) — PTC self-limiting
The element is a **PTC ceramic**: its resistance rises sharply at the Curie
temperature, collapsing its own power, so it is strongly **self-limiting** rather
than thermally runaway-prone. PTC behavior reduces the risk of uncontrolled element heating, but it does
**not** by itself establish a safe chamber, wiring, enclosure or component
temperature during an SSR failure. The bonded protector and PTC material need
independent identification and fault-condition characterization.

## Layer 3 (this firmware) — soft cutoffs
`pb_heater` enforces, every control tick, before any heat request:
- **PTC element over-temp** → force off at 105 °C (stock parity). Always the first,
  unconditional, latching check.
- **Element foldback limiter** → below the 105 °C cutoff, the SSR is cut with
  hysteresis at a per-Rref point and held off until the element cools to a resume
  point, so a hot/slow element stays below the fixed 105 °C software trip instead of ratcheting
  into it. Cut point is per-board (82 kΩ → 102 °C / 33 kΩ → 99 °C) or a user
  override `fb_cut` (90–104 °C). It can **only remove power**, can never exceed
  104 °C on the calibrated PTC reading (subject to the offset described below),
  and does not touch the fixed 105 °C **software** cutoff. The separate bonded
  thermal protector is a physical device, not this 105 °C constant.
- **Chamber over-temp** → force off at 85 °C.
- **Sensor fault while heating** (open/short) → fail-closed.
- **Comms-loss watchdog** → if no controller link while heating for the watchdog
  timeout — **5 min by default, runtime-configurable to 10 s–5 min** (`comms_ms`,
  clamped to `[PB_HEATER_COMMS_TIMEOUT_MS_MIN, …_MAX]`; never disabled or beyond
  5 min) — latch off. (The device can't tell "idle hold" from "controller crashed",
  so a live link must be refreshed via `pb_heater_notify_link_alive()`.)
- **Set-point clamp** → target capped at the runtime `max` ceiling (default 70 °C),
  itself hard-bounded by `PB_HEATER_ABS_MAX_TARGET_C` (70 °C) — a target can never
  exceed the absolute cap regardless of settings.
- **Boot state** → SSR forced OFF in `pb_heater_init()` before anything else runs;
  a hung control loop trips the task watchdog (`CONFIG_ESP_TASK_WDT_PANIC`).

## Sensor calibration is bounded (cannot defeat a cutoff)
Each temperature channel (chamber, PTC) has a user-settable calibration
**offset** so a mis-reading sensor can be corrected. The offset is **hard-clamped
to ±5 °C** and the clamp is enforced **on every set AND on every NVS load** — a
corrupt or hand-edited stored value clamps to the bound, it is never applied raw.
The offset is applied inside `pb_ntc`, so the display, control regulation, AUTO,
and the over-temp cutoffs all act on the **same** calibrated value.

The software compares the **calibrated reading** `estimated + offset` with each
fixed threshold `T`. In terms of the unoffset temperature estimate, the trip
condition is `estimated >= T - offset`. An offset of +5 °C makes a threshold
trigger 5 °C earlier; an offset of -5 °C makes it trigger 5 °C later. Thus the
85 °C chamber trip corresponds to **80–90 °C** on the unoffset estimate, and
the 105 °C PTC trip to **100–110 °C**. These are calculated *estimate* ranges,
not guaranteed actual temperatures: sensor error, placement and thermal lag add
uncertainty. The constants (`PB_HEATER_CHAMBER_MAX_C` = 85,
`PB_HEATER_PTC_CUTOFF_C` = 105) remain fixed and calibration does not change the
raw-count sensor fault classification. Bounded calibration is not a substitute
for verifying the independent physical protector.

## Thermistor channel mapping
`swap_thermistors` is an opt-in setting for the uncommon case where the chamber
and PTC sensor connectors are plugged into each other's JST sockets on the board.
It swaps the complete logical
channels, so the 85 °C chamber cutoff, 105 °C PTC cutoff, fault status, and
per-channel calibration follow the selected sensor roles. The choice is staged
in NVS and takes effect only after reboot; the live mapping cannot change while
the control loop is running. Leave it off unless crossed channels are confirmed,
and verify both readings on `/diag` before enabling heat. Reversing the two leads
of a single two-wire thermistor does not require this option.

## Front-panel panic-off (long-press)
Holding **any** front-panel button for 2 s latches the heater off and drops the
mode to OFF. It is called **panic-off**, deliberately, and is **not** a
safety-rated emergency stop — it is software running on the same MCU as the rest
of the control loop, so a firmware fault could defeat it. The separate bonded
protector and PTC self-limiting behavior provide additional physical protection,
but their complete fault envelope has not been established.
The panic-off latch is RAM-only and clears on reboot (it is a manual stop, not a
hazard trip); reboot still starts with the SSR, mode, and target OFF, and never
restores an active heat command. Hazard-driven trips, by contrast, **persist** —
see below.

How it stays inside the safety model:
- The button task never writes the SSR GPIO. `pb_policy_request_panic_off()`
  sets the same mux-guarded latch as an over-temp trip
  (`pb_heater_request_panic_off()`, no GPIO write) and then wakes the control
  task, which drops the SSR from its own context — preserving the
  single-SSR-writer invariant.
- The wake means the drop happens on the very next control-task scheduling, not
  at the next 500 ms periodic tick. Host tests verify that the wake is issued
  before diagnostic logging, and the physical Panda bench verified the complete
  panic-off transition. Electrical long-event-to-SSR-low latency has not been
  instrumented, so no sub-20-ms measurement is claimed; that trace is optional
  characterization, not a safety or release gate.
- The transition is attributed to the button (not the generic safety path), so
  it invalidates any remote control lease immediately; a reconnecting
  dashboard/Klipper cannot silently restore heat.

A **long-press on Power while a fault is latched** attempts a fault clear instead
of a redundant panic. The clear only succeeds if the underlying condition has
recovered — the next control tick re-evaluates safety and re-latches if the trip
still holds, so an accidental press cannot revive an unsafe device.

## Persistent fault latch (survives a power cycle)
A **hazard-driven** safety trip — PTC/chamber over-temp, a sensor fault while
heating, or the comms-loss watchdog — is written to NVS on the latching
transition and **restored at boot**. A device that tripped and then lost power
comes back up **heater-OFF with heat/mode commands inhibited**, rather than
silently ready to heat, and stays that way until an **explicit** clear after the
condition has recovered (the next tick re-latches if it hasn't). The persisted
cause is a small, range-checked numeric code; a live reason string carries
session detail but is not persisted.

Boot restore is **fail-safe**: if the persisted state cannot be read reliably it
comes up latched (`persisted fault state unreadable`). A fresh device (no NVS
namespace yet) is not a fault. Clearing is **persist-first** — the NVS latch is
cleared *before* the RAM latch, so a failed persist keeps the heater latched and
the API returns HTTP 500 rather than falsely reporting the fault gone.

This is deliberately separate from heat resume: the persisted latch is the **only**
fault state that survives a reboot. The active mode, target, deadline, and lease
are never persisted — the device always boots OFF. The permanent inhibit
(`pb_heater_inhibit`) is likewise **not** persisted, so a power cycle lifts it (a
transient init failure must not brick the device across reboots).

## Residual-heat purge (session-gated, hysteresis)
After heat has run **this session**, the cooldown fan keeps running while the
chamber **or** PTC sensor is hot. The release temperature is the user-configurable
`cool_release` (30–65 °C, default 40 °C); the fan engages one 3 °C hysteresis band
above it and releases only once **both** sensors are below `cool_release` (an
unknown/faulted sensor keeps the fan on). It is strictly session-gated: the fan
**never** starts on temperature alone, and because that gate is RAM-only, a
**power-cycle-while-hot does not spin the fan**. A fault forces airflow ON
unconditionally, independent of this purge.

## Fan-only filtration is heater-independent and idle-only to enable
The manual filtration blower (`filter` command) and the AUTO fan-only filtration
band drive **only the blower** — they never engage the heater/SSR. Enabling manual
filtration is **idle-only**: turning it on while the heater is heating or the
cooldown purge is running is rejected (`heater_busy`, HTTP 409) so a status-page
toggle can't disturb an active heat cycle; turning it *off* is always allowed. The
AUTO band fails to no-airflow if disabled or if Moonraker disconnects.

## What is NOT protected
- **Over-current** is only the 6.3 A mains fuse. There is no PCB over-temp cutoff.
- Residual risk includes chamber, wiring and enclosure overheating if the SSR
  fails or airflow is compromised. The bonded protector's actual trip point,
  tolerance and resulting chamber temperature have not been characterized here;
  a stock-equivalent worst-case ceiling is not established.
- **Panic-off is not an emergency stop** (see above): a firmware fault could
  defeat it. Do not rely on it as your only means of making the device safe.

## Rule for contributors
Any code path that can energize `PB_GPIO_RELAY` must be reachable only after the
`pb_heater` safety checks. Never drive the SSR GPIO directly from another module.
Cross-task "off" requests (buttons, HTTP, watchdogs) must go through a
mux-guarded latch that converges on the next `pb_heater_tick()` — never call
`pb_heater_emergency_off()` (which writes the GPIO) from a task other than the
control task.
