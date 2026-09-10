# Control source — how DragonBreath decides who drives the heater

DragonBreath follows **exactly one controller at a time**. This is deliberate: the
device drives a **mains-powered heater**, and letting two systems set the target at
once ("Klipper says 60 °C, Home Assistant says 80 °C") is an ownership hazard, not a
feature. So there is always a single owner of the setpoint — never several.

## The sources

| Source | What it does |
|---|---|
| **Klipper (Moonraker)** | Follows the printer over the Moonraker WebSocket. AUTO follows the active print's DragonBreath filament-zone target. The default and shipped path. |
| **Bambu (LAN)** | Follows a Bambu Lab printer over LAN MQTT. AUTO follows the active print's DragonBreath filament-zone target (read-only from the printer; DragonBreath still owns the heater). |
| **Prusa (PrusaLink)** | Follows the printer over PrusaLink. Because PrusaLink does not report filament type, AUTO follows the bed setpoint and the configured bed threshold. |
| **Home Assistant** | HA is the **controller** — a climate entity + sensors auto-appear via MQTT discovery, and HA sets target / on / off. |
| **None (unbound)** | No external controller. The heater is driven only from the DragonBreath web UI (or left idle). |

Only the **selected** source has control. In particular, **Home Assistant is
full-control only while it is the selected source.** It may optionally publish
read-only telemetry alongside a printer source, but it cannot send heater commands
in that monitor role.

## Switching sources — and why there's an Unbind

Because only one source owns the heater, moving control from a printer to Home
Assistant (or to nothing) is a deliberate step. The **Unbind** button on the setup
page mirrors stock's "disconnect":

1. **Unbind** the current source — this clears that source's saved connection
   details and drops the control source to **None**.
2. Pick the new source (e.g. **Home Assistant**), enter its details, and save.

Unbinding a printer is how you "make HA the primary": once no printer is bound,
select Home Assistant and it becomes the sole controller with full control.

## Control source is not control mode

Selecting **Klipper / Moonraker** as the source does not mean that slicer commands
and AUTO can drive the target together. In Klipper source mode, choose either:

- DragonBreath AUTO, with no `M141`/`M191` heater commands in the sliced G-code; or
- manual slicer/Klipper control through `M141`, `M191`, or
  `SET_HEATER_TEMPERATURE`, with AUTO off.

The helper's Python file may stay installed, but its active `[dragonbreath]`
configuration is a manual controller and sends a safety OFF when it connects or
disconnects. Disable that configuration for a reliable AUTO workflow. Issuing one
of its heater commands with a positive target starts a manual POWER_ON session and
replaces AUTO. See
[`USING_THE_HEATER.md`](USING_THE_HEATER.md) for setup recipes and OrcaSlicer's
important command-ordering behavior.

Read-only Home Assistant telemetry does not create a second owner: the device does
not subscribe to HA command topics in monitor mode. One selected control source
still owns heater intent.

## Safety note

Whatever the source, `pb_policy` remains the **sole** authority over mode/target and
enforces every heater cutoff (element/chamber over-temp, comms-loss watchdog, fan-
follows-heater). The control source only *requests* a setpoint; it can never defeat
the safety limits.
