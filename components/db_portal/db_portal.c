// SPDX-License-Identifier: MIT
#include "db_portal.h"
#include "db_portal_config.h"

#include "db_klipper_mqtt.h"
#include "dc_bambu.h"
#include "dc_prusa.h"
#include "dc_moonraker.h"
#include "dc_portal.h"
#include "dc_source.h"
#include "pb_ha.h"
#include "pb_httpd.h"
#include "pb_policy.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "db_portal";

#define DB_NVS_NAMESPACE "app_nvs"
#define DB_NVS_KEY_BAMBU_CHAMBER_CTL "bb_ch_ctl"

static bool bambu_direct_chamber_control_get(void)
{
    uint8_t enabled = 0;   // opt-in: absent key means local DragonBreath regulation
    nvs_handle_t h;
    if (nvs_open(DB_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, DB_NVS_KEY_BAMBU_CHAMBER_CTL, &enabled);
        nvs_close(h);
    }
    return enabled != 0;
}

static esp_err_t bambu_direct_chamber_control_set(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, DB_NVS_KEY_BAMBU_CHAMBER_CTL, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// A zero-length chunk terminates an ESP-IDF chunked response, so skip empty text.
#define SEND(req, text) do { \
    const char *chunk_ = (text); \
    if (chunk_ && chunk_[0]) \
        httpd_resp_send_chunk((req), chunk_, HTTPD_RESP_USE_STRLEN); \
} while (0)

extern const unsigned char favicon_png_start[] asm("_binary_favicon_png_start");
extern const unsigned char favicon_png_end[] asm("_binary_favicon_png_end");
extern const unsigned char diagnostics_html_start[] asm("_binary_diagnostics_html_start");
extern const unsigned char diagnostics_html_end[] asm("_binary_diagnostics_html_end");
extern const unsigned char consequential_toggle_js_start[] asm("_binary_consequential_toggle_js_start");
extern const unsigned char consequential_toggle_js_end[] asm("_binary_consequential_toggle_js_end");

// ---- product-local diagnostics page (/diag) ---------------------------------
// The shared dc_ui SPA owns the dashboard and dc_portal owns the generic /console;
// /diag is device-specific (chamber/element/PTC/SSR/fault chain) so it stays here,
// rendering DragonBreath's own /api/v2 telemetry. Compact copy of the portal chrome
// since dc_portal owns the shared head. Restored after the pb_portal->db_portal
// extraction dropped it.

static esp_err_t embedded_text(httpd_req_t *req, const char *content_type,
                               const unsigned char *start,
                               const unsigned char *end)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t diag_page(httpd_req_t *req)
{
    return embedded_text(req, "text/html; charset=utf-8",
                         diagnostics_html_start, diagnostics_html_end);
}

static esp_err_t consequential_toggle_js_get(httpd_req_t *req)
{
    return embedded_text(req, "text/javascript; charset=utf-8",
                         consequential_toggle_js_start,
                         consequential_toggle_js_end);
}

static cJSON *field(const char *key, const char *label, const char *type,
                    const char *value, bool secret)
{
    cJSON *f = cJSON_CreateObject();
    if (!f) return NULL;
    cJSON_AddStringToObject(f, "key", key);
    cJSON_AddStringToObject(f, "label", label);
    cJSON_AddStringToObject(f, "type", type);
    cJSON_AddStringToObject(f, "value", secret ? "" : (value ? value : ""));
    if (secret) cJSON_AddBoolToObject(f, "secret", true);
    return f;
}

static cJSON *boolean_field(const char *key, const char *label, bool value)
{
    cJSON *f = field(key, label, "select", value ? "1" : "0", false);
    cJSON *opts = cJSON_AddArrayToObject(f, "options");
    const char *values[] = {"1", "0"};
    const char *labels[] = {"Enabled", "Disabled"};
    for (int i = 0; i < 2; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "value", values[i]);
        cJSON_AddStringToObject(o, "label", labels[i]);
        cJSON_AddItemToArray(opts, o);
    }
    return f;
}

static cJSON *section(cJSON *root, const char *title)
{
    cJSON *sections = cJSON_GetObjectItem(root, "sections");
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "title", title);
    cJSON_AddItemToObject(s, "fields", cJSON_CreateArray());
    cJSON_AddItemToArray(sections, s);
    return s;
}

