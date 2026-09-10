# Using the chamber heater

Adding a chamber heater to a printer that was not designed or configured for one
requires changes to the print workflow. DragonBreath provides heater control and
safety; it cannot know how a particular printer should home, position its bed,
run its fans, heat-soak, or sequence its bed, nozzle, and chamber.

It therefore does **not** automatically turn on the bed when chamber heating
starts, and installing a Klipper integration does not rewrite slicer profiles or
print-start macros. The integration supplies tools, not a universal startup policy.

## What the Klipper integration provides

- `M141 S<temperature>` starts chamber heating without waiting.
- `M191 S<temperature>` starts chamber heating and waits for the chamber target.
- `M141 S0` turns chamber heating off.
- `SET_HEATER_TEMPERATURE HEATER=dragonbreath TARGET=<temperature>` is the
  corresponding native Klipper command.
- The `heater_generic` object exposes chamber state to Klipper front ends and
  macros.

DragonBreath remains responsible for its independent sensor checks, target clamp,
element foldback, over-temperature shutdown, cooldown airflow, and communications
watchdog. The printer configuration remains responsible for when to issue those
commands.

## First choose who controls the target

Use exactly one of these control workflows:

| Workflow | Target source | Print-start behavior |
|---|---|---|
| [DragonBreath AUTO](#dragonbreath-auto) | DragonBreath filament-zone settings | Follows an active print; does not block print start |
| [Slicer / Klipper control](#slicer--klipper-control) | Slicer settings and printer G-code | Completely defined by the user's start/end G-code |

Do not mix them. A positive `M141`, `M191`, or `SET_HEATER_TEMPERATURE`
request starts a manual `POWER_ON` session and replaces AUTO.

The UI reflects that ownership choice. When an active `[dragonbreath]` Klippy
configuration is detected, slicer/Klipper owns chamber heat and AUTO is hidden.
This is expected—not a missing feature. Disable the active helper configuration
and restart Klipper to use AUTO; DragonBreath's direct Moonraker connection
supplies the printer state for that workflow.

## Decide the order of operations

There is no single correct heating sequence. Choose one that suits the printer,
material, enclosure, and desired soak time.

| Desired behavior | Command shape |
|---|---|
| Start bed and chamber together, then wait for both | `M140`, `M141`, later `M190`, `M191` |
| Heat and soak the chamber before starting the bed | `M141`, `M191`, then `M140`, `M190` |
| Heat the bed before starting the chamber | `M140`, `M190`, then `M141`, optionally `M191` |
| Start both but do not delay printing for the chamber | `M140`, `M141`, later `M190`; omit `M191` |
| Add a fixed soak after temperatures are reached | Wait with `M190`/`M191`, then use the printer's dwell or soak macro |

These are examples, not requirements. A printer may need to home before lowering
or raising the bed, keep electronics-cooling fans running, park the toolhead away
from a hot area, or limit its chamber target. Put those printer-specific decisions
in its start macro or Machine start G-code.

## The OrcaSlicer trap

When both **Support controlling chamber temperature** (printer preset) and
**Activate temperature control** (filament preset) are enabled, OrcaSlicer emits:

```gcode
M191 S<chamber-temperature>
```

before the printer's entire Machine start G-code. `M191` is blocking, so the usual
bed command has not run yet. The chamber heats alone and the rest of the startup
appears stuck. This is
[Orca's documented command ordering](https://github.com/OrcaSlicer/OrcaSlicer/wiki/material_temperatures#print-chamber-temperature),
not a DragonBreath fault.

Orca's default is effectively the “chamber first” policy. That may be desirable
for some printers, but it is not automatic bed/chamber coordination. If a different
policy is wanted, place the commands manually:

1. Leave the printer preset's **Support controlling chamber temperature** enabled.
2. In each filament preset, set the desired chamber temperature but leave
   **Activate temperature control unchecked**.
3. Add `M141`/`M191` where they belong in Machine start G-code or the printer's
   `PRINT_START` macro.
4. Add `M141 S0` to the end workflow.
5. Slice a small object and inspect the generated G-code to verify the actual
   command order and rendered temperatures.

Keeping the chamber temperature set while **Activate temperature control** is
unchecked leaves Orca's `overall_chamber_temperature` and `chamber_temperature`
placeholders available without its automatically injected `M191`/`M141` commands.

## Slicer / Klipper control

For the common “start together, wait later” policy, find the printer's early
non-blocking bed command:

```gcode
M140 S{bed_temperature}
```

and start the chamber beside it:

```gcode
M140 S{bed_temperature}
M141 S{chamber_temperature}
```

Then find the later bed wait:

```gcode
M190 S{bed_temperature}
```

and wait for the chamber beside it:

```gcode
M190 S{bed_temperature}
M191 S{chamber_temperature}
```

This starts both heaters together. When execution reaches the waits, the bed stays
hot while the chamber finishes. Moving `M191`, omitting it, or adding a soak after
it produces the other policies described above.

The placeholders shown here are illustrative. Use the variable names already
provided by the slicer/printer profile. In Orca, the chamber target is normally
`{overall_chamber_temperature}` (highest target across all filaments) or
`{chamber_temperature[0]}` (first filament only).

If the printer uses a parameterized `PRINT_START`, pass the bed and chamber values
to that macro and put the same command sequence inside it. Do not duplicate its
existing `M140`/`M190` calls. At print end, explicitly issue `M141 S0` unless an
existing end hook already turns DragonBreath off.

## DragonBreath AUTO

Use AUTO when DragonBreath should follow its filament-zone settings without
slicer-issued heater commands:

1. Select **Klipper / Moonraker** on the DragonBreath setup page and configure the
   printer's Moonraker address.
2. Configure the desired **Filament zones** in DragonBreath Settings.
3. Disable the active `dragonbreath-klipper`/printer-distribution chamber-heater
   configuration and restart Klipper. Leave DragonBreath's own Moonraker source
   enabled; the AUTO control becomes available when no active helper is detected.
4. In each Orca filament preset, leave the chamber temperature set if desired but
   clear **Activate temperature control**. Leave **Support controlling chamber
   temperature** enabled. Verify that sliced G-code contains no `M141` or `M191`.
5. Arm **AUTO** from the DragonBreath dashboard or front-panel Auto button.
   DragonBreath always boots OFF, so AUTO must be armed again after a reboot.

During an active print, DragonBreath reads the loaded material from Moonraker and
applies its matching filament-zone target. With no active print, no material, no
matching zone, or no Moonraker connection, AUTO waits with the heater off.

AUTO starts heating when the print becomes active, but it does not pause print
start until the chamber is hot. Use slicer/Klipper control when a blocking heat
soak is required.

## Worked example: Snapmaker U1 / PAXX Orca profile

This is one application of the common “start together, wait later” policy. It is
not a required DragonBreath sequence. The underscores below are normal G-code
underscores; do not type backslashes before them.

In **Printer settings → Machine G-code → Machine start G-code**, find:

```gcode
TIMELAPSE_START
M140 S{bed_temperature_initial_layer_single}
M104 T{initial_extruder} S140
```

Insert `M141` immediately after `M140`:

```gcode
TIMELAPSE_START
M140 S{bed_temperature_initial_layer_single}
M141 S{overall_chamber_temperature} ; start chamber, do not wait
M104 T{initial_extruder} S140
```

Later, find:

```gcode
M106 S255
M109 S{nozzle_temperature[initial_extruder] - 90}
M190 S{bed_temperature_initial_layer_single}
M107 P2
```

Insert `M191` immediately after `M190`:

```gcode
M106 S255
M109 S{nozzle_temperature[initial_extruder] - 90}
M190 S{bed_temperature_initial_layer_single}
M191 S{overall_chamber_temperature} ; wait for chamber; bed remains hot
M107 P2
```

Do not remove the profile's existing `WAIT_CHAMBER_TEMP TIMEOUT=180`. Add this as
the first line of Machine end G-code unless it is already present:

```gcode
M141 S0 ; chamber off
```

After slicing, verify that there is no `M191` before
`SET_PRINT_AUTO_BED_LEVELING`, that the early `M140`/`M141` and later
`M190`/`M191` pairs render in that order, and that both chamber commands contain
the same expected non-zero numeric target. If Orca rejects
`overall_chamber_temperature`, use `{chamber_temperature[0]}` in both places.

## Quick diagnosis

- **Chamber heats first; bed stays cold:** Orca probably injected `M191` before
  Machine start G-code. Inspect the generated file, not only the preset UI.
- **AUTO is not shown:** an active `[dragonbreath]` Klippy configuration was
  detected, so slicer/Klipper owns chamber heat. Disable it and restart Klipper if
  AUTO is the desired workflow.
- **AUTO is armed but not heating:** check that a print is active, Moonraker is
  connected, its active material is detected, and that material has a non-zero
  DragonBreath filament zone.
- **A rendered chamber command says `S0`:** set a non-zero chamber temperature in
  the filament preset, while keeping **Activate temperature control unchecked**.
- **`M191` never completes:** confirm the requested value does not exceed the
  device's configured maximum (hard ceiling 70 °C) and is realistically reachable
  in the enclosure.
- **Displayed chamber temperature seems wrong:** compare it with a trusted probe,
  then use the bounded sensor calibration in Settings. Do not calibrate by feel.
