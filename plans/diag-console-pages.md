# RFC: `/diag` and `/console` web pages

Status: **Draft / design.** Two read-only, non-interactive web pages served by the
device — a diagnostics telemetry view and a firmware console log — themed to match
`/fw` and `/setup`, each with a `[Download]` button. No on-device persistence.

## Motivation

The new BIGTREETECH Panda revision **has no external USB port** — the first
DragonBreath install means opening the unit, and after that there is **no way to
reach the serial console without opening it again**. On top of that, **release
builds repurpose the UART TX pin (GPIO21) as the Power LED** (`CONFIG_PB_POWER_LED`),
so a shipped device effectively has *no* serial console at all: `ESP_LOGx` output is
generated but goes nowhere reachable.

So the only practical way to see what the firmware is doing — boot sequence,
Wi-Fi/mDNS/broker chatter, warnings, a fault's context — is **over the web**. That
is what `/console` is for. `/diag` is the companion: a live telemetry view (the
`tools/diag.py` logger, in the browser) so you don't have to run a host script.

## What already exists (so we don't rebuild it)

- **`/api/v2/state` + SSE `/api/v2/events`** — full telemetry (chamber, PTC/element,
  SSR output, mode, fault+reason, sensor status), pushed ~every 2 s and on every
  transition. `tools/diag.py` is just a 2 Hz poll of this plus a client-side running
  peak + CSV.
- **`/api/v2/logs`** — the **event ring** (`pb_evlog`, 64 × 96 B `{ms,text}`), the
  *curated* notable events (mode/fault transitions). **Already rendered** in the
  dashboard's Settings → "Event log". This is NOT the raw `ESP_LOGx` stream — nothing
  captures that today.

Implication: `/diag` is almost entirely a **new front-end over existing data**;
`/console` needs a **new capture path** (the raw log stream is not retained anywhere).

## Feature 1 — `/diag` (telemetry view)

Client-side page, **zero new device RAM**. Reuses SSE + `/api/v2/info` (for
`rref_kohm`). Presents what `diag.py` prints:

- chamber temp, element (PTC) temp, SSR output, mode, fault + reason, sensor status,
- **running peak element temp** (computed in-browser since page open),
- a live trend (reuse the dashboard's chart),
- **`[Download]` → CSV** of everything accumulated in the browser since the page was
  opened (same columns as `diag.py`: `t_s,chamber_c,ptc_c,ptc_status,out,mode,fault,reason,peak_c`).
  Generated in-page; nothing is stored on the device.

Caveat: it's a *live* view at SSE cadence (~2 s + transitions). It cannot back-fill
history from before the page opened — the device keeps no telemetry log (only the
64-event ring). Reviewing a *past* print's full time-series would require on-device
flash logging, which is explicitly **out of scope** (see below).

## Feature 2 — `/console` (firmware log stream)

The genuinely new capability: capture the `ESP_LOGx` stream into a RAM ring and serve
it.

- **Capture:** install an `esp_log_set_vprintf` hook that (a) formats the line into a
  fixed **static byte ring** and (b) forwards to the original vprintf so UART output
  (where wired) is unchanged. Byte ring (not fixed line slots) to avoid per-line
  padding waste and handle variable-length lines.
- **Size:** **16 KB** (~180 lines). Static allocation, never grows. See RAM analysis.
- **Boot capture:** initialise the ring + hook **very early in `app_main`**, before
  other subsystem init, so the **boot sequence is captured** (the whole point on a
  no-USB device).
- **Concurrency:** the hook runs from many task contexts and must be **lock-safe,
  non-blocking, and non-recursive** (never log from inside the hook; guard the ring
  with a portMUX spinlock or a lock-free single-writer discipline). It must not call
  anything that can block or re-enter the logger.
- **Endpoint:** `GET /api/v2/console` returns the ring as `text/plain` (oldest →
  newest, wrap handled). The page polls it (~1–2 s; SSE is possible later but polling
  is simpler and the volume is low).
- **`[Download]`** → save the current ring as `dragonbreath-console.txt`.

## RAM analysis (measured on the bench)

The `/diag` view costs zero device RAM. The console ring is the only new cost.

| Mode | Transport | Free heap | Min watermark |
|---|---|---|---|
| Klipper | WebSocket (no TLS) | ~156 KB | **~121 KB** (14 h uptime) |
| HA | plain MQTT :1883 (no TLS) | ~153 KB | ~142 KB (fresh boot) |
| Bambu | MQTT-over-TLS (mbedTLS) | — | **not yet measurable** (no printer) |

Bambu is the RAM worst case (only mode pulling in mbedTLS); a TLS 1.2 handshake peaks
~25–45 KB. Pessimistically: ~121 KB − ~45 KB TLS − 16 KB ring ≈ **~60 KB free** at the
worst instant — comfortable. A 16 KB static ring is a fixed, one-time reduction and
does not interact with the TLS spike dynamically. **RAM is not a constraint**; re-check
the Bambu/TLS headroom on hardware once a tester has a printer.

## Shared scaffolding

Both pages reuse the existing themed chrome (`PAGE_HEAD` / `PAGE_HDR` / `WRAP_OPEN`,
light-dark tokens, product header, `DB_AUTH_JS`) exactly like `/fw` and `/setup`.
Registered as `GET /diag` and `GET /console` in `pb_portal`. Factor the shared bits
while adding `/diag` first.

## Decisions

- **Ring size:** 16 KB (~180 lines) — headroom is ample and more scrollback is more
  useful for a boot/debug log reachable only over the web.
- **Auth:** `/diag` **open** (telemetry only, same as the dashboard reads).
  `/console` **gated behind the control token** — the raw log is chattier (IPs,
  Wi-Fi/mDNS detail) and it's a debug tool; falls back to open when no token is set,
  consistent with the rest of the UI.
- **Non-interactive:** both are strictly read-only. No command input on `/console`,
  so there is no injection surface.
- **Capture scope:** tee whatever the build already logs (release = INFO and up);
  no separate log-level control in v1.

## Implementation plan

1. **`/diag`** (front-end only, zero firmware risk): shared page scaffolding + the
   SSE-driven telemetry view + client-side CSV download.
2. **`/console`**:
   - `pb_evlog` (or a new `pb_conlog`) gains a 16 KB byte ring + `esp_log_set_vprintf`
     hook, initialised early in `app_main`.
   - `GET /api/v2/console` (auth-gated) returns the ring as text.
   - `/console` page (themed, polling, auto-scroll, `[Download]`).
3. Docs: `FEATURES.md` / `api-v2.md` note the two pages + the `/api/v2/console`
   endpoint; mention the no-USB / TX=LED rationale.

## Out of scope

- **On-device persistence / flash logging.** Neither page stores data; both are live
  windows (telemetry since page-open; the rolling 16 KB log ring). Reviewing a past
  print's full history is a separate, larger design (wear, flash budget) and is not
  part of this.
- Interactive console / command input.
- An SSE log stream (polling is enough for v1; can add later).

## Open questions

- Confirm the `esp_log_set_vprintf` hook composes cleanly with the existing UART
  console and the `CONFIG_PB_POWER_LED` TX-repurposing (verify on a release build).
- Ring size final (16 KB proposed) vs. a `Kconfig`-tunable.
- Whether `/console` should also fold in the `pb_evlog` curated events inline, or keep
  them separate (Settings already shows the event ring).