static void add_field(cJSON *s, cJSON *f)
{
    cJSON_AddItemToArray(cJSON_GetObjectItem(s, "fields"), f);
}

// Reveal a section only when the control-source selector equals this value. Sections
// without it always render; HA is intentionally left always-on since it carries over
// as read-only telemetry alongside any selected control source.
static void visible_when(cJSON *s, const char *field_key, const char *value)
{
    cJSON *vw = cJSON_AddObjectToObject(s, "visible_when");
    cJSON_AddStringToObject(vw, "field", field_key);
    cJSON_AddStringToObject(vw, "value", value);
}

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "sections", cJSON_CreateArray());

    cJSON *s = section(root, "Control source");
    char selected_source[4];
    snprintf(selected_source, sizeof selected_source, "%d", dc_source_get());
    cJSON *src = field("ctl_src", "Source", "select", selected_source, false);
    cJSON *opts = cJSON_AddArrayToObject(src, "options");
    static const char *labels[] = {"Klipper (Moonraker)", "Bambu LAN", "Home Assistant", "None", "Klipper MQTT", "Prusa (PrusaLink)"};
    for (int i = 0; i < DC_SRC_MAX; i++) {
        cJSON *o = cJSON_CreateObject();
        char value[4];
        snprintf(value, sizeof value, "%d", i);
        cJSON_AddStringToObject(o, "value", value);
        cJSON_AddStringToObject(o, "label", labels[i]);
        cJSON_AddItemToArray(opts, o);
    }
    add_field(s, src);

    dc_moonraker_config_t mr = {0};
    dc_moonraker_get_config(&mr);
    char port[8]; snprintf(port, sizeof port, "%u", mr.port ? mr.port : 7125);
    s = section(root, "Klipper / Moonraker");
    visible_when(s, "ctl_src", "0");
    add_field(s, field("mr_host", "Host", "text", mr.host, false));
    add_field(s, field("mr_port", "Port", "number", port, false));
    add_field(s, field("mr_key", "API key (leave blank to keep)", "password", "", true));

    dc_bambu_config_t bb = {0};
    dc_bambu_get_config(&bb);
    s = section(root, "Bambu LAN");
    visible_when(s, "ctl_src", "1");
    cJSON_AddStringToObject(s, "description",
        "Experimental: when enabled, DragonBreath regulates from the printer-reported chamber temperature. The local chamber NTC and PTC remain authoritative for thermal limits, sensor faults, and emergency shutdown.");
    add_field(s, field("bb_host", "Printer host", "text", bb.host, false));
    add_field(s, field("bb_serial", "Serial", "text", bb.serial, false));
    add_field(s, field("bb_code", "Access code (leave blank to keep)", "password", "", true));
    add_field(s, boolean_field("bb_chamber_ctl", "Use Bambu chamber sensor for heater control",
                               bambu_direct_chamber_control_get()));
    // LAN discovery: the shared SPA renders a Search picker from this block and
    // fills bb_host + bb_serial, so the user only enters the access code. This MUST
    // stay attached to the Bambu section's `s` — keep it before the next section().
    cJSON *disc = cJSON_AddObjectToObject(s, "discovery");
    cJSON_AddStringToObject(disc, "scan", "/api/v2/bambu/scan");
    cJSON_AddStringToObject(disc, "endpoint", "/api/v2/bambu/discovered");
    cJSON_AddStringToObject(disc, "list", "printers");
    cJSON_AddStringToObject(disc, "label", "Search for printers on the network");
    cJSON *bb_fill = cJSON_AddObjectToObject(disc, "fill");   // discovered key -> field key
    cJSON_AddStringToObject(bb_fill, "host", "bb_host");
    cJSON_AddStringToObject(bb_fill, "serial", "bb_serial");

    dc_prusa_config_t pr = {0};
    dc_prusa_get_config(&pr);
    s = section(root, "Prusa (PrusaLink)");   // no discovery: PrusaLink has no SSDP scan
    visible_when(s, "ctl_src", "5");
    cJSON_AddStringToObject(s, "description",
        "Bed-follow: DragonBreath polls the printer's PrusaLink API and, in AUTO, heats the "
        "chamber once the printer's bed setpoint reaches your threshold (PrusaLink reports no "
        "filament type). Set the bed threshold and chamber target on the dashboard's Auto "
        "card. The API key is the printer's PrusaLink password.");
    add_field(s, field("pr_host", "Printer host", "text", pr.host, false));
    add_field(s, field("pr_key", "API key / PrusaLink password (leave blank to keep)", "password", "", true));

    pb_ha_config_t ha = {0};
    pb_ha_get_config(&ha);
    snprintf(port, sizeof port, "%u", (unsigned)(ha.port ? ha.port : 1883));
    s = section(root, "Home Assistant MQTT");
    add_field(s, field("ha_host", "Broker host", "text", ha.host, false));
    add_field(s, field("ha_port", "Port", "number", port, false));
    add_field(s, field("ha_user", "Username", "text", ha.user, false));
    add_field(s, field("ha_pass", "Password (leave blank to keep)", "password", "", true));
    add_field(s, field("ha_topic", "Topic prefix", "text", ha.topic, false));

    db_km_config_t km = {0};
    db_klipper_mqtt_get_config(&km);
    snprintf(port, sizeof port, "%u", (unsigned)(km.port ? km.port : (km.tls ? 8883 : 1883)));
    s = section(root, "Klipper MQTT");
    visible_when(s, "ctl_src", "4");
    cJSON_AddStringToObject(s, "description",
        "After saving, open /km-config to generate moonraker.conf, printer.cfg, and the Mosquitto ACL.");
    add_field(s, field("km_host", "Broker host", "text", km.host, false));
    add_field(s, field("km_port", "Port", "number", port, false));
    add_field(s, field("km_user", "Username", "text", km.user, false));
    add_field(s, field("km_pass", "Password (leave blank to keep)", "password", "", true));
    add_field(s, field("km_inst", "Moonraker instance", "text", km.inst, false));
    add_field(s, field("km_topic", "Device topic", "text", km.topic, false));
    add_field(s, boolean_field("km_tls", "Use TLS", km.tls));
    add_field(s, boolean_field("km_writeback", "Temperature writeback", km.writeback));
    return root;
}

