# Control diagnostics semantics and delivery

PR #92 exposes the existing control path. It does not reconstruct an independent
controller demand or introduce another actuator calculation.

## Demand boundary

The firmware pins dragon-core v0.33.0. In `dc_pid_step`, the integral is first
clamped to the configured integral bounds. Conditional integration then compares
the candidate output to `output_max`. DragonBreath sets that maximum to the
active approach cap and sets `integral_max` to the cap minus P (bounded to 0..1).
Thus even the returned P+I+D terms have already been shaped by product policy.
Holding integration during thermal foldback also affects controller history.

At a steady 1 °C error, the integral reaches 0.30 and output reaches the 0.40
approach cap. A cooling transient can make P+I+D exceed the cap, but that does
not establish demand independent of the cap. The old host test proved only
this transient, not the stronger semantics advertised by `controller_request`.

| Quantity | Availability / presentation |
| --- | --- |
| Demand before all product limits | Unavailable; no field is fabricated |
| PID output after approach/target policy | `control.loop.pid_output`, copied from the real adapter's duty |
| Allowed output after thermal governors | `control.loop.allowed_output`, the existing `heater.commanded_duty` |
| Instantaneous SSR command | `heater.output`; no electrical feedback is implied |

The prototype `controller_request` field is replaced, not retained as an alias.
No PID gains, math, thresholds, policy, foldbacks, faults, interlocks, fans, SSR
timing, or mutation APIs are changed. The browser only presents these values.

Upstream follow-up candidate (not implemented here): clarify `dc_pid_result_t`
documentation that its terms and any raw/pre-clamp sum reflect the supplied
bounds and anti-windup history. Adding a raw-output field alone would not yield
limit-independent demand. Any broader demand/tracking contract needs separate
design and controller validation; no shared dc_pid change is part of this PR.

## Asset and live-update review

All five review findings were confirmed against the PR head and pinned sources:

- ESP-IDF v5.3 `tools/cmake/scripts/data_file_embed_asm.cmake` appends a NUL for
  TEXT **before** emitting `_binary_*_end`. The response helper now strips an
  existing trailing NUL, handles empty spans without underflow, and propagates
  header errors. Host coverage checks exact sent lengths and cache headers.
- The controller-demand finding is resolved by the semantics above.
- `/diag` restores explicit `db_theme=light|dark` before rendering. Otherwise it
  follows the system theme, including when storage is unavailable. Plot colors
  use the same override as the page.
- EventSource retains automatic reconnection on transient errors. A terminal
  CLOSED state or three errors without a received update closes it and starts
  polling once. Successful updates reset that budget; queued events after the
  switch cannot create a second update path. Unsupported EventSource also
  falls back. Polling remains a read-only 2 s loop.
- HTML stays `no-store`. JS uses `public, max-age=60, must-revalidate` because
  its URLs remain stable across firmware images. This permits caching while
  bounding stale JS to 60 seconds on reload after OTA. Long-lived `immutable`
  caching would be inappropriate without versioned URLs. An already open page
  still needs a reload to load new firmware assets.

No physical-HIL validation is implied by host tests or safe-HIL builds.
