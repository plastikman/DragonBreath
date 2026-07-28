# OEM feature parity

DragonBreath is not intended to reproduce the stock firmware byte-for-byte. This
matrix records which user-visible Panda Breath behaviors are implemented, still
planned, deliberately changed for safety, or intentionally omitted.

Current as of **v0.7.2** (drying validated on hardware 2026-07-27; fan-only filtration added 2026-07-28; web UI reworked — no brand header, per-card intros, light/dark, desktop keeps full layout / phones stack).

| Stock/OEM behavior | DragonBreath status | Notes |
|---|---|---|
| Manual chamber target | **Implemented** | Local Web UI and Klipper `M141`/`M191`; device-side regulation and limits remain authoritative. |
| Follow-printer-bed automatic mode | **Partial** | Policy/API support is present: live Moonraker bed temperature, 3 °C disengage hysteresis, and fail-off on disconnect. The shipped dashboard control is not yet accepted as end-to-end validated. |
| Timed filament drying | **Implemented** | A target plus a bounded 1–12 hour duration with automatic shutoff, driven from the dashboard Dry screen and API v2 (`drying_start` / `drying_stop`). Material presets are implemented (PLA 45 °C/6 h, PETG 65 °C/4 h, ABS 70 °C/4 h, Nylon 55 °C/8 h — one tap pre-fills target + duration). Validated on hardware. |
| Chamber and PTC temperature display | **Implemented** | Both sensors and their health are exposed in the dashboard and API v2 state. |
| Sensor-fault and over-temperature shutdown | **Implemented** | Heater fails closed; fixed 85 °C chamber and 105 °C PTC cutoffs are not user-configurable. |
| Fan follows heater | **Implemented** | TRIAC is held on/off and never phase-angle PWM'd. |
| Residual-heat fan purge | **Implemented** | Session-gated cooldown with hysteresis around the configurable `cool_release` temp (30–65 °C, default 40 °C): engages one 3 °C band above it, releases only once **both** chamber and PTC are below it; an unknown sensor keeps the fan on. Strictly session-gated: the fan never starts on temperature alone, and a power-cycle-while-hot does not spin it. The opt-in temperature-latched policy was **intentionally not added** for that reason. Fault airflow remains unconditional. |
| Front-panel mode LEDs | **Implemented** | All four outputs are driven from the authoritative policy snapshot: Power is solid whenever the device is up and blinks on fault; On, Auto, and Dry each light for their own mode. Auto slow-blinks when armed but not engaged (no Moonraker link, or bed below the threshold). Power is release-only — GPIO21 is also the serial console TX. |
| Front-panel buttons | **Implemented** | All four (Power GPIO9, Auto GPIO8, On GPIO10, Dry GPIO2) are polled with debounce and short/long-press detection. A short press toggles the button's labeled mode (arming from the remembered parameters); a 2 s long-press latches a panic-off, except a long-press on Power while faulted attempts a fault clear. Every button action is attributed to the panel and invalidates any remote lease. A button held at power-on is ignored until released (GPIO9/8/2 are strapping pins). |
| Local status/configuration UI | **Implemented** | Responsive, touch-first single-page dashboard with manual/automatic/drying control screens, a settings screen (safety limits, sensor calibration, LEDs), an at-a-glance status panel (fan + reason, controller, auto/drying, printer, fault), and setup/OTA pages. Light/dark themes. |
| Wi-Fi captive setup and mDNS | **Implemented** | Product identity is DragonBreath; reachable as `dragonbreath.local` when mDNS works. |
| Firmware update | **Implemented** | Like stock, DragonBreath accepts a local firmware upload from its Web UI. DragonBreath additionally authenticates the write, verifies project identity, refuses updates while heating, and uses dual-slot rollback. |
| Printer integration | **Implemented for Klipper** | Both stock and DragonBreath can read Klipper bed state through Moonraker. DragonBreath also exposes heater control through its Klipper helper/API v2. Stock additionally supports Bambu MQTT and Home Assistant MQTT; DragonBreath intentionally focuses on Moonraker/Klipper. |
| Home Assistant MQTT discovery | **Intentionally omitted** | DragonBreath uses HTTP/JSON plus Server-Sent Events; no MQTT broker is required. |
| Stock WebSocket API and Web UI | **Intentionally omitted** | API v2 and the DragonBreath dashboard are the supported interfaces. |
| Boot/resume active heating | **Intentionally changed** | DragonBreath always boots OFF and never restores active heat, timers, or leases after reboot. |
| Persistent fault latch | **Implemented** | Hazard-driven trips (over-temp, sensor fault, comms-loss) are persisted to NVS on the latching transition and restored at boot, so a device that tripped before losing power comes back up heater-OFF with commands inhibited. Boot restore is fail-safe (unreadable store → latched); a failed persist is retried until it commits; clearing is persist-first (HTTP 500 rather than a false "cleared"). User panic-off and the permanent inhibit are deliberately not persisted (clear on reboot). |
| Fan-only filtration mode | **Implemented** | Stock runs the blower alone as the low band of AUTO (bed ≥ `filter_temp`, before the heater engages). DragonBreath reproduces this as a configurable AUTO fan-only band (default on, `filter_temp` default 30 °C) and additionally exposes a mode-independent manual filtration fan (dashboard toggle + `filter` API command). Fan-only in both cases — the heater is never engaged by filtration. |

Safety takes precedence over exact OEM behavior. See [SAFETY.md](SAFETY.md) for
the enforced invariants and
[the iteration-2 plan](../plans/iteration-2-stock-parity-and-config.md) for
remaining implementation work.
