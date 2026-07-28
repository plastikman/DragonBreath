# RFC: Pluggable control source — Klipper · Bambu · Home Assistant

Status: **Draft / seeking testers.** Klipper is shipped; Bambu and Home
Assistant are proposed parity add-ons that must be built and validated by the
community, because the maintainer has no Bambu printer or HA broker to test
against.

## Goal

DragonBreath binds to **exactly one** control source at a time — never more than
one. Today that source is hard-wired to Klipper (Moonraker). This RFC generalizes
it to a **single, user-selected control source**:

| Source | Role | Status |
|---|---|---|
| **Klipper** (Moonraker) | Follow bed temp for AUTO; `M141`/`M191` + `output_pin` via the host helper | **Shipped** — the real target |
| **Bambu** | Follow the printer's bed temp over Bambu LAN MQTT (read-only) | **Proposed** (parity) |
| **Home Assistant** | HA is the controller: it sends target/mode over MQTT and reads back state | **Proposed** (parity) |

Only one is active per boot. This mirrors stock Panda Breath, which binds to a
single printer/controller. It is **not** "Bambu-in + HA-out simultaneously."

Non-goals: running two sources at once; sending *control* commands to a Bambu
printer (we only read status); a Bambu cloud path (LAN only).

## Why this is safe to ship untested behind a selector

The heater safety envelope is **independent of the control source**. Every cutoff
lives in `pb_heater`/`pb_policy` and runs regardless of who set the target:

- PTC 105 °C / chamber 85 °C fixed cutoffs, element foldback limiter, sensor
  fail-closed, comms-loss watchdog, boot-OFF, single-SSR-writer, `fb_cut`, the
  70 °C target ceiling — none of these consult the source.

A control source's *entire* job is to produce two numbers for AUTO
(`bed_c`, `connected`) and/or submit ordinary policy commands that any client
could already submit. So an untested Bambu/HA path can, at worst, mean **"AUTO
doesn't engage/track correctly"** or **"a command is missed"** — never unsafe
heating. That bound is what makes it defensible to ship these behind an
explicit, non-default selector and recruit testers.

Klipper stays the default. Selecting Bambu/HA is opt-in and labeled experimental.

## Current architecture (the seam we build on)

The whole "follow the printer" feature funnels through two values, fed once per
~500 ms control loop in `main/app_main.c`:

```c
// app_main.c ~194-198  (inside #ifndef CONFIG_PB_HIL_DEVBOARD)
if (s_net_up && s_mk_up) pb_moonraker_get_status(&st);
bool mk_connected = s_mk_up && st.state == PB_MK_SUBSCRIBED;
pb_policy_set_env(st.bed_temp, mk_connected);   // <-- the seam
```

`pb_policy` consumes only `bed_c` (float °C) and `connected` (bool). AUTO
engage/disengage hysteresis, LEDs, fail-off-on-disconnect, and the HTTP
`environment.moonraker_connected` field all derive from those. **A new source
plugs in by feeding this same seam** — nothing downstream changes.

Config plumbing today (to mirror): captive-portal `/setup` form →
`POST /save` (`components/pb_portal/pb_portal.c`, `save_post()`) → NVS namespace
`app_nvs`, keys `mk_host`/`mk_port` → `pb_moonraker_start()` reads NVS at boot.
There is no runtime settings path and no source selector yet.

## Proposed design

### 1. Control-source selector

New NVS key in `app_nvs`: `ctl_src` (u8) — `0=klipper` (default), `1=bambu`,
`2=ha`. On boot `app_main` starts **only** the selected client. `/setup` gains a
source `<select>`; the form reveals only that source's fields (small JS toggle).
Factory reset clears `ctl_src` back to Klipper.

`app_main` control loop generalizes the seam:

```c
float bed_c; bool connected;
pb_source_get_env(&bed_c, &connected);   // dispatches on ctl_src
pb_policy_set_env(bed_c, connected);
```

where `pb_source_*` is a thin dispatch over the one active client. Each client
keeps the `pb_moonraker`-style lifecycle API
(`start`/`set_config`/`get_config`/`get_status`/`clear_config`) for symmetry.

### 2. Bambu source — new `pb_bambu` component

MQTT-over-TLS client to the printer itself (the printer is the broker in LAN
mode). Mirrors `pb_moonraker`'s shape; uses ESP-IDF's bundled `esp-mqtt` +
`mbedTLS` (no new dependency).

Connection (all confirmed against OpenBambuAPI + ha-bambulab):

| Item | Value |
|---|---|
| URI | `mqtts://{printer_ip}:8883` (printer is its own broker in LAN mode) |
| Username | `bblp` (literal) |
| Password | printer **LAN Access Code** |
| TLS | self-signed cert, `CN = serial` — connect by IP, so **`skip_cert_common_name_check = true`**; bundle Bambu CA or run insecure on-LAN |
| Subscribe | `device/{serial}/report` |
| Request | `device/{serial}/request` |
| On connect | publish once: `{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}` (**required** on P1/A1 which send deltas; harmless on X1). Never re-`pushall` more than every 5 min on P1. |

Report fields we read (top-level of the `print` object):

- `bed_temper` (°C float) — **the follow signal; universal across all models**
- `gcode_state` — `IDLE`/`PREPARE`/`RUNNING`/`PAUSE`/`FINISH`/`FAILED` (note
  `FINISH`, not `FINISHED`) → maps to the six-state printer model
