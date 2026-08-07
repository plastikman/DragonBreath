// Home Assistant MQTT client. Connects to the user's broker, publishes
// MQTT-Discovery so a climate entity + temperature sensors auto-appear in HA,
// publishes a retained state topic, and maps command topics to pb_policy calls.
// Heat control holds a device-issued POWER_ON lease and heartbeats it like any
// remote controller. See plans/control-source-bambu-ha.md.
//
// Threading: the esp-mqtt event handler runs on the mqtt task (connect/discovery/
// subscribe + inbound commands); pb_ha_tick() runs on the control task (state
// publish + lease heartbeat). esp_mqtt_client_publish() is thread-safe; the shared
// lease/target state is guarded by s_lock.
#include "pb_ha.h"
#include "pb_policy.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_ha";

#define NVS_NS    "app_nvs"
#define KEY_HOST  "ha_host"
#define KEY_PORT  "ha_port"
#define KEY_USER  "ha_user"
#define KEY_PASS  "ha_pass"
#define KEY_TOPIC "ha_topic"

#define DEFAULT_PORT     1883
#define DEFAULT_PREFIX   "dragonbreath"
#define STATE_PERIOD_US  (2 * 1000 * 1000)   // publish state every ~2 s
#define HB_PERIOD_US     (5 * 1000 * 1000)   // heartbeat lease every ~5 s (<< watchdog min)
#define TARGET_MIN_C     20.0f
#define TARGET_MAX_C     70.0f

static SemaphoreHandle_t        s_lock   = NULL;
static pb_ha_config_t           s_cfg    = {0};
static pb_ha_status_t           s_status = { .state = PB_HA_DISABLED };
static esp_mqtt_client_handle_t s_client = NULL;

// Read-only mode: publish telemetry (sensors + state) but never subscribe to command
// topics and never take a pb_policy lease — used when HA runs ALONGSIDE another
// control source (Bambu/Klipper) purely as a monitor. Set once at start, before the
// client connects, so it's effectively immutable while the client runs.
static bool s_readonly = false;

// Shared control state (guarded by s_lock).
static bool               s_have_lease = false;
static pb_policy_lease_t  s_lease      = {0};
static float              s_desired_target = 45.0f;

// Pacing (control task only). s_pub_pending is a lock-guarded request from the MQTT
// callback (on connect) asking tick() to publish promptly — avoids the MQTT task
// writing the control-task pacing clock directly (cross-task data race).
static int64_t s_last_state_us = 0;
static int64_t s_last_hb_us    = 0;
static bool    s_pub_pending   = false;   // guarded by s_lock

// Availability (LWT) topic — must outlive esp_mqtt_client_init(), so it's static.
static char s_avail_topic[80] = {0};

static const char *prefix(void) { return s_cfg.topic[0] ? s_cfg.topic : DEFAULT_PREFIX; }

// ---------- NVS ----------

static esp_err_t nvs_load(pb_ha_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host = unconfigured

    uint16_t p = 0;
    if (nvs_get_u16(h, KEY_PORT, &p) == ESP_OK && p > 0) out->port = p;
    else out->port = DEFAULT_PORT;

    sz = sizeof(out->user);  nvs_get_str(h, KEY_USER, out->user, &sz);
    sz = sizeof(out->pass);  nvs_get_str(h, KEY_PASS, out->pass, &sz);
    sz = sizeof(out->topic); nvs_get_str(h, KEY_TOPIC, out->topic, &sz);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const pb_ha_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, cfg->port ? cfg->port : DEFAULT_PORT);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_USER, cfg->user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, cfg->pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_TOPIC, cfg->topic);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- control mapping ----------

