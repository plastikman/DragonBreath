# RFC: Dragon firmware family unification

Status: **Accepted; implementation in progress.** Updated after the initial
dragon-core extraction landed in DragonBreath via
[#68](https://github.com/plastikman/DragonBreath/pull/68).

The Dragon family uses one board-neutral ESP-IDF core and thin product firmware
repositories. Products own their hardware and safety behavior; dragon-core owns
services and transports that can be reused without product-specific GPIO, sensor,
or actuator assumptions.

## What has landed

As of 2026-08-07:

- [`justinh-rahb/dragon-core`](https://github.com/justinh-rahb/dragon-core) exists and
  uses the family-neutral `dc_` / `DC_` namespace.
- DragonBreath #68 extracted `dc_evlog`, `dc_source`, `dc_bambu`, `dc_wifi`, and
  `dc_moonraker`. The shared-SPA follow-on adds `dc_ui`; DragonBreath consumes all six
  through ESP-IDF Component Manager manifests pinned to one dragon-core revision.
- The extraction preserved NVS namespaces, keys, and persisted enum values. Existing
  Wi-Fi, Moonraker, Bambu, and control-source configuration survives the upgrade.
- DragonBreath's product-local policy command-origin enum now uses `db_source_t` and
  `DB_SOURCE_*`, keeping it distinct from dragon-core's `dc_ctl_source_t` and
  `DC_SRC_*` control-source selector.
- DragonBreath #68 passed host tests, its ESP-IDF CI build, and the complete safe
  ESP32-C3 devboard HIL suite: 3 scenarios, 66 steps, 0 failures, with Panda mains
  GPIO/ADC backends compiled out.
- dragon-core has annotated `v0.1.0` and `v0.1.1` tags. Its CI runs the Bambu parser
  and family-SPA checks and compiles/links every shared component together with
  ESP-IDF 5.3 for ESP32-C3.
- The shared-SPA follow-on adds `dc_ui`: dragon-core owns the editable HTML and
  reproducible gzip asset, while DragonBreath keeps its HTTP, setup, OTA, and product
  policy handlers. `/api/v2/info` now supplies an additive UI schema/product descriptor,
  and the SPA gates optional Manual/Auto/Dry surfaces from runtime capabilities.

The first extraction did **not** require HTTP or UI decoupling. That work remains the
prerequisite for moving the HTTP/OTA/portal/shared-SPA layer into dragon-core; see
[the follow-on decoupling RFC](decouple-httpd-device-interface.md).

## Namespace and compatibility rules

| Ownership | C/component namespace | Status |
|---|---|---|
| dragon-core shared code | `dc_*` / `DC_*` | Active |
| DragonBreath product code | `db_*` / `DB_*` | Target convention |
| DragonVent product code | `dv_*` / `DV_*` | Target convention |
| Existing DragonBreath `pb_*` / `PB_*` | Transitional legacy | Migrate in focused follow-ups |

New DragonBreath components use `db_*`; existing `pb_*` components are not precedent
for new names. In particular, new app-side Klipper MQTT work should use
`db_klipper_mqtt`, while an independently reusable transport would be a `dc_*`
component.

The source namespace may change, but compatibility-sensitive identifiers do not
change merely for naming consistency. Preserve these unless an explicit migration is
designed and tested:

- NVS namespaces, keys, and numeric enum values;
- HTTP routes, JSON fields, auth headers, and other wire contracts;
- partition labels and OTA/project identities;
- persisted configuration strings and externally consumed names.

The remaining `pb_*` to `db_*` source rename should be mechanical and isolated from
behavior changes. Compatibility exceptions are intentional, documented exceptions to
the source-prefix rule.

## Architecture

### Shared core

The landed core owns:

| Component | Responsibility |
|---|---|
| `dc_evlog` | Event and diagnostic-console rings |
| `dc_source` | Persisted external-control source selection |
| `dc_bambu` | Bambu LAN MQTT status client |
| `dc_wifi` | Wi-Fi, provisioning, scanning, and mDNS with product identity input |
| `dc_moonraker` | Moonraker WebSocket client and Klipper status |
| `dc_ui` | Embedded family SPA asset and capability-aware browser shell |

Candidate follow-on modules include HTTP/OTA, the portal and shared SPA, discovery and
group management, and reusable RGB LED behavior. A candidate moves to core only after
its product hardware and policy dependencies are behind a narrow capability boundary.

### Products

Products retain board maps, sensors, actuators, safety policy, and product composition:

```text
dragon-core (dc_*)                 product repositories
  transport and service code        DragonBreath: db_* target; legacy pb_* remains
  no product GPIO assumptions        DragonVent:   dv_*
  no heater/motor policy              DragonStatus: product/controller-board layer
```

The proposed `dc_device` capability boundary is a follow-on seam, not something #68
already implemented. DragonBreath would provide a `db_device` implementation;
DragonVent and DragonStatus would provide their own product implementations.

## Shared web client and same-LAN management

The UI is already a browser-hosted SPA. Today it uses HTTP requests plus server-sent
events; the firmware's Moonraker client, not the browser UI, is the current WebSocket
consumer. When the UI becomes the family SPA, the bundle can carry sibling-product
screens once and select them from device capabilities; no native mobile app is
required.

The management-plane follow-on is intentionally same-LAN and opt-in:

1. A user enables sibling discovery in configuration.
2. The device discovers other Dragon-family devices on the LAN and presents them for
   explicit selection.
3. Selected devices form a group; membership is reflected to the group members.
4. Opening any group member's UI presents the selected siblings in one tabbed or
   otherwise unified pane.
5. The browser maintains a live session with each selected device so every view stays
   current. This can extend today's HTTP/SSE model or introduce a family WebSocket
   transport after measuring device and browser constraints.

Discovery must not imply authorization or control. Product differences are expressed
through capabilities, and grouping remains explicit rather than silently claiming
every device found on the subnet.

## Delivery sequence

1. **Initial core extraction — complete.** dragon-core stood up; five components
   extracted; DragonBreath #68 merged after CI and hardware HIL.
2. **MQTT-Klipper coordination — complete.** `DC_SRC_KLIPPER_MQTT = 4` landed in
   dragon-core while preserving `DC_SRC_NONE = 3`, with an exclusive `DC_SRC_MAX`;
   DragonBreath #67 was rebased onto the `dc_*` API and merged.
3. **Finish product source namespacing.** Migrate remaining DragonBreath-owned
   `pb_*`/`PB_*` identifiers to `db_*`/`DB_*` in a focused change. New app code uses
   `db_*` immediately; persisted and wire identifiers remain compatible.
4. **Decouple and extract the web layer — in progress.** The static SPA asset and its
   runtime product/capability selection move first as `dc_ui`; DragonBreath continues
   to own the existing API v2, HTTP, OTA, setup, and recovery handlers. Add golden
   response coverage and the product capability boundary before moving those handlers.
5. **DragonVent.** Create a fresh firmware repository on dragon-core, with `dv_*`
   board/motor/device code. Archive OpenVent with a pointer after replacement hardware
   behavior is proven.
6. **DragonStatus.** Build one product family for Panda Status, Pop Status, and Panda
   LUX P2, which share a controller board. Model product variations as capabilities or
   configuration. Evaluate the Vent RGB implementation as a reusable `dc_*` LED module
   that products can specialize.
7. **Same-LAN single pane.** Add opt-in discovery, explicit grouping, multi-device
   live sessions, and the unified family SPA after the device/capability contract is
   stable.

## Repository ownership and releases

The current, working home of the shared core is `justinh-rahb/dragon-core`. Moving the
core and products into a jointly owned organization remains an organizational option,
not a prerequisite and not a completed decision.

Products should pin every selected core component to one tag or one full commit from
the same core revision. DragonBreath currently uses a full SHA. dragon-core's own
ESP-IDF compile fixture commits its dependency lock; DragonBreath currently regenerates
and ignores its application lock, so its component manifests are the reviewed pin.

## Non-goals of the extraction

- No API v2, OTA, or revert-to-stock behavior change.
- No product hardware policy in dragon-core.
- No native management app; the browser SPA is the client.
- No forced NVS or wire-format rename to match source namespaces.
- No claim that every candidate module is shared before two products validate its
  boundary.

## Open decisions

1. The exact `dc_device` capability contract and where each product implementation
   lives.
2. Whether Klipper helpers remain per-product or converge after DragonVent has a real
   Klipper use case.
3. The discovery protocol, group-membership persistence, browser trust model, and
   whether multi-device live state uses SSE/HTTP or a new WebSocket transport.
4. Which RGB behaviors are generic enough for core versus product-specific policy.
5. Whether and when the repositories move into a jointly owned organization.