- `chamber_temper` — informational; **only X1/X1C/X1E/H2/P2-class** report it.
  On H2/newer it moved to a packed nested field
  `device.ctc.info.temp` (`cur = v & 0xFFFF`, `tgt = (v>>16) & 0xFFFF`) — parse
  both forms. P1/A1 have no chamber sensor (we follow the bed anyway).

RAM discipline on the C3 (~400 KB SRAM): a full `pushall` report is **10–15 KB+**.
Do **not** cJSON-parse the whole blob (raw buffer + parse tree ≈ double RAM).
Use a **targeted/streaming scan** (jsmn or hand-rolled) that extracts just
`bed_temper` / `gcode_state` / `chamber_temper`. Tune `sdkconfig`: keep
`MBEDTLS_SSL_IN_CONTENT_LEN` at 16384 if the printer needs it, shrink
`MBEDTLS_SSL_OUT_CONTENT_LEN` to ~2048 (we send tiny commands). Measure
`esp_get_free_heap_size()` around the TLS handshake on real hardware to finalize
buffers.

New NVS keys (`app_nvs`): `bb_host`, `bb_serial`, `bb_code`. `/setup` gains those
three fields when Bambu is selected.

### 3. Home Assistant source — new `pb_ha` component

Plain MQTT client to the **user's** broker (Mosquitto/HA add-on) — HA is the
controller. Two directions on one connection:

- **Subscribe** a command topic → translate HA messages into ordinary
  `pb_policy` commands (power_on/target, off, mode). These are the same commands
  a web/klipper client already issues, so no new authority is granted.
- **Publish** a retained state topic — the flat schema stock already uses, which
  we captured from a live unit:
  ```json
  {"chamber_temp":24,"work_on":"ON","mode":"power on","target_temp":45,
   "filter_temp":30,"heater_temp":80,"drying_remaining_min":720,
   "drying_running":"OFF","printer_ip":"...","printer_name":"...","printer_bind":"bind"}
  ```
  Optionally emit HA MQTT-discovery config so entities auto-appear.

New NVS keys: `ha_host`, `ha_port`, `ha_user`, `ha_pass`, `ha_topic` (prefix).
`/setup` reveals these when HA is selected.

Note: HA does **not** feed the AUTO bed-follow seam (there's no printer bed to
follow); in HA mode `pb_source_get_env` reports `connected=false`/no bed, and HA
drives target/mode directly. That asymmetry is intentional and matches stock.

### Shared MQTT plumbing

Bambu and HA both need an MQTT client; only one runs at a time. Options: a thin
internal `pb_mqtt` helper both call, or two independent components. Recommend
starting with two components sharing a small static TLS/connect helper, to keep
Bambu's TLS quirks out of the plain-broker HA path. Decide during implementation.

## Implementation phases

1. **Source selector scaffolding** — `ctl_src` NVS key, `pb_source_*` dispatch in
   `app_main`, `/setup` `<select>` + field-reveal JS, factory-reset handling.
   Klipper remains default; behavior byte-identical when `ctl_src=0`.
2. **`pb_bambu`** — esp-mqtt/TLS client, `pushall` on connect, streaming scan for
   `bed_temper`/`gcode_state`/`chamber_temper`, feed the seam, reconnect+backoff.
   `sdkconfig` TLS buffer tuning.
3. **`pb_ha`** — broker client, command-topic → policy commands, retained state
   publish, optional MQTT discovery.
4. **Docs** — `/setup` help, `FEATURES.md`/`OEM_PARITY.md` (Bambu/HA rows flip to
   "Implemented (community-tested)" only after real-hardware confirmation),
   per-source setup instructions.

Phases 1–3 are buildable and CI-verifiable (compile + host tests) **blind**.
None can be marked validated without a tester.

## What a tester needs to do

**Bambu:** a printer in **LAN Only Mode**; note the **Access Code** and
**serial**; give the printer a static DHCP lease. Select "Bambu" in `/setup`,
enter IP + serial + code. Verify: connects, `bed_temper` shows in the dashboard,
AUTO engages when the bed crosses the threshold, and it fails safe (heater off)
when the printer/Wi-Fi drops. Report model + firmware version. Known gotchas to
watch: **single-client limit** on P1/A1 (only one LAN MQTT client — HA/Bambu
Studio/Handy will contend), and the 2025 authorization-control firmware (status
*read* still works with access code; we never send control commands).

**HA:** an MQTT broker; select "Home Assistant", enter broker + creds. Verify
state topic publishes, HA can command target/mode, entities appear.

## Open questions

- Bambu TLS: bundle the Bambu CA (more correct) vs `skip-verify` on-LAN
  (simplest)? Lean CA-bundle + `skip_cert_common_name_check`; needs the CA PEM.
- Streaming JSON: adopt `jsmn` (adds a small dep) vs a hand-rolled field scan?
- HA command schema: match stock's exact topic/payload for drop-in parity, or
  define a clean DragonBreath schema + discovery? (Affects existing stock-HA
  users migrating.)
- Selector UX: `/setup` `<select>` (proposed) vs a compile-time Kconfig per
  build. Runtime selector is friendlier for a single binary.

## Sources

Bambu LAN protocol (topics, `pushall`, fields, ports, `bblp`/access-code, TLS
CN=serial, model/chamber matrix, single-client limit, 2025 auth control):
OpenBambuAPI (`mqtt.md`, `tls.md`) and Home Assistant `ha-bambulab`
(`pybambu/bambu_client.py`, `pybambu/models.py`). HA state schema: captured from a
live stock v1.0.4 unit.