static esp_err_t request_error(const char *key, char *message, size_t message_size)
{
    snprintf(message, message_size, "Invalid value for %s.", key);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t parse_text_field(const cJSON *values, const char *key,
                                  db_portal_text_value_t *out,
                                  char *message, size_t message_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    if (!v) return ESP_OK;
    if (!cJSON_IsString(v)) return request_error(key, message, message_size);
    out->present = true;
    out->value = v->valuestring;
    return ESP_OK;
}

static esp_err_t parse_port_field(const cJSON *values, const char *key,
                                  db_portal_port_value_t *out,
                                  char *message, size_t message_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    if (!v) return ESP_OK;
    long port = 0;
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 1 || v->valuedouble > UINT16_MAX ||
            v->valuedouble != (int)v->valuedouble)
            return request_error(key, message, message_size);
        port = v->valueint;
    } else if (cJSON_IsString(v)) {
        if (!v->valuestring[0]) return ESP_OK; // empty means retain the saved port
        char *end = NULL;
        port = strtol(v->valuestring, &end, 10);
        if (*end || port < 1 || port > UINT16_MAX)
            return request_error(key, message, message_size);
    } else {
        return request_error(key, message, message_size);
    }
    out->present = true;
    out->value = (uint16_t)port;
    return ESP_OK;
}

static esp_err_t parse_bool_field(const cJSON *values, const char *key,
                                  db_portal_bool_value_t *out,
                                  char *message, size_t message_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    if (!v) return ESP_OK;
    bool value;
    if (cJSON_IsBool(v)) {
        value = cJSON_IsTrue(v);
    } else if (cJSON_IsString(v) &&
               (!strcmp(v->valuestring, "true") || !strcmp(v->valuestring, "1") ||
                !strcmp(v->valuestring, "on"))) {
        value = true;
    } else if (cJSON_IsString(v) &&
               (!strcmp(v->valuestring, "false") || !strcmp(v->valuestring, "0") ||
                !strcmp(v->valuestring, "off"))) {
        value = false;
    } else {
        return request_error(key, message, message_size);
    }
    out->present = true;
    out->value = value;
    return ESP_OK;
}

