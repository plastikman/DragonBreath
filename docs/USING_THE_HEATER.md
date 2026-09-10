# Using the chamber heater

DragonBreath does not decide how a print should warm up. It carries out the
workflow selected by the user, slicer, or printer configuration. In particular,
it does **not** automatically turn on the bed when the chamber heater starts.

For Klipper, choose **one** of these workflows:

| Workflow | Who chooses the chamber target? | Does the print wait for it? |
|---|---|---|
| [DragonBreath AUTO](#workflow-1-dragonbreath-auto) | DragonBreath's filament-zone settings | No |
| [Slicer / Klipper control](#workflow-2-slicer--klipper-control) | Orca or a Klipper start macro | Yes, when the macro uses `M191` |

Do not combine them. `M141`, `M191`, and
`SET_HEATER_TEMPERATURE HEATER=dragonbreath ...` are manual Klipper commands.
Sending a positive target replaces AUTO with a manual `POWER_ON` session; sending
zero turns the heater off. Two independent pieces of software cannot own the
target at the same time.

The `dragonbreath-klipper` Python file may remain installed in either workflow,
but its active `[dragonbreath]` configuration is itself a manual controller. It
sends a safety OFF when it connects, disconnects, or Klippy shuts down, so a
reconnect can disarm AUTO even if the slicer sends no heater command. For a
reliable AUTO workflow, disable the active helper configuration as well as its
G-code commands. DragonBreath's direct Moonraker connection supplies AUTO with
printer state; the Klippy helper is not required for that path.

## The OrcaSlicer trap

When both **Support control chamber temperature** (printer preset) and
**Activate temperature control** (filament preset) are enabled, OrcaSlicer emits
this before the printer's Machine start G-code:

```gcode
M191 S<chamber-temperature>
```

`M191` means **set the chamber target and wait until the chamber reaches it**.
Because Orca places it before Machine start G-code, the usual bed command has not
run yet. The chamber therefore warms without help from the bed, and the rest of
the start sequence appears stuck. This is
[Orca's documented command ordering](https://github.com/OrcaSlicer/OrcaSlicer/wiki/material_temperatures#print-chamber-temperature),
not a DragonBreath fault.

After changing a preset, slice a small model and inspect the first lines of the
generated G-code. Preset inheritance can leave the chamber option enabled in a
derived filament or printer profile.

### PAXX does not choose a warm-up workflow

Enabling the DragonBreath/Panda Breath component in PAXX installs the Klipper
integration that makes `M141`, `M191`, and the `heater_generic` object work. It
cannot safely rewrite each user's Orca presets or decide whether their bed, axes,
fans, and chamber should preheat sequentially or together. PAXX users must still
choose one of the workflows below and configure their slicer/start macro to match.

## Workflow 1: DragonBreath AUTO

Use this when you want DragonBreath to follow the active filament without the
slicer directly controlling the heater.

1. On the DragonBreath setup page, select **Klipper / Moonraker** and configure
   the printer's Moonraker address.
2. In DragonBreath Settings, configure the desired **Filament zones**.
3. Arm **AUTO** from the DragonBreath dashboard or front-panel Auto button.
   DragonBreath always boots OFF, so AUTO must be armed again after a reboot.
4. Disable the active `dragonbreath-klipper`/PAXX chamber-heater integration. The
   device's own Moonraker control source remains enabled and supplies AUTO data.
5. In Orca, disable automatic chamber-temperature control so it does not emit
   `M191` at the start or `M141` at the end. Verify the generated G-code contains
   neither command.

During an active print, DragonBreath reads the loaded material from Moonraker
and applies its matching filament-zone target. With no active print, no material,
no matching zone, or no Moonraker connection, AUTO waits with the heater off.

AUTO starts heating when the print becomes active, but it does **not** pause the
print-start sequence until the chamber is hot. Use slicer/Klipper control if the
print must heat-soak before extrusion begins.

## Workflow 2: Slicer / Klipper control

Use this when the print-start sequence must coordinate the bed and chamber or
must wait for a heat soak. Leave DragonBreath AUTO off. `M141` starts chamber
heating without waiting; `M191` starts it and blocks until the target is reached.

Do not use Orca's automatically injected `M191` if you want the bed and chamber
to warm together. Disable that option and pass the chamber target into your
existing `PRINT_START` macro instead. Parameter names vary between printer
profiles, but the macro's heating phase should have this shape:

```ini
[gcode_macro CHAMBER_PREHEAT]
description: Start bed and DragonBreath together, then wait for both
gcode:
    {% set bed = params.BED|default(0)|float %}
    {% set chamber = params.CHAMBER|default(0)|float %}
    M140 S{bed}          ; start the bed; do not wait
    M141 S{chamber}      ; start DragonBreath; do not wait
    {% if bed > 0 %}
        M190 S{bed}      ; wait for the bed
    {% endif %}
    {% if chamber > 0 %}
        M191 S{chamber}  ; wait for the chamber; bed stays hot and soaking
    {% endif %}
```

Call it from the printer's start macro with the bed and chamber temperatures
supplied by the slicer. If the existing `PRINT_START` already starts or waits for
the bed, merge these four commands into that heating phase instead of heating the
bed twice.

This sequence starts both heaters together, waits for the bed, and then keeps the
bed at temperature while the chamber finishes. Bed position, homing, circulation
fans, and any extra timed soak are printer-specific decisions and belong in the
printer's start macro, not in the generic `M141`/`M191` definitions.

At print end, explicitly stop the chamber with `M141 S0` unless the printer's end
macro already does so.

## Quick diagnosis

- **Chamber heats first; bed stays cold:** Orca probably injected `M191` before
  Machine start G-code. Inspect the generated file, not only the preset UI.
- **AUTO changes to On/Manual at print start:** the G-code sent `M141`, `M191`, or
  `SET_HEATER_TEMPERATURE`. Remove those commands for the AUTO workflow.
- **AUTO is armed but not heating:** check that a print is active, Moonraker is
  connected, its active material is detected, and that material has a non-zero
  DragonBreath filament zone.
- **`M191` never completes:** confirm the requested value does not exceed the
  device's configured maximum (hard ceiling 70 °C) and is realistically reachable
  in the enclosure.
- **Displayed chamber temperature seems wrong:** compare it with a trusted probe,
  then use the bounded sensor calibration in Settings. Do not calibrate by feel.

DragonBreath remains responsible for its independent sensor checks, target clamp,
element foldback, over-temperature shutdown, cooldown airflow, and communications
watchdog in both workflows.