// Turn heat ON at a target (idempotent target update). Takes a fresh lease and
// remembers it so pb_ha_tick() can heartbeat. Reuses DB_SOURCE_WEB (no new policy
// source) with owner "ha" for attribution.
static void start_heat(float target_c)
{
    if (target_c < TARGET_MIN_C) target_c = TARGET_MIN_C;
    if (target_c > TARGET_MAX_C) target_c = TARGET_MAX_C;
    pb_policy_lease_t lease = {0};
    pb_policy_result_t r = pb_policy_set_power_on(
        target_c, DB_SOURCE_WEB, "ha", PB_POLICY_REVISION_ANY, &lease);
    if (r == PB_POLICY_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_lease = lease;
        s_have_lease = true;
        s_desired_target = target_c;
        s_last_hb_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "HA -> heat %.0f C", target_c);
    } else {
        ESP_LOGW(TAG, "HA heat rejected (policy result %d)", (int)r);
    }
}

static void stop_heat(void)
{
    pb_policy_set_mode_off(DB_SOURCE_WEB);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_have_lease = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "HA -> off");
}

static void handle_cmd(const char *topic, int tlen, const char *data, int dlen)
{
    char t[96];
    int n = tlen < (int)sizeof(t) - 1 ? tlen : (int)sizeof(t) - 1;
    memcpy(t, topic, n); t[n] = '\0';
    char d[32];
    n = dlen < (int)sizeof(d) - 1 ? dlen : (int)sizeof(d) - 1;
    memcpy(d, data, n); d[n] = '\0';

    char mode_set[80], temp_set[80];
    snprintf(mode_set, sizeof mode_set, "%s/mode/set", prefix());
    snprintf(temp_set, sizeof temp_set, "%s/temp/set", prefix());

    if (strcmp(t, mode_set) == 0) {
        if (strcmp(d, "heat") == 0) {
            float tgt;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            tgt = s_desired_target;
            xSemaphoreGive(s_lock);
            start_heat(tgt);
        } else {   // "off" (or anything else -> safer off)
            stop_heat();
        }
    } else if (strcmp(t, temp_set) == 0) {
        float tgt = strtof(d, NULL);
        if (tgt > 0) start_heat(tgt);   // setting a target implies heat-on
    }
}

// ---------- publish ----------

