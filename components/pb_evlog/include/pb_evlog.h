#pragma once

// Small in-memory ring buffer of human-readable event lines so the portal can
// show what's been happening without the user needing serial access. Nothing
// clever: fixed slot count, oldest overwritten first, snapshot copied under a
// lock. Add is safe from any task including ISRs-that-can-take-a-mutex (which
// means: not real ISRs — this is a coarse-grained mutex, not a spinlock).

#include <stdint.h>
#include <stddef.h>

#define PB_EVLOG_MAX_ENTRIES  64
#define PB_EVLOG_TEXT_BYTES   96

typedef struct {
    uint32_t ms;                    // millis since boot
    char     text[PB_EVLOG_TEXT_BYTES];
} pb_evlog_entry_t;

// Initialise the log. Safe to call before any producer. If already initialised,
// no-op.
void pb_evlog_init(void);

// Append a printf-formatted line. Truncated to PB_EVLOG_TEXT_BYTES-1. Safe to
// call from any task once pb_evlog_init has been called; also safe to call
// before init (silently drops the line — no crash).
void pb_evlog_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copy up to `max` most-recent entries into `out`, newest first. Returns how
// many were copied.
size_t pb_evlog_snapshot(pb_evlog_entry_t *out, size_t max);

// ---- firmware console capture (raw ESP_LOGx stream) ----
// Separate from the curated event ring above: a fixed byte ring that captures the
// full ESP_LOGx output via an esp_log_set_vprintf hook while still forwarding to
// UART. Motivation: newer Panda hardware has no external USB, and release builds
// repurpose the UART TX pin (GPIO21) as the Power LED — so the serial console is
// otherwise unreachable. Served by the /console page.
#define PB_EVLOG_CONSOLE_BYTES  16384   // ~180 lines; ~3% of worst-case free heap

// Install the log-capture hook. Call ONCE, as early as possible in app_main, so
// the boot log is captured. Idempotent. Uses a spinlock (safe from any context);
// never logs from within the hook (no recursion).
void pb_evlog_console_init(void);

// Copy the console ring, oldest -> newest, into `out` (always NUL-terminated).
// Returns bytes written (excluding the NUL). `max` should be >= the ring size + 1.
size_t pb_evlog_console_snapshot(char *out, size_t max);
