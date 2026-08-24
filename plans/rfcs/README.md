# Dragon RFC series

This directory holds proposed cross-product architecture for the Dragon firmware
family. RFCs are discussion documents: they define constraints, boundaries, and
questions before implementation work is committed.

Historical RFCs remain in [`plans/`](../) and are not being renumbered. New
cross-product proposals use numbered files here so their relationships and status
are easy to follow.

## Status vocabulary

| Status | Meaning |
|---|---|
| **Proposed** | Open for feedback; no implementation commitment. |
| **Accepted** | Direction agreed; unresolved details may remain. |
| **Implementing** | Accepted and represented by active implementation work. |
| **Implemented** | Shipped and retained as a design record. |
| **Superseded** | Replaced by another RFC, linked from both documents. |
| **Withdrawn** | Deliberately abandoned. |

A status change should happen in a reviewed pull request. Merging a Proposed RFC
records the proposal and discussion; it does not by itself mark the proposal
Accepted.

## Current series

| RFC | Title | Status |
|---|---|---|
| [0001](0001-dragon-integration-planes.md) | Dragon device integration planes | Proposed |
| [0002](0002-wired-safety-spine.md) | Wired safety spine for heater-class devices | Proposed |
| [0003](0003-esp-now-peripheral-fabric.md) | ESP-NOW low-power peripheral fabric | Proposed |
| [0004](0004-peer-capability-plane.md) | Dragon peer capabilities and remote sensors | Proposed |

RFC 0001 is the umbrella vocabulary. RFCs 0002–0004 are intentionally separate:
they have different power, timing, trust, and failure requirements even when a
future product participates in more than one plane.

## Review expectations

Feedback is especially useful when it identifies:

- a device class that does not fit the proposed boundaries;
- a safety property that accidentally depends on a network or radio path;
- a transport assumption that is being confused with an application contract;
- a compatibility or migration problem for deployed devices;
- a failure mode that is not observable or testable;
- a power, memory, socket, or timing cost missing from the proposal; or
- a smaller first implementation that still validates the boundary.

Implementation PRs should link the RFC they implement and state which acceptance
criteria they cover. Material protocol or safety changes should update the RFC or
supersede it rather than silently diverging.