static void publish_discovery(void)
{
    const char *p = prefix();
    char buf[1024];
    char topic[96];

    // Climate entity (controllable thermostat) — only in full-control mode. In
    // read-only mode we publish an EMPTY retained payload to the same config topic so
    // HA removes any thermostat left over from a previous full-control session (no
    // dead buttons); the device just reports via sensors below.
    snprintf(topic, sizeof topic, "homeassistant/climate/%s/config", p);
    if (s_readonly) {
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, 1);   // clear retained config
    } else {
        // Climate entity (HA MQTT discovery, abbreviated keys). State fields are read
        // from the single retained <p>/state JSON via templates.
        int cfg = snprintf(buf, sizeof buf,
            "{\"name\":\"DragonBreath\",\"uniq_id\":\"%s_climate\","
            "\"avty_t\":\"%s/availability\","
            "\"curr_temp_t\":\"%s/state\",\"curr_temp_tpl\":\"{{value_json.chamber}}\","
            "\"temp_stat_t\":\"%s/state\",\"temp_stat_tpl\":\"{{value_json.target}}\","
            "\"temp_cmd_t\":\"%s/temp/set\","
            "\"mode_stat_t\":\"%s/state\",\"mode_stat_tpl\":\"{{value_json.mode}}\","
            "\"mode_cmd_t\":\"%s/mode/set\","
            // temp_unit "C": our target/current values on the state + command topics are
            // Celsius. HA converts for display and converts a user's setpoint (e.g. °F on
            // an imperial system) back to °C before publishing to temp_cmd_t.
            "\"temp_unit\":\"C\","
            "\"modes\":[\"off\",\"heat\"],\"min_temp\":20,\"max_temp\":70,\"temp_step\":1,"
            "\"dev\":{\"ids\":[\"%s\"],\"name\":\"DragonBreath\",\"mdl\":\"Panda Breath\",\"mf\":\"DragonBreath\"}}",
            p, p, p, p, p, p, p, p);
        if (cfg > 0 && cfg < (int)sizeof buf)
            esp_mqtt_client_publish(s_client, topic, buf, 0, 1, 1);   // qos1, retain
    }

    // Chamber temperature sensor.
    snprintf(buf, sizeof buf,
        "{\"name\":\"DragonBreath Chamber\",\"uniq_id\":\"%s_chamber\","
        "\"avty_t\":\"%s/availability\",\"stat_t\":\"%s/state\","
        "\"val_tpl\":\"{{value_json.chamber}}\",\"unit_of_meas\":\"\xC2\xB0""C\","
        "\"dev_cla\":\"temperature\",\"dev\":{\"ids\":[\"%s\"]}}",
        p, p, p, p);
    snprintf(topic, sizeof topic, "homeassistant/sensor/%s_chamber/config", p);
    esp_mqtt_client_publish(s_client, topic, buf, 0, 1, 1);

    // Element (PTC) temperature sensor.
    snprintf(buf, sizeof buf,
        "{\"name\":\"DragonBreath Element\",\"uniq_id\":\"%s_ptc\","
        "\"avty_t\":\"%s/availability\",\"stat_t\":\"%s/state\","
        "\"val_tpl\":\"{{value_json.ptc}}\",\"unit_of_meas\":\"\xC2\xB0""C\","
        "\"dev_cla\":\"temperature\",\"dev\":{\"ids\":[\"%s\"]}}",
        p, p, p, p);
    snprintf(topic, sizeof topic, "homeassistant/sensor/%s_ptc/config", p);
    esp_mqtt_client_publish(s_client, topic, buf, 0, 1, 1);

    // Read-only mode has no controllable thermostat, so surface target + mode as
    // plain sensors too (so HA still sees the setpoint the active source is driving).
    if (s_readonly) {
        snprintf(buf, sizeof buf,
            "{\"name\":\"DragonBreath Target\",\"uniq_id\":\"%s_target\","
            "\"avty_t\":\"%s/availability\",\"stat_t\":\"%s/state\","
            "\"val_tpl\":\"{{value_json.target}}\",\"unit_of_meas\":\"\xC2\xB0""C\","
            "\"dev_cla\":\"temperature\",\"dev\":{\"ids\":[\"%s\"]}}",
            p, p, p, p);
        snprintf(topic, sizeof topic, "homeassistant/sensor/%s_target/config", p);
        esp_mqtt_client_publish(s_client, topic, buf, 0, 1, 1);

        snprintf(buf, sizeof buf,
            "{\"name\":\"DragonBreath Mode\",\"uniq_id\":\"%s_mode\","
            "\"avty_t\":\"%s/availability\",\"stat_t\":\"%s/state\","
            "\"val_tpl\":\"{{value_json.mode}}\",\"dev\":{\"ids\":[\"%s\"]}}",
            p, p, p, p);
        snprintf(topic, sizeof topic, "homeassistant/sensor/%s_mode/config", p);
        esp_mqtt_client_publish(s_client, topic, buf, 0, 1, 1);
    }
}

static void publish_state(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);

    char cb[16], pb[16];
    if (isfinite(snap.chamber_c)) snprintf(cb, sizeof cb, "%.1f", snap.chamber_c); else strcpy(cb, "null");
    if (isfinite(snap.ptc_c))     snprintf(pb, sizeof pb, "%.1f", snap.ptc_c);     else strcpy(pb, "null");
    const char *mode = (snap.mode == PB_MODE_OFF) ? "off" : "heat";

    char buf[160];
    snprintf(buf, sizeof buf,
        "{\"chamber\":%s,\"ptc\":%s,\"target\":%.0f,\"mode\":\"%s\",\"fan\":%u}",
        cb, pb, (double)snap.effective_target_c, mode, (unsigned)snap.effective_fan_percent);

    char topic[80];
    snprintf(topic, sizeof topic, "%s/state", prefix());
    esp_mqtt_client_publish(s_client, topic, buf, 0, 0, 1);   // qos0, retain
}

