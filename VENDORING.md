# Shared-component provenance

DragonBreath consumes its board-neutral services from
[`justinh-rahb/dragon-core`](https://github.com/justinh-rahb/dragon-core) through
ESP-IDF Component Manager. The exact revision is pinned for every component in
[`main/idf_component.yml`](main/idf_component.yml); the source is no longer copied
into DragonBreath's `components/` directory.

## Current shared components

| Component | Origin | Purpose |
|---|---|---|
| `dc_evlog` | OpenVent `pv_evlog` via DragonBreath `pb_evlog` | in-memory event and console rings |
| `dc_wifi` | OpenVent `pv_wifi` via DragonBreath `pb_wifi` | Wi-Fi STA/AP provisioning, scanning, and mDNS |
| `dc_moonraker` | OpenVent `pv_moonraker` via DragonBreath `pb_moonraker` | Moonraker WebSocket client |
| `dc_source` | DragonBreath `pb_source` | persisted control-source selection |
| `dc_bambu` | DragonBreath `pb_bambu` | Bambu LAN MQTT client and printer status |
| `dc_ui` | DragonBreath dashboard SPA | embedded family UI asset and capability gating |

DragonBreath retains its product-specific board, sensor, actuator, safety-policy,
HTTP API, setup/OTA portal, LED, button, Home Assistant, and HIL components.

## OpenVent lineage

The first three components originated in
[`justinh-rahb/OpenVent`](https://github.com/justinh-rahb/OpenVent) at commit
`ec4691f8d7fe95be8e3c6af4cac35d4992b08c79` (`v0.3.0-4-gec4691f`). DragonBreath
initially consumed them through a submodule, then copied them locally in v0.6.1 and
renamed their `pv_*` APIs to `pb_*`. The current extraction moves those copies, plus
the DragonBreath-originated source selector and Bambu client, into `dragon-core` and
renames their public APIs to the product-neutral `dc_*` namespace.

`dc_wifi` additionally exposes a product identity API so each consumer supplies its
own hostname, mDNS instance name, and provisioning-AP branding instead of applying
post-start overrides.

## License

OpenVent and dragon-core are MIT licensed. The OpenVent attribution remains part of
this provenance record even though the components have moved repositories and their
symbols have changed. See each upstream repository's `LICENSE` file for the license
text.
