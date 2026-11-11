# Read-only ADC capture (DragonBreath #122)

This diagnostic is **not** a safety mechanism or a temperature reference.
It records the same chamber and PTC ADC samples that the existing 500 ms
heater safety tick already consumes. Sampling frequency, conversion,
averaging, trip logic, SSR control, calibration, and mode policy are unchanged.

## Capturing

`GET /api/v2/adc-capture` returns a CSV file containing a rolling 256-entry
history, oldest first. With two channels sampled every ~500 ms, this is
approximately **64 seconds** of history. A periodic anomaly with a 37–57 s
cycle may require multiple overlapping exports. Captures are RAM-only and reset at
reboot. They keep collecting while the device is unarmed and while faulted;
there is **no start, stop, or rate-setting endpoint**.

Access requires the same `X-DragonBreath-Auth` header as the existing
`/api/v2/console` endpoint. The request never samples the ADC directly.
If the device uses the safe HIL/devboard backend, the CSV contains only the
header because the backend has no real ADC.

For longer passive captures, poll **no slower than every 30 seconds** and
stitch by the tuple (device identity, boot ID, timestamp_us, channel). Exports
overlap intentionally; deduplicate matching records without inventing missing
samples. Polls spanning a reboot must remain separate. The boot ID and capture
uptime are provided as response headers, not additional CSV columns.

For a unit suspected of electrical coupling or insulation failure, **do not
energize or connect it to mains simply to capture more data**. A qualified
technician should assess it. A safe bench may use USB-only capture with the
mains disconnected, subject to the unit's electrical condition.

CSV columns:

- `timestamp_us`: monotonic microseconds since boot when the read began,
  not wall-clock time
- `channel`: logical `chamber` or `ptc`, after user-selected swap mapping
- `source_index`: physical ADC input index 0 or 1, before mapping
- `raw`: 12-bit ADC count, or `-1` if acquisition failed
- `millivolts`: ESP-IDF calibrated ADC pin voltage, or `-1` if unavailable
- `temperature_c`: instantaneous temperature after the persisted per-channel
  calibration offset (blank for non-OK); this is **not** a separately verified
  physical element temperature
- `status`: `ok`, `open`, `short`, or `uninit`
- `rref_kohm`: resolved reference resistor value

Capture-response provenance headers:

- `X-DragonBreath-Boot-ID`: per-boot identity (compare across exports)
- `X-DragonBreath-Firmware`: running firmware version
- `X-DragonBreath-Capture-Uptime-Us`: monotonic uptime at export snapshot
- `X-DragonBreath-Chamber-Offset-C`: current configured chamber offset
- `X-DragonBreath-PTC-Offset-C`: current configured PTC offset

The offset headers describe configuration **at export time**, not necessarily
at every historic sample. A calibration update between acquisition and export
can therefore make them differ from the offset applied to an older sample.
Use the CSV values as original observations, not reconstructed measurements.
The HTTP response must be saved with its headers for provenance; a plain CSV
file alone cannot preserve this metadata.

## Limits and interpretation

The 2 Hz per-channel cadence cannot resolve 50/60 Hz noise directly or prove
mains coupling. It exposes whether the apparent temperature oscillation is
already present at raw ADC counts, at calibrated voltage, or introduced
downstream by conversion. It can retain short fault-adjacent traces if exported
promptly; it is not durable event logging.

If higher-rate waveforms are needed, design and review a *separate* opt-in
acquisition path that explicitly owns ADC access, has bounded CPU/heap costs,
and demonstrates that safety-loop deadlines and trip behavior are unchanged.
Do not assume concurrent ADC reads are harmless.

Even a clear command/constraint log does not verify physical SSR contacts or
element temperature. Independent electrical and thermal measurements remain
necessary.

## Validation

`sh tests/run_ntc_diag_host_test.sh` checks partial/exact-fill snapshots,
chronology, rollover, bounded
snapshots, and invalid sample retention. The ESP-IDF builds in
`.github/workflows/firmware-build.yml` must cover production and HIL variants.
Neither proves electrical measurement integrity on hardware.