static esp_err_t parse_source_field(const cJSON *values,
                                    db_portal_product_request_t *request,
                                    char *message, size_t message_size)
{
    const char *key = "ctl_src";
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    if (!v) return ESP_OK;
    long source = -1;
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0 || v->valuedouble >= DC_SRC_MAX ||
            v->valuedouble != (int)v->valuedouble)
            return request_error(key, message, message_size);
        source = v->valueint;
    } else if (cJSON_IsString(v)) {
        char *end = NULL;
        source = strtol(v->valuestring, &end, 10);
        if (!v->valuestring[0] || *end || source < 0 || source >= DC_SRC_MAX)
            return request_error(key, message, message_size);
    } else {
        return request_error(key, message, message_size);
    }
    request->source_present = true;
    request->source = (dc_ctl_source_t)source;
    return ESP_OK;
}

static esp_err_t parse_product_request(const cJSON *values,
                                       db_portal_product_request_t *request,
                                       char *message, size_t message_size)
{
    memset(request, 0, sizeof(*request));
    esp_err_t err = parse_source_field(values, request, message, message_size);
    if (err != ESP_OK) return err;

#define PARSE_TEXT(member) do { \
    err = parse_text_field(values, #member, &request->member, message, message_size); \
    if (err != ESP_OK) return err; \
} while (0)
#define PARSE_PORT(member) do { \
    err = parse_port_field(values, #member, &request->member, message, message_size); \
    if (err != ESP_OK) return err; \
} while (0)
#define PARSE_BOOL(member) do { \
    err = parse_bool_field(values, #member, &request->member, message, message_size); \
    if (err != ESP_OK) return err; \
} while (0)

    PARSE_TEXT(mr_host); PARSE_PORT(mr_port); PARSE_TEXT(mr_key);
    PARSE_TEXT(bb_host); PARSE_TEXT(bb_serial); PARSE_TEXT(bb_code);
    PARSE_TEXT(ha_host); PARSE_PORT(ha_port); PARSE_TEXT(ha_user);
    PARSE_TEXT(ha_pass); PARSE_TEXT(ha_topic);
    PARSE_TEXT(km_host); PARSE_PORT(km_port); PARSE_TEXT(km_user);
    PARSE_TEXT(km_pass); PARSE_TEXT(km_inst); PARSE_TEXT(km_topic);
    PARSE_BOOL(km_tls); PARSE_BOOL(km_writeback);
    PARSE_TEXT(pr_host); PARSE_TEXT(pr_key);

#undef PARSE_TEXT
#undef PARSE_PORT
#undef PARSE_BOOL
    return ESP_OK;
}

static esp_err_t persistence_error(const char *area, esp_err_t err,
                                   char *message, size_t message_size)
{
    snprintf(message, message_size, "Could not save %s settings: %s.",
             area, esp_err_to_name(err));
    return err;
}

