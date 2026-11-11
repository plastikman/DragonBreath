# Read-only ADC capture (DragonBreath #122)

This diagnostic is **not** a safety mechanism or a temperature reference.
It records the same chamber and PTC ADC samples that the existing 500 ms
heater safety tick already consumes. Sampling frequency, conversion,
averaging, trip logic, SSR control, calibration, and mode policy are unchanged.

## Capturing

`GET /api/v2/adc-capture` returns a CSV file containing a rolling 256-entry
history, oldest first. With two channels sampled every ~500 ms, this is
approximately **64 seconds** of history. Captures are RAM-only and reset at
reboot. They keep collecting while the device is unarmed and while faulted;
there is **no start, stop, or rate-setting endpoint**.

Access requires the same `X-DragonBreath-Auth` header as the existing
`/api/v2/console` endpoint. The request never samples the ADC directly.
If the device uses the safe HIL/devboard backend, the CSV contains only the
header because the backend has no real ADC.

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
- `temperature_c`: instantaneous calibrated temperature (blank for non-OK)
- `status`: `ok`, `open`, `short`, or `uninit`
- `rref_kohm`: resolved reference resistor value

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

`sh tests/run_ntc_diag_host_test.sh` checks chronology, rollover, bounded
snapshots, and invalid sample retention. The ESP-IDF builds in
`.github/workflows/firmware-build.yml` must cover production and HIL variants.
Neither proves electrical measurement integrity on hardware.