// ---------- mqtt events ----------

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = PB_HA_CONNECTED;
        s_status.connected = true;
        s_pub_pending = true;   // ask tick() to publish state promptly (no cross-task write)
        xSemaphoreGive(s_lock);
        // Availability online (retained) + discovery. In full-control mode also
        // subscribe the command topics; read-only mode never accepts commands.
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);
        publish_discovery();
        if (!s_readonly) {
            char sub[80];
            snprintf(sub, sizeof sub, "%s/mode/set", prefix());
            esp_mqtt_client_subscribe(s_client, sub, 1);
            snprintf(sub, sizeof sub, "%s/temp/set", prefix());
            esp_mqtt_client_subscribe(s_client, sub, 1);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = PB_HA_DISCONNECTED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len > 0 && e->data_len >= 0)
            handle_cmd(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default:
        break;
    }
}

// ---------- lifecycle ----------

static esp_err_t ha_start_common(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (nvs_load(&s_cfg) != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Home Assistant config saved; idle");
        s_status.state = PB_HA_DISABLED;
        return ESP_OK;
    }

    // Seed the desired target from the remembered manual target.
    pb_policy_params_t params;
    pb_policy_get_params(&params);
    if (params.manual_target_c > 0) s_desired_target = params.manual_target_c;

    snprintf(s_avail_topic, sizeof s_avail_topic, "%s/availability", prefix());

    char uri[96];
    snprintf(uri, sizeof uri, "mqtt://%s:%u", s_cfg.host, (unsigned)s_cfg.port);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .session.last_will = {
            .topic  = s_avail_topic,
            .msg    = "offline",
            .msg_len = 7,
            .qos    = 1,
            .retain = 1,
        },
    };
    if (s_cfg.user[0]) mc.credentials.username = s_cfg.user;
    if (s_cfg.pass[0]) mc.credentials.authentication.password = s_cfg.pass;

    s_client = esp_mqtt_client_init(&mc);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        s_status.state = PB_HA_DISCONNECTED;
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        s_status.state = PB_HA_DISCONNECTED;
        return err;
    }
    s_status.state = PB_HA_CONNECTING;
    ESP_LOGI(TAG, "connecting to broker %s (prefix '%s'%s)", uri, prefix(),
             s_readonly ? ", read-only" : "");
    return ESP_OK;
}

// Full-control: HA is the selected control source (subscribes commands, holds a lease).
esp_err_t pb_ha_start(void) { s_readonly = false; return ha_start_common(); }

// Read-only: HA runs ALONGSIDE another control source as a monitor — publishes
// sensors + state, never subscribes commands, never takes a pb_policy lease.
esp_err_t pb_ha_start_readonly(void) { s_readonly = true; return ha_start_common(); }

void pb_ha_tick(void)
{
    if (s_client == NULL) return;
    bool connected;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    connected = s_status.connected;
    bool pub_now = s_pub_pending;
    s_pub_pending = false;
    xSemaphoreGive(s_lock);
    if (!connected) return;

    int64_t now = esp_timer_get_time();
    if (pub_now || now - s_last_state_us >= STATE_PERIOD_US) {
        s_last_state_us = now;
        publish_state();
    }

    // Heartbeat the heat lease so the comms watchdog keeps heat alive. If the
    // heartbeat is rejected (lease expired/superseded), drop our copy.
    bool do_hb = false;
    pb_policy_lease_t lease = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_readonly && s_have_lease && now - s_last_hb_us >= HB_PERIOD_US) {
        lease = s_lease;
        s_last_hb_us = now;
        do_hb = true;
    }
    xSemaphoreGive(s_lock);
    if (do_hb && pb_policy_heartbeat(&lease) != PB_POLICY_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_have_lease = false;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t pb_ha_set_config(const pb_ha_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;   // takes effect on next boot (matches dc_moonraker semantics)
}

esp_err_t pb_ha_get_config(pb_ha_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pb_ha_get_status(pb_ha_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pb_ha_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_PORT);
    nvs_erase_key(h, KEY_USER);
    nvs_erase_key(h, KEY_PASS);
    nvs_erase_key(h, KEY_TOPIC);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
