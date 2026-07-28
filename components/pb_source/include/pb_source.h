#pragma once
// Control-source selector. The device binds to EXACTLY ONE printer/controller at
// a time (never several). Klipper (Moonraker) is the default and the shipped
// path; Bambu and Home Assistant are optional parity sources. The choice is
// persisted in NVS (app_nvs / "ctl_src") and read once at boot by app_main, which
// starts only the selected client. See plans/control-source-bambu-ha.md.
#include "esp_err.h"
#include <stdint.h>

typedef enum {
    PB_SRC_KLIPPER = 0,   // Moonraker WebSocket — default, the real target
    PB_SRC_BAMBU   = 1,   // Bambu LAN MQTT bed-follow (read-only)
    PB_SRC_HA      = 2,   // Home Assistant MQTT — HA is the controller
} pb_ctl_source_t;

// Persisted control source. Returns PB_SRC_KLIPPER if unset or out of range
// (fail-safe to the shipped path).
pb_ctl_source_t pb_source_get(void);

// Persist the control source. Takes effect on the next boot (app_main starts the
// selected client at bring-up).
esp_err_t pb_source_set(pb_ctl_source_t src);

const char *pb_source_str(pb_ctl_source_t src);
