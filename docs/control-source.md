# Control source — how DragonBreath decides who drives the heater

DragonBreath follows **exactly one controller at a time**. This is deliberate: the
device drives a **mains-powered heater**, and letting two systems set the target at
once ("Klipper says 60 °C, Home Assistant says 80 °C") is an ownership hazard, not a
feature. So there is always a single owner of the setpoint — never several.

## The sources

| Source | What it does |
|---|---|
| **Klipper (Moonraker)** | Follows the printer over the Moonraker WebSocket. AUTO mode triggers on the **bed setpoint**. The default, and the shipped path. |
| **Bambu (LAN)** | Follows a Bambu Lab printer's chamber/bed over LAN MQTT (read-only from the printer; DragonBreath still owns the heater). |
| **Home Assistant** | HA is the **controller** — a climate entity + sensors auto-appear via MQTT discovery, and HA sets target / on / off. |
| **None (unbound)** | No external controller. The heater is driven only from the DragonBreath web UI (or left idle). |

Only the **selected** source is connected. If you pick Klipper, the Bambu and HA
clients do not run at all — and vice-versa. In particular, **Home Assistant is
full-control only while it is the selected source.** When a printer (Klipper or
Bambu) is bound, HA is not connected — there is no background HA link.

### Klipper AUTO vs. the `dragonbreath-klipper` helper

Within the Klipper source the chamber can be driven two ways, and — by the same
one-owner rule — they must not both run:

- **AUTO** — DragonBreath follows the active print itself, over its own Moonraker
  connection.
- **The `dragonbreath-klipper` helper** — a Klipper module (`[dragonbreath]`) that
  commands the heater from `M141` / `M191` / `SET_HEATER_TEMPERATURE`. Installing its
  active configuration is itself choosing a manual controller (it even sends a
  safety OFF on connect, which used to silently disarm AUTO).

You no longer have to remember to turn one off. DragonBreath detects the helper on
Moonraker (its `[dragonbreath]` object) and **AUTO automatically defers to it**: AUTO
stays armed but does not drive the heater while the helper is installed (the dashboard
shows *"AUTO paused — Klipper helper is active"*). Remove the helper's active
`[dragonbreath]` configuration and AUTO resumes on the next tick. In short:

- **Want AUTO?** Don't install (or disable) the `dragonbreath-klipper` helper.
- **Want slicer / start-macro control?** Install the helper — AUTO steps aside on its own.

## Switching sources — and why there's an Unbind

Because only one source owns the heater, moving control from a printer to Home
Assistant (or to nothing) is a deliberate step. The **Unbind** button on the setup
page mirrors stock's "disconnect":

1. **Unbind** the current source — this clears that source's saved connection
   details and drops the control source to **None**.
2. Pick the new source (e.g. **Home Assistant**), enter its details, and save.

Unbinding a printer is how you "make HA the primary": once no printer is bound,
select Home Assistant and it becomes the sole controller with full control.

> **Why not run HA as a read-only monitor alongside Klipper/Bambu?** We considered
> it and chose not to: a second always-on connection — even read-only — muddies
> "who owns the heater" and invites exactly the ownership confusion the single-source
> rule exists to prevent. One source, one owner, no ambiguity.

## Safety note

Whatever the source, `pb_policy` remains the **sole** authority over mode/target and
enforces every heater cutoff (element/chamber over-temp, comms-loss watchdog, fan-
follows-heater). The control source only *requests* a setpoint; it can never defeat
the safety limits.
