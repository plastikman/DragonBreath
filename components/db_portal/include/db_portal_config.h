// SPDX-License-Identifier: MIT
#pragma once

#include "db_klipper_mqtt.h"
#include "dc_bambu.h"
#include "dc_moonraker.h"
#include "dc_source.h"
#include "pb_ha.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool present;
    const char *value;
} db_portal_text_value_t;

typedef struct {
    bool present;
    uint16_t value;
} db_portal_port_value_t;

typedef struct {
    bool present;
    bool value;
} db_portal_bool_value_t;

typedef struct {
    bool source_present;
    dc_ctl_source_t source;
    db_portal_text_value_t mr_host, mr_key;
    db_portal_port_value_t mr_port;
    db_portal_text_value_t bb_host, bb_serial, bb_code;
    db_portal_text_value_t ha_host, ha_user, ha_pass, ha_topic;
    db_portal_port_value_t ha_port;
    db_portal_text_value_t km_host, km_user, km_pass, km_inst, km_topic;
    db_portal_port_value_t km_port;
    db_portal_bool_value_t km_tls, km_writeback;
    db_portal_text_value_t pr_host, pr_key;      // Prusa host + API key (handled inline
                                                 // in apply_product, not the shared planner)
} db_portal_product_request_t;

typedef struct {
    dc_ctl_source_t source;
    dc_moonraker_config_t moonraker;
    dc_bambu_config_t bambu;
    pb_ha_config_t ha;
    db_km_config_t klipper_mqtt;
    bool source_changed;
    bool moonraker_changed;
    bool bambu_changed;
    bool ha_changed;
    bool klipper_mqtt_changed;
} db_portal_product_plan_t;

// Build a complete, validated save plan without mutating runtime or NVS state.
// Empty secret fields deliberately retain the stored credential.
esp_err_t db_portal_plan_product_save(
    const db_portal_product_request_t *request,
    dc_ctl_source_t current_source,
    const dc_moonraker_config_t *current_moonraker,
    const dc_bambu_config_t *current_bambu,
    const pb_ha_config_t *current_ha,
    const db_km_config_t *current_klipper_mqtt,
    db_portal_product_plan_t *plan,
    char *message,
    size_t message_size);