static esp_err_t apply_product(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    dc_moonraker_config_t mr = {0};
    dc_bambu_config_t bb = {0};
    pb_ha_config_t ha = {0};
    db_km_config_t km = {0};
    esp_err_t err = dc_moonraker_get_config(&mr);
    if (err != ESP_OK) return persistence_error("Moonraker", err, message, message_size);
    err = dc_bambu_get_config(&bb);
    if (err != ESP_OK) return persistence_error("Bambu", err, message, message_size);
    err = pb_ha_get_config(&ha);
    if (err != ESP_OK) return persistence_error("Home Assistant", err, message, message_size);
    err = db_klipper_mqtt_get_config(&km);
    if (err != ESP_OK) return persistence_error("Klipper MQTT", err, message, message_size);
    dc_prusa_config_t pr = {0};
    err = dc_prusa_get_config(&pr);
    if (err != ESP_OK) return persistence_error("Prusa", err, message, message_size);

    // Parse and stage every field before the first setter can touch runtime/NVS.
    db_portal_product_request_t request;
    err = parse_product_request(values, &request, message, message_size);
    if (err != ESP_OK) return err;
    db_portal_bool_value_t bb_chamber_ctl = {0};
    err = parse_bool_field(values, "bb_chamber_ctl", &bb_chamber_ctl,
                           message, message_size);
    if (err != ESP_OK) return err;
    bool bb_chamber_ctl_changed =
        bb_chamber_ctl.present &&
        bb_chamber_ctl.value != bambu_direct_chamber_control_get();

    db_portal_product_plan_t plan;
    err = db_portal_plan_product_save(&request, dc_source_get(), &mr, &bb, &ha, &km, &pr,
                                      &plan, message, message_size);
    if (err != ESP_OK) return err;

    if (plan.moonraker_changed) {
        err = dc_moonraker_set_config(&plan.moonraker);
        if (err != ESP_OK) return persistence_error("Moonraker", err, message, message_size);
    }
    if (plan.bambu_changed) {
        err = dc_bambu_set_config(&plan.bambu);
        if (err != ESP_OK) return persistence_error("Bambu", err, message, message_size);
    }
    if (plan.ha_changed) {
        err = pb_ha_set_config(&plan.ha);
        if (err != ESP_OK) return persistence_error("Home Assistant", err, message, message_size);
    }
    if (plan.klipper_mqtt_changed) {
        err = db_klipper_mqtt_set_config(&plan.klipper_mqtt);
        if (err != ESP_OK) return persistence_error("Klipper MQTT", err, message, message_size);
    }
    if (plan.prusa_changed) {
        err = dc_prusa_set_config(&plan.prusa);
        if (err != ESP_OK) return persistence_error("Prusa", err, message, message_size);
    }
    if (bb_chamber_ctl_changed) {
        err = bambu_direct_chamber_control_set(bb_chamber_ctl.value);
        if (err != ESP_OK)
            return persistence_error("Bambu chamber control", err, message, message_size);
    }
    // Source is deliberately last: an invalid or failed config save can never bind
    // a different controller. Selecting None changes only this enum; credentials stay.
    if (plan.source_changed) {
        err = dc_source_set(plan.source);
        if (err != ESP_OK) return persistence_error("control source", err, message, message_size);
    }

    bool changed = plan.moonraker_changed || plan.bambu_changed || plan.ha_changed ||
                   plan.klipper_mqtt_changed || plan.prusa_changed ||
                   plan.source_changed || bb_chamber_ctl_changed;
    snprintf(message, message_size, changed
             ? "Configuration saved; restart to apply source/control changes."
             : "Configuration already up to date.");
    return ESP_OK;
}

static bool authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    return pb_httpd_auth_ok(req);
}

