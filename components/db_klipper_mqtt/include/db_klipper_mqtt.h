#pragma once
// Klipper-over-MQTT control source. For locked/managed Klipper installs that permit
// moonraker.conf + printer.cfg edits + a broker, but no klippy/extras plugin. Unlike
// PB_SRC_KLIPPER (Moonraker WebSocket bed-follow), this is a CONTROLLER: Klipper
// macros publish desired-state (target/mode/armed/seq) which Moonraker relays as MQTT
// split-status; the device holds a pb_policy lease and heartbeats it. Heat arming is
// governed by the retained-aware state machine in db_klipper_mqtt_arm.h (never arms
// from retained values — only on a fresh seq + a live heartbeat).
// See plans/mqtt-klipper-implementation-design.md.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    DB_KM_DISABLED,      // no config saved / source not selected
    DB_KM_DISCONNECTED,  // config present, not currently connected
    DB_KM_CONNECTING,
    DB_KM_CONNECTED,     // broker session up (subscribed)
} db_km_conn_t;

typedef struct {
    char     host[64];   // broker (Moonraker's) IP/hostname; empty = unconfigured
    uint16_t port;       // defaults to 1883 (8883 with TLS) if 0
    char     user[32];   // broker username (required by the validator)
    char     pass[64];   // broker password
    char     inst[48];   // Moonraker instance_name (required — derives all topics)
    char     topic[48];  // device topic base (defaults to "dragonbreath")
    bool     tls;        // mqtts (skip-verify LAN, like pb_bambu)
    bool     writeback;  // push live chamber temperature into a macro var (off by default)
} db_km_config_t;

typedef struct {
    db_km_conn_t conn;
    bool connected;      // conn == DB_KM_CONNECTED
    bool engaged;        // heat currently armed+driven by this source
    bool comms_lost;     // last disengage was a heartbeat-timeout
} db_km_status_t;

esp_err_t db_klipper_mqtt_start(void);

// Periodic pump (control task): evaluate the arming machine, apply/stop heat, publish
// telemetry + power state, heartbeat the lease. No-op if the client isn't running.
void db_klipper_mqtt_tick(void);

esp_err_t db_klipper_mqtt_set_config(const db_km_config_t *cfg);
esp_err_t db_klipper_mqtt_get_config(db_km_config_t *out);
esp_err_t db_klipper_mqtt_get_status(db_km_status_t *out);
esp_err_t db_klipper_mqtt_clear_config(void);
