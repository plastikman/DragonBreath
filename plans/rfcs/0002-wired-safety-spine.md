# RFC 0002: Wired safety spine for heater-class devices

Status: **Proposed — feedback requested from Klipper, firmware, and hardware
contributors.**

Date: 2026-08-24

Depends on: [RFC 0001](0001-dragon-integration-planes.md)

## Summary

Future Dragon products with hazardous actuators should separate network/product
orchestration from deterministic hardware control. An RP2040, STM32, or comparable
MCU runs a real Klipper MCU firmware and owns time-sensitive I/O over a wired link. A
Dragon brain runs product services and a
[`klipper-micro`](https://github.com/justinh-rahb/klipper-micro) integration but
cannot make radio availability part of the hardware safety chain.

This RFC defines the boundary and evidence required before selecting boards or buses.
It does not propose retrofitting current DragonBreath hardware.

## Device class

This plane is intended for hardware where an incorrect or indefinitely held output
can create a hazardous condition, including:

- chamber and element heaters;
- mains-switched loads;
- motors whose uncontrolled motion can cause damage;
- safety fans coupled to a heat-producing actuator; and
- local thermal or current interlocks that must remain effective when the brain is
  rebooting or disconnected.

It is not required for every wired sensor or LED.

## Proposed topology

```text
┌──────────────────────────────────┐
│ Dragon brain                     │
│                                  │
│ product policy and UI            │
│ network/cloud/printer transports │
│ klipper-micro integration        │
└───────────────┬──────────────────┘
                │ wired, framed, versioned
                ▼
┌──────────────────────────────────┐
│ RP/STM safety/actuator MCU       │
│                                  │
│ Klipper MCU firmware             │
│ deterministic GPIO/ADC/timers    │
│ watchdog and safe boot outputs   │
└───────────────┬──────────────────┘
                ▼
        SSR / fan / sensors
```

The exact ownership split between Klipper's existing MCU facilities and Dragon-local
hardware protections remains to be designed. The non-negotiable property is that a
brain or radio failure cannot leave a hazardous output energized indefinitely.

## Responsibilities

### Actuator MCU

The wired MCU should own:

- safe pin state before firmware initialization;
- deterministic output scheduling;
- direct sampling needed by hardware-local protections;
- an independently enforced command/watchdog timeout;
- immediate de-energization on malformed, expired, or lost control;
- a versioned description of supported pins, sensors, and safety features; and
- enough diagnostics to distinguish watchdog, sensor, protocol, and power faults.

### Dragon brain

The brain should own:

- user-visible product modes and targets;
- network transports and printer integrations;
- configuration, UI, OTA coordination, and event history;
- high-level policy that remains safe when commands are rejected or delayed;
- compatibility checks before arming an actuator MCU; and
- explicit presentation of MCU disconnect and fault states.

### Hardware

Software watchdogs do not replace appropriate hardware design. Depending on the
product, the board may still need pull states, thermal fuses, interlocks, isolation,
and an output topology that defaults off through reset and partial power conditions.

## Safety contract

### Boot

- Actuator outputs are off before either processor has completed boot.
- Reboot does not restore an active heat command.
- The brain must complete protocol/capability negotiation before arming.

### Link loss

- The actuator MCU expires active demand locally within a documented upper bound.
- Reconnection reports authoritative MCU state before accepting new demand.
- Restored connectivity does not silently replay a pre-disconnect command.

### Version mismatch

- Unknown protocol or safety capability versions fail unarmed.
- A compatibility fallback may reduce functionality but cannot bypass required
  safeguards.

### Brain failure

- A wedged, rebooting, or disconnected brain cannot keep an output alive beyond the
  MCU watchdog bound.
- The actuator MCU does not depend on Wi-Fi, MQTT, a browser, or a cloud service.

### MCU failure

- Hardware pull states and independent cutoffs handle MCU reset where feasible.
- The brain reports loss of the MCU but does not claim that reporting itself made the
  hardware safe.

## Relationship to Klipper

The intent is to use real Klipper MCU firmware and protocol behavior rather than
inventing a second approximate pin scheduler. `klipper-micro` on the Dragon brain is
the proposed integration boundary.

The current `klipper-micro` reference is a native ESP-IDF Klipper host for an ESP32
CYD. It drives an unmodified Klipper MCU over UART and already demonstrates bounded
wire frames, retry/resynchronization, selective identify parsing, clock regression,
scheduled heater/fan updates, a three-second output watchdog, thermistor conversion,
and host-side safety checks. This is evidence that the brain-side boundary is
feasible, not evidence that its current CYD product choices or safety implementation
are ready to transplant unchanged into a Dragon heater product.

Before acceptance, the design needs validation against upstream Klipper assumptions:

- which host responsibilities `klipper-micro` implements;
- how MCU clocks, configuration, restart, and shutdown are handled;
- which watchdog behavior exists upstream versus must remain product hardware policy;
- whether a Dragon product can expose useful local behavior while also participating
  in a printer's Klipper instance; and
- how protocol/version compatibility is tested across releases.

No Dragon-specific protocol extension should be proposed until the upstream behavior
has been measured and documented.

## Candidate wired transports

| Transport | Strengths | Questions |
|---|---|---|
| UART | simple, cheap, easy to inspect | distance, framing, isolation, single peer |
| USB | standard host integration and bootloader options | connector/power behavior, embedded host cost |
| CAN | robust multidrop and differential signaling | transceiver/termination cost, arbitration and addressing |

Transport selection should follow board topology and fault testing. It is not an
architectural vote for one bus across every product.

## Prototype proposal

Use a safe dev board with visible low-energy outputs before connecting a heater:

1. run the candidate Klipper MCU firmware on an RP/STM board;
2. connect it by the simplest candidate wired transport to an ESP32 brain;
3. schedule an LED or logic-level output and sample simulated sensors;
4. inject brain reset, link removal, malformed frames, stale commands, MCU reset, and
   version mismatch;
5. measure worst-case output-off latency with external instrumentation; and
6. only then repeat through an isolated dummy load representative of product I/O.

## Acceptance criteria for an implementation

- No radio or IP path is required for actuator watchdog timing.
- Outputs are demonstrably off during reset, boot, unconfigured, and incompatible
  states.
- Link-loss output-off latency has a measured maximum and test margin.
- A stale command cannot be replayed after either processor reboots.
- Hardware-local sensor failures de-energize affected outputs without brain help.
- Protocol and capability mismatch leave the actuator unarmed.
- Host tests cover framing/state transitions and hardware tests cover actual output
  timing.
- Recovery and firmware update order cannot strand the product with unsafe or
  permanently incompatible processor versions.

## Non-goals

- Wireless actuator control.
- A generic replacement for Klipper's existing MCU protocol.
- Moving product UI or network integrations onto the actuator MCU.
- Assuming a second processor alone makes a design safe.
- Selecting the production MCU or bus in this RFC.

## Feedback requested

1. Is `klipper-micro` the correct brain-side boundary, and which upstream assumptions
   need a proof-of-concept first?
2. Which protections belong in ordinary Klipper MCU configuration versus dedicated
   product firmware or hardware?
3. What wired transport best represents the likely board-to-board topology?
4. How should coordinated brain/MCU firmware upgrades and rollback work?
5. Which measurements are required before this architecture can be considered for a
   heater-class product?
