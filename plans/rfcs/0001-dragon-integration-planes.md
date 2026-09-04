# RFC 0001: Dragon device integration planes

Status: **Proposed — feedback requested.**

Date: 2026-08-24

## Summary

The Dragon family should use three distinct integration planes, selected by device
class rather than by a single preferred transport:

1. a **wired safety spine** for mains-powered, actuator-bearing, timing-sensitive
   hardware;
2. an **ESP-NOW peripheral fabric** for simple, low-power leaf devices; and
3. a **Dragon peer capability plane** for powered, intelligent products exchanging
   semantic state and capabilities.

These planes complement each other. A product may bridge them, but their authority
and failure contracts must remain separate.

## Motivation

Current and proposed Dragon devices span incompatible constraints:

- heater controllers must remain safe across host, network, and radio failure;
- a battery door switch or filament sensor benefits more from low association and
  idle-power cost than deterministic sub-millisecond timing;
- products such as a future DragonTouch can understand attached accessories and
  publish meaningful capabilities to another Dragon product; and
- some simple devices may be complete on a single ESP32, while others need a wired
  RP/STM actuator controller plus a network-capable brain.

Treating all of these as one bus would blur safety authority, power budgets, pairing,
and acceptance criteria. Treating them as unrelated would duplicate identity,
discovery, freshness, and diagnostics work. This RFC establishes shared vocabulary
while leaving each plane its own focused design.

## Proposed model

| Plane | Typical devices | Primary goal | Timing/safety authority |
|---|---|---|---|
| Wired safety spine | heaters, motors with hazardous failure modes, mains outputs | deterministic local actuation | wired controller owns real-time output safety |
| ESP-NOW peripheral fabric | switches, runout sensors, buttons, battery probes, indicators | low power and easy installation | never sole hard-safety authority |
| Dragon peer capability plane | DragonTouch, DragonBreath, DragonVent, DragonStatus | semantic product-to-product cooperation | product policy remains local to the actuator owner |

A fourth shape—one ESP32 running a complete simple product—does not require a fourth
plane. It is a product composition choice: the device can implement local behavior
and participate in the peripheral or peer plane where useful.

## System sketch

```text
 low-power leaves                    powered Dragon products
┌───────────────┐                   ┌──────────────────────┐
│ door / sensor │── ESP-NOW ──────▶│ DragonTouch / gateway│
└───────────────┘                   └──────────┬───────────┘
                                             │ semantic peer capability
                                             ▼
                                  ┌────────────────────────┐
                                  │ DragonBreath brain     │
                                  │ product policy         │
                                  └───────────┬────────────┘
                                              │ wired safety spine
                                              ▼
                                  ┌────────────────────────┐
                                  │ RP/STM actuator MCU    │
                                  │ watchdog + safe outputs│
                                  └────────────────────────┘
```

The diagram is illustrative. DragonBreath currently runs its product policy and
hardware control on one ESP32-C3; adopting a wired safety spine is a future product
architecture, not a claim about current hardware.

## Cross-plane invariants

### 1. The actuator owner retains safety authority

A remote command, measurement, UI, or gateway may influence desired behavior but
must not disable local sensor checks, fixed cutoffs, watchdogs, safe boot state, or
output interlocks.

### 2. Transport does not imply authority

Discovery, pairing, and successful delivery do not grant permission to command an
actuator. Command authority, observation, and regulation input are separate roles.

### 3. Freshness is decided by the consumer

Messages may carry source timestamps and sequence numbers, but a consumer must track
local receipt time and expire data itself. A remote clock cannot prove a sample is
fresh.

### 4. Identity and binding are explicit

Discovery presents candidates. A user or an authenticated provisioning flow binds a
specific stable device and capability. Friendly names and network addresses are not
identities.

### 5. Loss behavior is part of configuration

Every consumed remote capability defines what happens when it becomes unavailable:
suspend, fall back, retain for a bounded time, or report unknown. Indefinite stale
state is never a valid policy.

### 6. Product and wire compatibility are deliberate

Persisted keys, numeric enum values, peer identifiers, API fields, and pairing state
need explicit migration plans. Source-code naming cleanup alone is not sufficient
reason to break deployed state.

### 7. Bridges do not erase boundaries

A DragonTouch may bridge an I²C accessory into Dragon peer telemetry, or an ESP-NOW
gateway may expose a leaf to Klipper. The bridge must preserve source identity,
freshness, validity, and authority instead of presenting bridged data as local truth.

## Relationship to existing work

- [`dragon-family-unification.md`](../dragon-family-unification.md) defines shared
  dragon-core services and product-local hardware policy.
- [`decouple-httpd-device-interface.md`](../decouple-httpd-device-interface.md)
  proposes a capability boundary between shared transport and product behavior.
- DragonBreath PR
  [#89](https://github.com/plastikman/DragonBreath/pull/89) demonstrates a remote
  chamber measurement used for regulation while local NTC/PTC sensors retain safety
  authority. It is a product-specific precursor, not the generic peer protocol.
- [`KLIPPER_DISTRIBUTION_INTEGRATION.md`](../../docs/KLIPPER_DISTRIBUTION_INTEGRATION.md)
  documents the current host integration and its device-authoritative safety model.

## Decision boundaries

RFC 0001 does not choose:

- UART versus CAN for the wired spine;
- a final ESP-NOW packet format or gateway topology;
- HTTP/SSE, UDP, ESP-NOW, or another carrier for smart peer communication;
- a common firmware image for every Dragon product; or
- whether a particular proposed sensor is accurate enough for regulation.

Those decisions belong in the focused RFCs or product validation.

## Adoption sequence

1. Agree on the three-plane vocabulary and cross-plane invariants.
2. Prototype each plane independently with simulated failure injection.
3. Let one real product validate each boundary before extracting shared code into
   dragon-core.
4. Define shared identity/pairing primitives only after comparing prototype needs.
5. Add bridges after both sides have stable contracts; do not make the first
   prototype a universal gateway.

## Non-goals

- Replacing DragonBreath's current hardware architecture in this RFC.
- Treating ordinary telemetry loss as a hardware emergency unless product policy
  explicitly requires it.
- Using ESP-NOW for mains heater interlocks or safety watchdog timing.
- Requiring Klipper for peer-to-peer Dragon product features.
- Defining a cloud service or cloud identity plane.

## Feedback requested

1. Do these three planes cover the known Dragon product and accessory classes?
2. Which devices genuinely need to participate in more than one plane?
3. Are command authority, regulation input, and observation the right minimum role
   separation?
4. Which identity and pairing concepts can safely be shared without prematurely
   forcing one transport?
5. Which first prototype would provide the strongest evidence for or against this
   architecture?

## Acceptance criteria for this RFC

The RFC may move from Proposed to Accepted when:

- maintainers agree that safety-critical wired actuation, low-power leaves, and smart
  peers have distinct contracts;
- each focused RFC has an owner and a bounded prototype target;
- no known device class requires violating the cross-plane invariants; and
- unresolved transport choices are explicitly assigned to a focused RFC rather than
  hidden in implementation work.
