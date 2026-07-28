#pragma once
// Home Assistant MQTT client. Unlike Klipper/Bambu, HA is a CONTROLLER, not a
// bed-temp source: it connects to the user's MQTT broker, publishes MQTT-Discovery
// configs (a climate entity + temperature sensors auto-appear in HA), publishes a
// retained state topic, and subscribes command topics that map to ordinary
// pb_policy commands (the same ones any web/klipper client issues). Heat control
// holds a device-issued lease and heartbeats it, like any remote controller. It
// does NOT feed the AUTO bed-follow seam — there is no printer bed in HA mode.
// See plans/control-source-bambu-ha.md.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PB_HA_DISABLED,      // no config saved / source not selected
    PB_HA_DISCONNECTED,  // config present, not currently connected
    PB_HA_CONNECTING,
    PB_HA_CONNECTED,     // broker session up (publishing/subscribing)
} pb_ha_state_t;

typedef struct {
    char     host[64];   // broker IP/hostname; empty string = unconfigured
    uint16_t port;       // defaults to 1883 if 0
    char     user[32];   // broker username (optional)
    char     pass[64];   // broker password (optional)
    char     topic[48];  // topic prefix (defaults to "dragonbreath")
} pb_ha_config_t;

typedef struct {
    pb_ha_state_t state;
    bool          connected;   // convenience: state == PB_HA_CONNECTED
} pb_ha_status_t;

esp_err_t pb_ha_start(void);

// Periodic pump: publishes retained state (~2 s) and heartbeats the heat lease
// (~5 s, well under the comms-watchdog minimum). Call from the control loop when
// HA is the active source. No-op if the client isn't running.
void pb_ha_tick(void);

esp_err_t pb_ha_set_config(const pb_ha_config_t *cfg);
esp_err_t pb_ha_get_config(pb_ha_config_t *out);
esp_err_t pb_ha_get_status(pb_ha_status_t *out);

// Wipe saved HA config (factory reset).
esp_err_t pb_ha_clear_config(void);
