# RFC 0004: Dragon peer capabilities and remote sensors

Status: **Proposed — feedback requested from Dragon product, networking, UI, and
safety contributors.**

Date: 2026-08-24

Depends on: [RFC 0001](0001-dragon-integration-planes.md)

## Summary

Powered Dragon products should be able to discover, explicitly bind, and consume
semantic capabilities from trusted peers. A future DragonTouch could read an attached
I²C chamber sensor and provide that measurement to DragonBreath; DragonBreath could use
it for regulation while retaining local sensors and policy as safety authority.

The peer plane exchanges meaning—temperature, presence, input events, display or
status capabilities—not remote GPIO numbers or Klipper MCU commands.

## Motivating example

Some printer ecosystems can display an accessory chamber sensor on a touchscreen but
cannot make the printer firmware use it for chamber regulation. A DragonTouch-style
product could bridge that locally attached sensor to DragonBreath:

```text
┌────────────────┐  I²C   ┌────────────────┐  Dragon peer telemetry  ┌──────────────┐
│ chamber sensor │───────▶│ DragonTouch    │────────────────────────▶│ DragonBreath │
└────────────────┘        │ provider/bridge│                         │ consumer      │
                          └────────────────┘                         └──────┬───────┘
                                                                          │
                                                local NTC + PTC safety ────┤
                                                                          ▼
                                                                       heater
```

