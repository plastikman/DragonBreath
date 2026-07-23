# Rebrand: OpenBreath → DragonBreath

> **Status:** proposal / RFC — sharing for input before execution.
> **Owner:** plastikman. Justin's review is advisory (esp. the OpenVent shared-core boundary — see *Input wanted*).

## Context
We're renaming the open chamber-heater project **OpenBreath → DragonBreath** (one word).
It's a cooler, on-the-nose name (a chamber heater that "breathes" hot air) and it clears up
a real naming problem: **"OpenBreath" is crowded** — it collides with the CERN open-hardware
*OpenBreath lung ventilator* and a Hackaday 3D-printed ventilator, so "OpenBreath" reads as
"open-source ventilator" to search engines. **"DragonBreath"** is unused in the
3D-printing / Klipper / chamber-heater space. (It does share a name with a niche Windows-malware
APT campaign, but that's irrelevant to a printer-firmware audience and was explicitly accepted.)

This affects three repos plus the PAXX firmware overlay:
- **`plastikman/OpenBreath`** — the ESP-IDF device firmware (`project(openbreath)`, ESP32-C3).
- **`plastikman/openbreath-klipper`** — the Klipper `extras` helper that drives the firmware's HTTP API.
- **PAXX overlay** (`SnapmakerU1-Extended-Firmware` fork) — installs the helper, ships the cfg, and
  provides the "Chamber Heater" settings dropdown.
- *Not affected:* `klipper-esp32` (a separate Klipper-MCU port) and the shared **OpenVent** core submodule.

## Decisions (locked with the owner)
- **Clean break** — no backward-compat aliases. There's no meaningful external OpenBreath user base
  yet, so existing `printer.cfg` users would edit config + reflash. Simpler and cleaner than shims.
- **Full scope** — firmware, Klipper helper, PAXX overlay, and the GitHub repos all rename.
- **Rename reach = everything, including network identity** — rename the WiFi AP SSID and override the
  mDNS hostname (details below). Keep **"Panda Breath"** only as the underlying BIQU *hardware*
  descriptor. **Do not rebrand the shared OpenVent submodule.**
- **Migration = USB reflash** — the firmware's OTA image-identity gate rejects a cross-brand OTA, so
  the first DragonBreath image is flashed over USB; OTA works normally afterward.
- **Naming convention** — `DragonBreath` for display/class names; lowercase `dragonbreath` for config
  keys, filenames, identifiers, and the hostname (mirrors today's lowercase `openbreath`).

## ⭐ Input wanted from Justin — the OpenVent shared-core boundary
The firmware sits on the shared **OpenVent** core (`external/OpenVent/…`, `pv_wifi`/`pv_evlog`/
`pv_moonraker`). Two "everything" rename items touch that boundary, and we want to keep them
**upstream-friendly** (no edits to the submodule):
1. **mDNS hostname.** Today it inherits `PV_WIFI_HOSTNAME = "OpenVent"` → `OpenVent.local`. Plan is to
   override it to `dragonbreath` **from the app layer** (`main/app_main.c`, right where the SSID prefix
   is already overridden), leaving `external/OpenVent/.../pv_wifi.h` untouched. **Is app-layer override
   the intended extension point, or should the core expose a cleaner hostname-override API?**
2. **WiFi AP SSID.** Today `OpenPanda_XXXX` (an app-layer override of the core's `OpenVent_` default).
   Plan changes it to `DragonBreath_XXXX`. Same mechanism — flagging in case you'd prefer a
   core-level convention.

Everything else below is app/product-level and doesn't touch OpenVent.

## Wire contract — must flip in lockstep (firmware + helper)
- **HTTP auth header** `X-OpenBreath-Auth` → `X-DragonBreath-Auth` — firmware `PB_AUTH_HEADER`
  (`components/pb_httpd/include/pb_httpd.h`), the portal JS + its `ob_tok` localStorage key
  (`pb_portal.c`), and the Klipper helper all validate/send it. Change together or auth breaks.
- **Klipper module name** `openbreath` → `dragonbreath` drives the config section `[dragonbreath]`,
  `sensor_type: dragonbreath`, the `heater_pin: dragonbreath:pwm` chip, and the overlay cfg + settings
  state key — these move as one unit.
- **Brand-neutral — deliberately unchanged:** HTTP route paths (`/status /target /heartbeat /reset
  /update`), JSON field names, and the NVS namespace `app_nvs` + keys (`ctl_token`, …). Because no NVS
  key is named after the project, **the rename does NOT wipe device settings.**

## Footprint by repo

### 1. Device firmware (`OpenBreath` → repo `DragonBreath`)
- **Build identity:** `CMakeLists.txt` `project(openbreath)` → `project(dragonbreath)`; banner/version vars `OB_*` → `DB_*`.
- **OTA identity gate (why USB reflash is needed):** `pb_httpd.c` rejects any uploaded image whose
  `project_name != "openbreath"`. Flip to `"dragonbreath"`; deployed units can't OTA across the rename.
- **Image name:** `openbreath.bin` → `dragonbreath.bin` (`tools/flash.py`, portal OTA copy, README).
- **Auth header / SSID / hostname:** as in the wire-contract + shared-core sections above.
- **Cosmetic:** log tag + boot logs, web-UI OpenBreath strings and title (`OpenPanda` → `DragonBreath`,
  keeping the "Panda Breath" hardware subtitle), `docs/`, `plans/`, README prose, repo URLs.

### 2. Klipper helper (`openbreath-klipper` → `dragonbreath-klipper`)
- git-rename `openbreath.py` → `dragonbreath.py` (⚠ the filename *is* the Klipper config-section name),
  `config/openbreath.cfg` → `dragonbreath.cfg`, and the repo.
- In the module: class names `OpenBreath*`/`_OpenBreathHTTP` → `DragonBreath*`; the sensor-factory and
  pin-chip registration strings; `OPENBREATH_RESET` → `DRAGONBREATH_RESET`; the auth header; log prefixes.
- cfg / README / `install.sh`: section names, `sensor_type`, `heater_pin`, `[heater_generic dragonbreath]`,
  `HEATER=dragonbreath`, clone URL/paths, Moonraker `[update_manager dragonbreath-klipper]`, example host
  `dragonbreath.local`.
- Tests (`tests/test_async_transport.py`) update imports; re-run (9/9, incl. `-W error::ResourceWarning`).

### 3. PAXX overlay (`37-feature-chamber-heater/`)
- git-rename the install script + the two shipped cfg fragments to `dragonbreath*`.
- install script: point `GIT_URL` at the renamed helper repo and **bump `GIT_SHA`** to its new main HEAD;
  update install paths + echoes.
- cfg fragments: all `[dragonbreath]` / `heater_generic` / `heater_pin` / `sensor_type` / `verify_heater`
  / `HEATER=` / `SENSOR=` refs + the `X-DragonBreath-Auth` comment + example host.
- Settings YAML (`25_settings_snapmaker_components_chamber_heater.yaml`): keep the 3-option dropdown
  (**Disabled / Panda Auto / DragonBreath**); rename the option key, get_cmd state key + `.cfg` detection,
  `OPENBREATH_IP` → `DRAGONBREATH_IP`, the config-writer section arg, `help_url`, and all display strings.

### 4. GitHub repos/org
- Rename `plastikman/openbreath-klipper` → `dragonbreath-klipper` and `plastikman/OpenBreath` →
  `DragonBreath` (GitHub keeps redirects). Update every cross-link (install-script `GIT_URL`, READMEs,
  settings `help_url`, `update_manager` origin).

## Sequencing (avoids a broken-build window)
1. Rename the **helper repo** (files + tokens), tests green, merge → capture new main SHA.
2. Rename the **firmware** (project/OTA gate/header/SSID/hostname/UI), build.
3. **USB-reflash** the device; confirm boot + `dragonbreath.local`.
4. Update the **PAXX overlay** (new `GIT_URL` + `GIT_SHA`, cfg/YAML) on a branch; build via CI.
5. Do the **GitHub repo renames** (redirects cover stragglers).
6. Flash the new PAXX firmware to the U1; verify end-to-end.

## Verification
- Helper: `python3 tests/test_async_transport.py` (9/9) + `py_compile`.
- Firmware: builds; after USB flash, `dragonbreath.local` resolves, web UI shows DragonBreath, and the
  OTA gate now accepts `dragonbreath` images.
- End-to-end on the U1: `[dragonbreath]` loads, `SET_HEATER_TEMPERATURE HEATER=dragonbreath` drives the
  device, the helper authenticates with `X-DragonBreath-Auth`, Fluidd shows the DragonBreath heater.
  *(Reminder: reloading a changed `extras/*.py` needs a klippy **process** restart, not FIRMWARE_RESTART.)*

## Out of scope
- The shared **OpenVent** submodule and the "Panda Breath" hardware descriptor.
- The separate device-side Wi-Fi/HTTP "flap" bug (tracked independently).
- The chamber-heater dropdown structure is unchanged (Disabled / Panda Auto / DragonBreath; MQTT already dropped).
