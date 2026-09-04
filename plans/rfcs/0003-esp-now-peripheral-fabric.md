# RFC 0003: ESP-NOW low-power peripheral fabric

Status: **Proposed — feedback requested from ESP-NOW, low-power, and Klipper
contributors.**

Date: 2026-08-24

Depends on: [RFC 0001](0001-dragon-integration-planes.md)

## Summary

Simple Dragon peripherals may use ESP-NOW to avoid Wi-Fi association and continuous
IP-stack costs. Battery-powered switches, filament sensors, buttons, environmental
probes, and indicators communicate with a powered ESP32 gateway.

The existing `klipper-esp32`
[`codex/transport-abstraction`](https://github.com/justinh-rahb/klipper-esp32/tree/codex/transport-abstraction)
branch already validates one form of this fabric: a continuously connected C3 Klipper
MCU carries its bidirectional byte stream over reliable ESP-NOW through an S3 USB
bridge. This RFC distinguishes that working **wireless MCU stream** from a future
**sleeping battery leaf** protocol; the latter has different availability and power
semantics and may require a capability adapter rather than a continuous Klipper MCU
session.

This fabric is optimized for power, installation, and bounded stale-state behavior.
It is not a heater interlock or deterministic actuator bus.

## Device class

Good candidates include:

- filament presence or motion sensors;
- door and enclosure switches;
- user buttons and simple remotes;
- ambient, humidity, or monitor-only temperature sensors;
- battery level and accessory-health telemetry; and
- low-duty indicators where delayed or lost updates are non-hazardous.

Some candidates can remain continuously connected as Klipper MCUs. Others need to
wake, report state, and sleep for long intervals. The RFC does not assume those two
operating models can share an unchanged application protocol merely because both use
ESP-NOW.

Excluded from this RFC:

- the sole cutoff for a heater or hazardous motor;
- an output that must be de-energized within a hard radio-dependent deadline;
- high-bandwidth media or firmware streaming; and
- general product-to-product semantic control, which belongs to
  [RFC 0004](0004-peer-capability-plane.md).

## Proposed topology

The existing connected-MCU prototype is:

```text
Klipper host ⇄ S3 native-USB bridge ⇄ reliable ESP-NOW ⇄ C3 Klipper MCU
```

The proposed sleeping-leaf shape is:

```text
┌─────────────────┐       ESP-NOW       ┌────────────────────────┐
│ sleeping leaf   │ ──────────────────▶ │ powered ESP32 gateway  │
│ sensor/switch   │ ◀── optional ACK ── │ pairing + freshness    │
└─────────────────┘                     └────────────┬───────────┘
                                                   │
                                  ┌────────────────┴──────────────┐
                                  ▼                               ▼
                         Klipper-facing adapter          Dragon peer capability
```

The initial topology should be leaf-to-gateway, not arbitrary mesh. A gateway can
support several leaves, but every extra routing role increases awake time, state, and
failure complexity.

## Existing ESP-NOW prototype

At
[`bfb5d9b`](https://github.com/justinh-rahb/klipper-esp32/commit/bfb5d9baad3350b3a18d05e73e3334a871c5dade),
the `codex/transport-abstraction` branch is eight commits ahead of the current
`klipper-esp32` main branch. It adds:

- a transport abstraction beneath the Klipper console;
- coordinator/node diagnostic profiles and bridge/remote stream profiles;
- a bounded version-1 binary frame with source, destination, sequence,
  acknowledgement, type, flags, CRC, and up to 192 bytes of payload;
- discovery and one-second heartbeats;
- one reliable DATA frame in flight per direction;
- protocol acknowledgements, duplicate suppression, a 100 ms ACK deadline, and three
  bounded retransmissions;
- queue, decode, delivery, duplicate, acknowledgement, timeout, and RSSI diagnostics;
- host tests for frame and transport behavior; and
- hardware probes that carry the real Klipper dictionary/queries through the bridge
  and drive a remote C3 GPIO8 NeoPixel blink and color wipe.

The branch explicitly scopes this first stream to protocol bring-up, not motion or
heater control. It does not yet establish production pairing, per-peer authentication,
encrypted application identity, multiple leaves, deep-sleep behavior, WLAN channel
coexistence, or a battery power budget. Those are RFC requirements rather than reasons
to ignore the working transport.

## Why ESP-NOW

ESP-NOW can exchange small frames without joining an access point, acquiring an IP
address, or keeping a conventional Wi-Fi session alive. That can reduce connection
latency and average energy for a device that wakes, samples, transmits, and sleeps.

It does not eliminate RF power constraints. ESP32 transmit peaks can exceed what a
bare coin cell supplies reliably. Candidate hardware must measure peak current,
brownout margin, storage capacitance, sleep leakage, sensor current, and real message
cadence. Small LiPo or primary cells may be more realistic than a coin cell for some
nodes.

## Roles

### Continuously connected MCU node

A connected node runs `klipper-esp32` and keeps a reliable byte stream through the
gateway. Klipper owns its configuration and availability semantics. This is the model
already demonstrated by the bridge/remote profiles and is appropriate only where the
node can remain awake and responsive.

### Sleeping leaf

A sleeping leaf owns its physical sensor or indicator and should:

- wake on a timer or hardware event;
- report a complete current state, not only edges;
- carry stable identity, boot/session ID, and monotonic sequence;
- report battery/health when available;
- retransmit according to bounded policy rather than remaining awake indefinitely;
- store only the pairing material required for its gateway; and
- return to a known low-power state after failure.

### Gateway

A powered gateway should:

- pair and authenticate leaves;
- track replay, sequence, local receipt time, and freshness per capability;
- expose disconnected/stale/unknown separately from false/off;
- map leaf identity to a stable configured name;
- adapt state to Klipper or Dragon peer semantics without hiding its remote origin;
- rate-limit noisy or failing leaves; and
- provide diagnostics for RSSI, last receipt, battery, retries, and protocol version.

## Proposed message properties

The existing version-1 transport frame is already binary and bounded. It supplies the
link fields needed to carry a reliable Klipper byte stream:

```text
version / type / flags / source / destination / sequence / acknowledgement /
payload length / payload / CRC
```

A sleeping semantic leaf needs application state that the raw byte stream does not
define. Its messages need, at minimum:

| Field | Purpose |
|---|---|
| protocol version | reject incompatible layouts |
| message type | state, event, acknowledgement, pairing, diagnostic |
| leaf identity | stable binding independent of address/name |
| boot/session ID | distinguish restart from replay |
| sequence | order and deduplicate frames |
| capability ID | identify door, filament, temperature, battery, etc. |
| value + validity | distinguish false/zero from unknown |
| optional sample age | describe sampling delay; gateway still uses receipt time |
| authentication data | integrity and peer authentication |

The exact encoding, key management, and ESP-NOW encryption use remain open. The
sleeping-leaf protocol may reuse the transport frame after adding the necessary session
and trust behavior, but it should not overload a raw Klipper stream with an unbounded
generic object model.

## State and event semantics

A switch transition is useful as an event, but current state is authoritative. Leaves
must periodically send a full state snapshot so a lost edge cannot leave a door or
filament state wrong forever.

The gateway exposes at least four conditions:

```text
true / active
false / inactive
unknown / no valid sample
stale / previously valid but expired
```

Consumers may collapse these only through explicit policy. `unknown` must never
silently become `false` merely because no packet arrived.

## Pairing and trust

Discovery is not pairing. A proposed user flow is:

1. put one gateway into a short pairing window;
2. physically activate or reset the intended leaf;
3. show a device type and short confirmation code where UI exists;
4. establish per-peer key material;
5. persist the stable identity and allowed capabilities; and
6. leave pairing mode automatically after a short bound.

Factory reset, ownership transfer, gateway replacement, and lost-leaf removal require
explicit flows. A MAC address may help transport delivery but is not sufficient as the
long-term application identity or proof of authorization.

## Channel and coexistence constraints

ESP-NOW and ordinary Wi-Fi channel behavior must be measured on the actual gateway
hardware. A gateway that also joins a WLAN may constrain ESP-NOW peers to the WLAN's
channel. Channel changes, AP replacement, and 2.4 GHz congestion can therefore affect
availability even without IP association on the leaf.

The product UI should expose this dependency rather than promising radio behavior that
cannot survive arbitrary AP channel changes.

## Relationship to `klipper-esp32`

The current Dragon-maintained `klipper-esp32` project is an experimental ESP-IDF port
of Klipper MCU firmware derived from
[`nikhil-robinson/klipper_esp32`](https://github.com/nikhil-robinson/klipper_esp32).
The main branch documents native USB Serial/JTAG and UART transports and useful
peripheral building blocks including scheduled digital output, ADC, hardware I²C,
LEDC PWM, and RMT NeoPixel output. Its `codex/transport-abstraction` branch adds the
working reliable ESP-NOW stream described above.

The branch makes `klipper-esp32` the demonstrated implementation for a continuously
connected wireless MCU. It is not automatically the application contract for a
sleeping Dragon leaf. Before acceptance, follow-up work should establish:

- whether each leaf presents as a Klipper MCU or the gateway aggregates leaves;
- where timing, pin configuration, and restart semantics live;
- how a sleeping node fits Klipper's expectations of MCU availability;
- whether non-Klipper Dragon consumers can reuse the leaf protocol without emulating a
  Klipper host; and
- which functionality belongs in dragon-core versus an integration repository.

One possible boundary is to retain the existing reliable stream unchanged for
connected Klipper MCUs and add a small sleeping-leaf protocol plus gateway adapter for
battery devices. The adapter can translate eligible capabilities into Klipper objects
while preserving native Dragon telemetry for other consumers.

## Power model and prototype evidence

No battery-life claim should be accepted from sleep-current figures alone. A prototype
report should include:

- supply chemistry and usable capacity;
- regulator and sensor quiescent current;
- deep-sleep current of the complete assembled node;
- wake, sample, transmit, retry, and acknowledgement durations;
- peak current and minimum observed voltage;
- normal heartbeat interval and worst-case retry behavior;
- measured packet success under representative enclosure and distance; and
- projected life with an explicit event-rate assumption.

## Acceptance criteria for an implementation

- The existing bridge/remote hardware probe remains reproducible and its reliability
  counters are captured under packet loss and reconnect tests.
- Pairing binds one stable leaf identity to an explicit gateway.
- Authenticated frames reject modification, replay, and stale boot sessions.
- A lost transition is repaired by periodic full-state publication.
- The gateway reports unknown/stale distinctly and expires state from local receipt
  time.
- AP/channel disruption and gateway reboot have documented recovery behavior.
- A noisy leaf cannot starve other leaves or the gateway's product duties.
- Battery measurements include real peak current and complete-board sleep current.
- No acceptance test uses this radio path as the sole hazardous-actuator cutoff.
- Klipper exposure and Dragon peer exposure preserve the same leaf identity and
  freshness rather than creating unrelated duplicate devices.
- Documentation distinguishes continuously connected Klipper nodes from sleeping
  semantic leaves and does not apply battery-life claims from one to the other.

## Non-goals

- General ESP-NOW mesh routing.
- Heater, mains, or hazardous-motion safety control.
- Guaranteed delivery of every event.
- Cloud provisioning.
- Treating a friendly name or MAC address as authorization.
- Treating the existing experimental branch as production-ready for motion, heaters,
  pairing, or battery operation.

## Feedback requested

1. Should the reliable ESP-NOW stream remain the connected-MCU path while sleeping
   leaves use a smaller Dragon capability protocol?
2. Can the existing transport frame safely become their common link envelope, or
   should sleeping leaves use a separate format?
3. Should the gateway aggregate sleeping leaves as one MCU, expose one logical MCU per
   leaf, or publish them only as Dragon peer capabilities?
4. Which pairing UX works on products with and without a screen?
5. What heartbeat and retry ranges are realistic for door, filament, and environmental
   sensor classes?
6. Which battery hardware should be the reference platform so power claims are
   reproducible?
7. How should a Wi-Fi-connected gateway communicate channel changes to sleeping leaves?