DragonBreath PR
[#89](https://github.com/plastikman/DragonBreath/pull/89) establishes a useful
product-local precedent: a fresh Bambu-reported chamber temperature can be selected
for regulation, while DragonBreath's local chamber NTC, PTC, fixed cutoffs, fault
handling, and output ownership remain local. This RFC generalizes the measurement
provider without making Bambu or DragonTouch special cases in heater policy.

## Scope

Initial peer capabilities may include:

- temperature, humidity, and other environmental observations;
- switches, presence, and user input events;
- accessory identity and health;
- status/display surfaces; and
- read-only product state useful to a unified family UI.

Command capabilities may be proposed later, but observation does not imply command
authority. Heater target/mode control and remote regulation input are distinct roles.

## Roles

### Provider

A provider owns a capability and publishes its current state, validity, and metadata.
It may bridge local hardware, but must preserve the sensor's remote/bridged identity.

### Consumer

A consumer explicitly binds a provider capability to a local purpose, tracks freshness
using local receipt time, and owns loss/fallback policy.

### Gateway

A gateway adapts another integration plane—for example an ESP-NOW leaf—into peer
capabilities. It does not erase the leaf identity or reset sample age merely because
the gateway itself is online.

### Browser/UI

The family SPA may discover and present peer candidates or grouped product state. The
browser is not required to relay safety-relevant telemetry between devices.

## Capability identity

A binding should address a stable tuple resembling:

```text
peer_id / capability_id
```

Example:

```text
dragontouch-a1b2 / sensor.chamber_air
```

Network address, mDNS name, model name, and user-facing label may change without
changing the binding. A capability identifier remains stable across ordinary reboot
and software update. Ownership transfer or factory reset creates a new trust decision.

## Proposed observation model

A temperature observation needs at least:

| Field | Meaning |
|---|---|
| peer identity | paired source device |
| capability identity | stable sensor/function within that device |
| kind | e.g. `temperature.chamber_air` |
| value and unit | finite temperature in a canonical wire unit |
| valid | source believes the sample is usable |
| sequence | ordering and duplicate detection |
| boot/session ID | restart and replay boundary |
| sampled age | optional source-side acquisition delay |
| local receipt time | consumer-owned freshness authority |
| optional accuracy/calibration metadata | diagnostic, not proof of correctness |

A provider timestamp alone cannot establish freshness because peer clocks may be
unset, skewed, or reset.

## Regulation-source model

DragonBreath should eventually replace provider-specific booleans with an explicit
regulation binding, conceptually:

```text
local
printer:bambu/chamber
peer:<peer-id>/<capability-id>
```

The selected input affects set-point demand only. Local DragonBreath sensors retain:

- chamber and element over-temperature trips;
- sensor-open/short failure handling;
- local thermal foldback;
- communications/watchdog policy;
- safe boot and fault-latch behavior; and
- sole ownership of physical heater output.

No remote sensor can raise local target ceilings or disable these protections.

## Loss policies

The consumer stores an explicit policy with each regulation binding:

| Policy | Behavior when remote data is stale/unavailable |
|---|---|
| `suspend` | stop heater demand while retaining the user's armed mode/target |
| `fallback_local` | regulate from the local chamber NTC |
| `monitor_only` | never use the peer for regulation |

`suspend` is the proposed default for a generic peer sensor. `fallback_local` may be
appropriate when the product has validated that transition, but it is not universally
conservative: a local sensor and remote bulk-air sensor can disagree in either
direction.

Recovery from a non-latching stale-data suspension may resume regulation when a fresh,
authenticated sample returns. Whether a product requires manual acknowledgement is a
product-policy decision and must be visible in its UI.

## Freshness and plausibility

The consumer should enforce:

- a configured maximum receipt age;
- finite and product-bounded values;
- sequence/replay checks;
- boot/session changes;
- optional rate-of-change or jump diagnostics; and
- immediate invalidation when the paired transport session loses trust.

Plausibility filtering cannot turn a remote sensor into a safety sensor. It can only
prevent clearly invalid data from influencing ordinary regulation.

## Pairing and authorization

Discovery lists candidates but grants no access. Pairing should:

1. require a short explicit window or physical confirmation on at least one product;
2. authenticate a stable peer identity;
3. exchange or derive per-peer key material;
4. record which capabilities may be consumed or commanded;
5. show the binding on both products where UI exists; and
6. support revocation, factory reset, and ownership transfer.

Observation, regulation, and command permissions should be distinct. A peer authorized
to publish a temperature is not thereby allowed to set a heater target.

## Transport direction

This RFC deliberately separates the semantic contract from its carrier. Candidate
same-LAN transports include:

- provider HTTP state with consumer polling;
- provider SSE/WebSocket stream with bounded polling fallback;
- authenticated periodic UDP/unicast telemetry; and
- ESP-NOW where both powered products and channel constraints make it appropriate.

The first implementation should favor existing ESP-IDF and dragon-core facilities,
measure socket/memory cost, and fail cleanly under reconnect storms. A browser must not
be the required relay.

ESP-NOW leaves from [RFC 0003](0003-esp-now-peripheral-fabric.md) may enter this plane
through a powered gateway. Their application identity and original sample age must be
preserved across the bridge.

## Discovery and grouping

The existing
[`dragon-family-unification.md`](../dragon-family-unification.md) proposes opt-in
same-LAN discovery and explicit device groups for a unified family SPA. Peer discovery
can reuse product identity and capability descriptions, but grouping and control
authorization remain separate:

- discovered does not mean paired;
- paired does not mean grouped in the UI;
- grouped does not mean authorized to command; and
- a browser group is not a device-to-device safety path.

## API and diagnostics

A consumer should expose generic regulation diagnostics rather than requiring the UI
to know every provider name. A future state shape could resemble:

```json
{
  "regulation": {
    "source": "peer:dragontouch-a1b2/sensor.chamber_air",
    "temperature_c": 41.2,
    "age_ms": 420,
    "fresh": true,
    "loss_policy": "suspend",
    "active": true
  }
}
```

This is illustrative, not a frozen API. Existing Bambu-specific fields and persisted
settings require an additive migration and compatibility period if generalized.

Diagnostics should also show:

- paired peer identity and display name;
- capability identity and kind;
- current validity/freshness reason;
- last sequence/session transition;
- transport reconnect/error counts; and
- whether regulation is remote, local fallback, or suspended.

Secrets, printer serials, raw credentials, and stable identifiers not needed by the
client should not be exposed in public state or logs.

## Prototype proposal

The first proof should use simulated temperature values and no mains load:

1. create a minimal provider exposing one `temperature.chamber_air` capability;
2. pair one consumer to its stable identity;
3. feed the observation into the existing DragonBreath external-regulation seam;
4. use LEDs or a logic-level dummy output to observe demand;
5. inject stale samples, replay, provider reboot, gateway reboot, address change,
   corrupted values, and trust revocation;
6. verify local safety inputs remain authoritative in every case; and
7. then replace the simulator with DragonTouch plus a real attached sensor.

## Acceptance criteria for an implementation

- Discovery and pairing are separate, explicit operations.
- Bindings use stable peer/capability identity, not address or friendly name.
- The consumer owns freshness from local receipt time and never holds a sample
  indefinitely.
- Regulation, observation, and command permissions are distinct.
- Loss policy is stored, visible, and covered by tests.
- Remote regulation cannot bypass local limits, cutoffs, faults, or output ownership.
- Provider restart, replay, address change, and malformed values have deterministic
  behavior.
- A bridge preserves original source identity and sample age.
- UI/API diagnostics state whether regulation is remote, local fallback, or suspended.
- A browser, cloud service, MQTT broker, or printer firmware is not required for the
  direct product-to-product path unless explicitly selected by a later transport RFC.

## Non-goals

- Generic remote GPIO access.
- Replacing Klipper MCU semantics.
- Multi-sensor averaging or automatic sensor fusion in version 1.
- Letting discovery grant command authority.
- Making remote measurements local hard-safety sensors.
- Selecting a final carrier before prototype measurements.

## Feedback requested

1. Is `peer_id/capability_id` sufficient for durable binding, or is another namespace
   layer needed?
2. Should `suspend` be the default loss policy for all remote regulation sources?
3. What freshness bounds are appropriate for chamber regulation versus ordinary
   environmental display?
4. Should pairing and group management reuse one trust store or remain independent?
5. Which carrier best fits powered peers without consuming too many ESP32 sockets or
   coupling to browser availability?
6. Which parts belong in dragon-core only after DragonTouch and DragonBreath validate
   the boundary?
