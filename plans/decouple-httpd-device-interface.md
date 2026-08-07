# RFC: Decouple the DragonBreath web layer through a device interface

Status: **Follow-on design after the initial dragon-core extraction.** The first five
board-neutral components moved successfully in DragonBreath
[#68](https://github.com/plastikman/DragonBreath/pull/68) without this refactor. This
RFC now covers the next boundary: making HTTP/OTA, the portal, and the browser SPA
reusable across DragonBreath, DragonVent, and later DragonStatus products.

## Current baseline

After #68:

- `dc_wifi`, `dc_moonraker`, `dc_bambu`, `dc_source`, and `dc_evlog` live in
  dragon-core and are consumed through pinned ESP-IDF Component Manager manifests.
- `pb_httpd`, `pb_portal`, `pb_ha`, and `pb_hil` remain DragonBreath-local because they
  still depend on product hardware or policy. Their `pb_*` names are transitional;
  DragonBreath's target product namespace is `db_*`.
- API v2, OTA, revert-to-stock behavior, and compatibility-sensitive NVS/wire values
  were unchanged by the extraction.
- The existing SPA remains product-local. The family direction is one browser SPA that
  carries sibling-product views and selects them from runtime capabilities.

The first asset-only slice is now implemented on the shared-SPA feature branches:
dragon-core `dc_ui` owns the editable SPA and embedded gzip bytes, DragonBreath serves
that asset through its unchanged product-local portal, and `/api/v2/info` supplies an
additive schema/product/display-name descriptor. HTTP, OTA, setup, recovery, and
hardware policy remain local; moving them still requires the response/device boundary
described below.

This means HTTP decoupling is no longer on the critical path for **creating**
dragon-core. It is on the critical path for extracting the web and management layer.

## Motivation

Most web transport and recovery behavior is product-neutral, but it shares component
boundaries with DragonBreath heater policy, NTC calibration, LEDs, and other hardware.
That coupling prevents DragonVent or DragonStatus from reusing the OTA and browser
stack without also inheriting heater assumptions.

The desired boundary is:

```text
dragon-core                         DragonBreath
  dc_httpd / dc_portal / SPA          db_device
  auth, events, logs, OTA             state + commands
  HTTP/SSE transport                  heater/sensor capabilities
  capability dispatch                 safety policy remains local
```

DragonVent and DragonStatus provide different product implementations behind the same
small capability contract. Core never switches product GPIO or decides heater/motor
safety policy.

## Safety net before movement

`tests/check_api_v2_contract.sh` is primarily a static source/layout check. It is useful
for catching accidental ownership changes, but it cannot prove that refactored handlers
emit equivalent JSON or preserve command semantics.

Before moving handlers across component boundaries:

1. Extract deterministic response builders from the transport handlers.
2. Add host tests using representative product snapshots and golden API responses.
3. Cover success and error results for state, command, heartbeat/lease, calibration,
   OTA metadata, and capability reporting.
4. Keep the existing static contract check for route/asset ownership where it remains
   useful.
5. Run the firmware build and safe-devboard HIL on every boundary change; use physical
   product hardware for product-specific actuator validation.

The tests should compare semantic JSON where object ordering is not contractual and
exact bytes only where clients genuinely depend on the representation.

## Proposed device boundary

The exact ABI remains to be designed, but ownership should resemble:

```c
typedef struct {
    const char *product;
    esp_err_t (*append_info)(cJSON *root);
    esp_err_t (*append_state)(cJSON *root);
    esp_err_t (*command)(const char *op, const cJSON *args, cJSON *result);
    esp_err_t (*heartbeat)(const cJSON *args, cJSON *result);
    const dc_calibration_iface_t *calibration; /* NULL when unsupported */
    const dc_ui_capabilities_t *ui;
} dc_device_t;
```

This is illustrative, not a frozen header. In particular, the design still needs to
settle allocation/ownership, error mapping, snapshot consistency, and whether
calibration should be generic or remain an optional product route.

Naming follows the family convention:

- shared interface and transport: `dc_device`, `dc_httpd`, `dc_portal`;
- DragonBreath implementation: `db_device`;
- DragonVent implementation: `dv_device`;
- compatibility-sensitive routes, JSON fields, headers, and NVS keys remain unchanged.

## Shared SPA and capabilities

The web client is already a SPA running in browser context. The family SPA can carry
the screens for all supported siblings; the connected device's runtime capabilities
decide which pages and controls are visible. A descriptor is useful where it expresses
capabilities and labels, but it does not need to generate every interaction from an
abstract schema.

Prefer folding stable capability metadata into the existing `/api/v2/info` response so
startup does not consume an extra constrained HTTP connection. Branding, hardware
variant, available sensors/actuators, calibration support, and control surfaces are
candidate fields.

Same-LAN discovery and grouping are a separate follow-on built on this capability
model. When enabled, the browser connects directly to each explicitly selected sibling
and presents a unified pane. The implementation may extend today's HTTP/SSE transport
or add a family WebSocket after measuring constraints. Discovery does not grant control
and should not silently group every device found on the LAN.

## Delivery sequence

1. **Golden response coverage.** Add executable API response and command-result tests
   without changing firmware behavior.
2. **Introduce the product interface locally.** Move DragonBreath-specific state,
   command, and calibration behavior behind `db_device` while the transport remains in
   the DragonBreath repository.
3. **Separate portal assets — implemented by `dc_ui`.** Keep editable HTML/SPA assets
   out of C string literals and retain the existing ESP-IDF + gzip embedding flow; do
   not require a Node build toolchain for firmware.
4. **Add runtime capabilities — foundation implemented.** DragonBreath publishes UI
   schema/product identity and the existing capability array gates current surfaces.
   Add DragonVent/DragonStatus views before freezing the wider contract.
5. **Extract reusable web components.** Move only proven product-neutral HTTP/OTA,
   portal, and SPA pieces into dragon-core, then pin all products to the same tested
   core release.
6. **Add sibling management.** Implement opt-in same-LAN discovery, explicit groups,
   and multi-device live views after the capability boundary is stable.

Each step must leave DragonBreath buildable and recoverable. `/update`, image
validation, boot-slot selection, and revert-to-stock are high-consequence paths and
need explicit regression coverage.

## Non-goals

- No API v2 or OTA behavior change merely to move code.
- No heater, fan, motor, sensor, or GPIO policy in dragon-core.
- No forced source-prefix rewrite of NVS or wire identifiers.
- No native phone/desktop app; the web SPA is the management client.
- No requirement that DragonVent or DragonStatus use every DragonBreath capability.

## Open decisions

1. The concrete `dc_device` ABI and snapshot/error semantics.
2. Whether calibration remains an optional specialized interface or becomes a named
   parameter capability.
3. How much of the UI is explicit sibling views versus descriptor-driven controls.
4. Authentication, trust, and transport behavior for a browser maintaining several
   live device sessions.
5. Which pieces of `pb_ha` and `pb_hil` become reusable after their product-policy
   dependencies are separated.