static esp_err_t guard_operation(dc_portal_operation_t operation, void *ctx,
                                 char *message, size_t message_size)
{
    (void)ctx;
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    if (snap.mode == PB_MODE_OFF && !snap.heater_output) return ESP_OK;
    snprintf(message, message_size, "Turn the heater off before %s.",
             operation == DC_PORTAL_OPERATION_OTA ? "updating" : "a factory reset");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t validate_image(const esp_app_desc_t *image, void *ctx,
                                char *message, size_t message_size)
{
    (void)ctx;
    if (!strcmp(image->project_name, "dragonbreath") || !strcmp(image->project_name, "panda_breath"))
        return ESP_OK;
    snprintf(message, message_size, "Not a DragonBreath or stock Panda Breath image.");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t factory_reset(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Product-local Klipper helper retained from the shipped portal. It deliberately
// emits no saved password, only placeholders and least-privilege ACL identities.
static esp_err_t km_config_get(httpd_req_t *req)
{
    db_km_config_t km = {0};
    esp_err_t err = db_klipper_mqtt_get_config(&km);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "could not read Klipper MQTT settings");
        return err;
    }
    const char *host = km.host[0] ? km.host : "mosquitto.lan";
    const char *user = km.user[0] ? km.user : "dragonbreath";
    const char *inst = km.inst[0] ? km.inst : "myprinter";
    const char *base = km.topic[0] ? km.topic : "dragonbreath";
    unsigned port = km.port ? km.port : (km.tls ? 8883U : 1883U);
    char moonraker_user[48];
    snprintf(moonraker_user, sizeof(moonraker_user), "%s_moonraker", user);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buffer[640];
    SEND(req,
        "# ===== DragonBreath - Klipper (MQTT) config =====\n"
        "# Generated from your /setup values. Paste each block into the named file,\n"
        "# restart Moonraker + Klipper, then select 'Klipper MQTT' as the control source.\n"
        "# Do NOT also run the native dragonbreath-klipper extra - pick one integration.\n\n");

    snprintf(buffer, sizeof(buffer),
        "########## moonraker.conf ##########\n"
        "[mqtt]\n"
        "address: %s\n"
        "port: %u\n"
        "username: %s\n"
        "password: <your broker password>\n"
        "instance_name: %s\n"
        "enable_moonraker_api: True\n"
        "publish_split_status: True\n"
        "status_objects:\n"
        "  gcode_macro DRAGONBREATH\n"
        "  gcode_macro DB_LINK\n\n",
        host, port, moonraker_user, inst);
    SEND(req, buffer);

    snprintf(buffer, sizeof(buffer),
             "[sensor %s]\ntype: mqtt\nname: DragonBreath\nstate_topic: %s/telemetry\n",
             base, base);
    SEND(req, buffer);
    SEND(req,
        "state_response_template:\n"
        "  {% set s = payload|fromjson %}\n"
        "  {set_result(\"chamber_temperature\", s[\"chamber_temperature\"]|float)}\n"
        "  {set_result(\"element_temperature\", s[\"element_temperature\"]|float)}\n"
        "parameter_chamber_temperature:\n  units=\xC2\xB0""C\n"
        "parameter_element_temperature:\n  units=\xC2\xB0""C\n\n");

    snprintf(buffer, sizeof(buffer),
             "[power %s]\ntype: mqtt\ncommand_topic: %s/power/set\n", base, base);
    SEND(req, buffer);
    snprintf(buffer, sizeof(buffer),
        "command_payload:\n  {command}\nstate_topic: %s/power/state\n"
        "state_response_template:\n  {payload}\noff_when_shutdown: True\n\n", base);
    SEND(req, buffer);

    SEND(req,
        "########## printer.cfg ##########\n"
        "[gcode_macro DRAGONBREATH]\n"
        "variable_seq: 0\nvariable_target: 0.0\nvariable_mode: \"off\"\n"
        "variable_fan: 0\nvariable_armed: 0\nvariable_purge_nonce: 0\n"
        "variable_temperature: -1.0\nvariable_humidity: -1.0\nvariable_fault: \"\"\ngcode:\n\n"
        "[gcode_macro DB_LINK]\nvariable_heartbeat: 0\ngcode:\n\n"
        "[delayed_gcode DB_HEARTBEAT]\ninitial_duration: 5\ngcode:\n"
        "  {% set hb = printer[\"gcode_macro DB_LINK\"].heartbeat|int %}\n"
        "  SET_GCODE_VARIABLE MACRO=DB_LINK VARIABLE=heartbeat VALUE={hb + 1}\n"
        "  UPDATE_DELAYED_GCODE ID=DB_HEARTBEAT DURATION=5\n\n");
    SEND(req,
        "# Arm + set chamber target. Writes all fields, then bumps seq LAST so the\n"
        "# device applies a coherent update. Call M141 in your filament/print START.\n"
        "[gcode_macro M141]\ngcode:\n"
        "  {% set s = params.S|default(0)|float %}\n"
        "  {% set m = \"heat\" if s > 0 else \"off\" %}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=target VALUE={s}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=mode VALUE='\"{m}\"'\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=armed VALUE={1 if s > 0 else 0}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=seq VALUE={printer[\"gcode_macro DRAGONBREATH\"].seq|int + 1}\n\n");
    SEND(req,
        "# M191 in MQTT mode sets the target but does NOT block (a true wait needs a\n"
        "# real Klipper sensor). To refuse printing without a chamber wait, comment out\n"
        "# this alias and uncomment the strict variant below.\n"
        "[gcode_macro M191]\ngcode:\n"
        "  {action_respond_info(\"M191: MQTT mode sets chamber target but does NOT wait.\")}\n"
        "  M141 S{params.S|default(0)}\n"
        "# [gcode_macro M191]\n# gcode:\n"
        "#   { action_raise_error(\"M191 unsupported in MQTT mode - use M141\") }\n\n");

    snprintf(buffer, sizeof(buffer),
        "########## mosquitto ACL (least privilege) ##########\n"
        "# Add/update both users (do not use -c on an existing password file):\n"
        "#   mosquitto_passwd /etc/mosquitto/passwd %s\n"
        "#   mosquitto_passwd /etc/mosquitto/passwd %s\n"
        "# DragonBreath device identity\n"
        "user %s\n"
        "topic write %s/telemetry\n"
        "topic write %s/power/state\n"
        "topic write %s/status\n"
        "topic read  %s/power/set\n",
        user, moonraker_user, user, base, base, base, base);
    SEND(req, buffer);
    snprintf(buffer, sizeof(buffer),
        "topic read  %s/moonraker/status\n"
        "topic read  %s/klipper/state/gcode_macro DRAGONBREATH/#\n"
        "topic read  %s/klipper/state/gcode_macro DB_LINK/#\n"
        "# writeback (only if you enabled it):\n"
        "topic write %s/moonraker/api/request\n",
        inst, inst, inst, inst);
    SEND(req, buffer);
    snprintf(buffer, sizeof(buffer),
        "\n# Moonraker identity (opposite direction on the same scoped topics)\n"
        "user %s\n"
        "topic read  %s/telemetry\n"
        "topic read  %s/power/state\n"
        "topic read  %s/status\n"
        "topic write %s/power/set\n",
        moonraker_user, base, base, base, base);
    SEND(req, buffer);
    snprintf(buffer, sizeof(buffer),
        "topic write %s/moonraker/status\n"
        "topic write %s/klipper/state/gcode_macro DRAGONBREATH/#\n"
        "topic write %s/klipper/state/gcode_macro DB_LINK/#\n"
        "topic read  %s/moonraker/api/request\n"
        "topic write %s/moonraker/api/response\n",
        inst, inst, inst, inst, inst);
    SEND(req, buffer);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    // These linker labels bound one target_add_binary_data blob.
    // cppcheck-suppress comparePointers
    const size_t length = (size_t)(favicon_png_end - favicon_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)favicon_png_start, length);
}

