#pragma once
// Bambu Lab LAN MQTT client. Connects to the printer's on-device broker in LAN
// mode (mqtts://<host>:8883, username "bblp", password = LAN access code; the
// self-signed cert has CN=serial while we connect by IP, so cert verification is
// relaxed — LAN read-only), subscribes device/<serial>/report, publishes one
// "pushall" on connect (required on P1/A1 which send deltas), and scans each
// report for bed_temper / chamber_temper. The cached bed temperature feeds the
// AUTO seam exactly as Moonraker does. Read-only: we never send control commands
// to the printer. UNTESTED against real hardware — for community validation (see
// plans/control-source-bambu-ha.md).
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PB_BAMBU_DISABLED,      // no config saved / source not selected
    PB_BAMBU_DISCONNECTED,  // config present, not currently connected
    PB_BAMBU_CONNECTING,
    PB_BAMBU_CONNECTED,     // MQTT+TLS session up, subscribe in flight
    PB_BAMBU_SUBSCRIBED,    // receiving report updates
} pb_bambu_state_t;

typedef struct {
    char host[64];    // printer IP/hostname; empty string = unconfigured
    char serial[32];  // printer serial (embedded in the MQTT topic path)
    char code[32];    // LAN access code (MQTT password)
} pb_bambu_config_t;

typedef struct {
    pb_bambu_state_t state;
    bool  connected;      // convenience: state == PB_BAMBU_SUBSCRIBED
    float bed_temp;       // bed_temper (°C); NaN until first report
    float chamber_temp;   // chamber_temper (°C); NaN if the model has no sensor
} pb_bambu_status_t;

esp_err_t pb_bambu_start(void);

// Overwrite saved config (NVS). The running client (once implemented) will
// reconnect with the new settings.
esp_err_t pb_bambu_set_config(const pb_bambu_config_t *cfg);

esp_err_t pb_bambu_get_config(pb_bambu_config_t *out);
esp_err_t pb_bambu_get_status(pb_bambu_status_t *out);

// Wipe saved Bambu config (factory reset).
esp_err_t pb_bambu_clear_config(void);
