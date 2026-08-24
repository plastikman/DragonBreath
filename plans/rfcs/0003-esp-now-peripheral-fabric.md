# RFC 0003: ESP-NOW low-power peripheral fabric

Status: **Proposed — feedback requested from ESP-NOW, low-power, and Klipper
contributors.**

Date: 2026-08-24

Depends on: [RFC 0001](0001-dragon-integration-planes.md)

## Summary

Simple Dragon peripherals may use ESP-NOW to avoid Wi-Fi association and continuous
IP-stack costs. Battery-powered switches, filament sensors, buttons, environmental
probes, and indicators communicate with a powered ESP32 gateway. The gateway may
expose them to Klipper through an extension of
[`klipper-esp32`](https://github.com/justinh-rahb/klipper-esp32), to Dragon products
through the peer capability plane, or to both through explicit adapters.

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

Excluded from this RFC:

- the sole cutoff for a heater or hazardous motor;
- an output that must be de-energized within a hard radio-dependent deadline;
- high-bandwidth media or firmware streaming; and
- general product-to-product semantic control, which belongs to
  [RFC 0004](0004-peer-capability-plane.md).

## Proposed topology

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

### Leaf

A leaf owns its physical sensor or indicator and should:

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

The first protocol should be binary and bounded rather than open-ended JSON. A frame
needs, at minimum:

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

The exact encoding, key management, and ESP-NOW encryption use remain open. Version 1
should reserve extension space without creating an unbounded generic object model.

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
Its documented transports are native USB Serial/JTAG and UART; it does not currently
claim ESP-NOW transport. It already supports useful leaf building blocks including
scheduled digital output, ADC, hardware I²C, LEDC PWM, and RMT NeoPixel output.

This RFC therefore proposes new ESP-NOW transport/gateway work around that project; it
does not describe a feature that has already landed. `klipper-esp32` is a candidate
Klipper-facing implementation, not automatically the wire contract for every Dragon
leaf. Before acceptance, a prototype should establish:

- whether each leaf presents as a Klipper MCU or the gateway aggregates leaves;
- where timing, pin configuration, and restart semantics live;
- how a sleeping node fits Klipper's expectations of MCU availability;
- whether non-Klipper Dragon consumers can reuse the leaf protocol without emulating a
  Klipper host; and
- which functionality belongs in dragon-core versus an integration repository.

The likely boundary is a small leaf protocol plus a gateway adapter. The adapter can
translate eligible capabilities into Klipper objects while preserving native Dragon
telemetry for other consumers.

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

## Non-goals

- General ESP-NOW mesh routing.
- Heater, mains, or hazardous-motion safety control.
- Guaranteed delivery of every event.
- Cloud provisioning.
- Treating a friendly name or MAC address as authorization.
- Committing to `klipper-esp32` internals before a gateway prototype is measured.

## Feedback requested

1. Should leaves speak a Dragon-specific bounded protocol with a Klipper adapter, or
   can `klipper-esp32` cleanly provide the common wire contract?
2. Should the gateway aggregate leaves as one MCU or expose one logical MCU per leaf?
3. Which pairing UX works on products with and without a screen?
4. What heartbeat and retry ranges are realistic for door, filament, and environmental
   sensor classes?
5. Which battery hardware should be the reference platform so power claims are
   reproducible?
6. How should a Wi-Fi-connected gateway communicate channel changes to sleeping leaves?