static esp_err_t send_json_obj(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

// POST /api/v2/bambu/scan — start a one-shot LAN scan (user-initiated only). The
// setup UI calls this when the operator opens/clicks Bambu setup, then polls GET.
static esp_err_t bambu_scan_post(httpd_req_t *req)
{
    dc_bambu_scan_start();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", dc_bambu_scanning());
    return send_json_obj(req, root);
}

// GET /api/v2/bambu/discovered — printers found by the most recent scan, plus
// whether a scan is still running, for the setup picker to fill host + serial.
static esp_err_t bambu_discovered_get(httpd_req_t *req)
{
    dc_bambu_found_t found[DC_BAMBU_DISCOVER_MAX];
    int n = dc_bambu_discover_get(found, DC_BAMBU_DISCOVER_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", dc_bambu_scanning());
    cJSON *arr = cJSON_AddArrayToObject(root, "printers");
    for (int i = 0; i < n; ++i) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "host", found[i].host);
        cJSON_AddStringToObject(p, "serial", found[i].serial);
        cJSON_AddStringToObject(p, "model", found[i].model);
        cJSON_AddStringToObject(p, "name", found[i].name);
        cJSON_AddNumberToObject(p, "age_s", found[i].age_s);
        cJSON_AddItemToArray(arr, p);
    }
    return send_json_obj(req, root);
}

// ---- Dashboard quick-control temperature presets (user-customizable) ----
// Four target temps shown as one-tap buttons on the dashboard. Persisted in NVS
// (app_nvs blob "quick_preset"); default 50/55/60/65. Clamped to the heater range.
#define QP_COUNT 4
static const uint8_t QP_DEFAULT[QP_COUNT] = { 50, 55, 60, 65 };

static void qp_load(uint8_t out[QP_COUNT])
{
    memcpy(out, QP_DEFAULT, QP_COUNT);
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READONLY, &h) == ESP_OK) {
        size_t len = QP_COUNT;
        nvs_get_blob(h, "quick_preset", out, &len);   // leaves defaults if absent
        nvs_close(h);
    }
}

static cJSON *qp_json(const uint8_t p[QP_COUNT])
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "presets");
    for (int i = 0; i < QP_COUNT; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(p[i]));
    return root;
}

// GET /api/v2/presets — the four dashboard quick-control temps.
static esp_err_t presets_get(httpd_req_t *req)
{
    uint8_t p[QP_COUNT];
    qp_load(p);
    return send_json_obj(req, qp_json(p));
}

// POST /api/v2/presets  body {"presets":[t0,t1,t2,t3]} — save + echo back (clamped).
static esp_err_t presets_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    char buf[257];
    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed"); return ESP_FAIL; }
    buf[r] = '\0';
    cJSON *body = cJSON_Parse(buf);
    cJSON *arr = body ? cJSON_GetObjectItemCaseSensitive(body, "presets") : NULL;
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != QP_COUNT) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "presets must be four numbers");
        return ESP_FAIL;
    }
    uint8_t p[QP_COUNT];
    for (int i = 0; i < QP_COUNT; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        int v = cJSON_IsNumber(e) ? e->valueint : 0;
        p[i] = (uint8_t)(v < 0 ? 0 : v > 70 ? 70 : v);   // heater hard ceiling is 70 C
    }
    cJSON_Delete(body);
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "quick_preset", p, QP_COUNT);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed"); return ESP_FAIL; }
    return send_json_obj(req, qp_json(p));
}

static esp_err_t register_product_routes(httpd_handle_t server, void *ctx)
{
    (void)ctx;
    esp_err_t err = pb_httpd_register(server);
    if (err != ESP_OK) return err;
    const httpd_uri_t presets_g = { .uri = "/api/v2/presets", .method = HTTP_GET, .handler = presets_get };
    err = httpd_register_uri_handler(server, &presets_g);
    if (err != ESP_OK) return err;
    const httpd_uri_t presets_p = { .uri = "/api/v2/presets", .method = HTTP_POST, .handler = presets_post };
    err = httpd_register_uri_handler(server, &presets_p);
    if (err != ESP_OK) return err;
    const httpd_uri_t bambu_scan = { .uri = "/api/v2/bambu/scan", .method = HTTP_POST, .handler = bambu_scan_post };
    err = httpd_register_uri_handler(server, &bambu_scan);
    if (err != ESP_OK) return err;
    const httpd_uri_t bambu_disc = { .uri = "/api/v2/bambu/discovered", .method = HTTP_GET, .handler = bambu_discovered_get };
    err = httpd_register_uri_handler(server, &bambu_disc);
    if (err != ESP_OK) return err;
    const httpd_uri_t km_config = { .uri = "/km-config", .method = HTTP_GET, .handler = km_config_get };
    err = httpd_register_uri_handler(server, &km_config);
    if (err != ESP_OK) return err;
    const httpd_uri_t diag = { .uri = "/diag", .method = HTTP_GET, .handler = diag_page };
    err = httpd_register_uri_handler(server, &diag);
    if (err != ESP_OK) return err;
    const httpd_uri_t consequential_toggle = {
        .uri = "/ui/consequential-toggle.js",
        .method = HTTP_GET,
        .handler = consequential_toggle_js_get,
    };
    err = httpd_register_uri_handler(server, &consequential_toggle);
    if (err != ESP_OK) return err;
    const httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get };
    return httpd_register_uri_handler(server, &favicon);
}

esp_err_t db_portal_start(void)
{
    httpd_config_t httpd = HTTPD_DEFAULT_CONFIG();
    httpd.lru_purge_enable = true;
    httpd.max_uri_handlers = 48;
    httpd.stack_size = 8192;
    httpd.keep_alive_enable = true;
    httpd.keep_alive_idle = 10;
    httpd.keep_alive_interval = 5;
    httpd.keep_alive_count = 3;

    const dc_portal_config_t cfg = {
        .product = "dragonbreath",
        .display_name = "DragonBreath",
        .register_product_routes = register_product_routes,
        .describe_product = describe_product,
        .apply_product = apply_product,
        .authorize = authorize,
        .guard_operation = guard_operation,
        .validate_image = validate_image,
        .factory_reset = factory_reset,
        .httpd_config = &httpd,
    };
    esp_err_t err = dc_portal_start(&cfg);
    if (err == ESP_OK) ESP_LOGI(TAG, "shared Dragon portal started");
    return err;
}
