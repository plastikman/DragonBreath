// SPDX-License-Identifier: MIT
#include "pb_peer.h"

#include "dc_peer.h"
#include "pb_policy.h"
#include "pb_ntc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "pb_peer";

#define PUBLISH_MS  2000   // heater heartbeat; the vent's fresh-window is many seconds
#define ANNOUNCE_MS 6000   // descriptor heartbeat (every 3rd status tick)

static uint8_t mode_code(const char *m)
{
    if (strcmp(m, "power_on") == 0) return 1;
    if (strcmp(m, "auto") == 0)     return 2;
    if (strcmp(m, "drying") == 0)   return 3;
    return 0;   // off / anything else
}

static void fill_ip(uint8_t ip[4])
{
    memset(ip, 0, 4);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        uint32_t a = info.ip.addr;
        ip[0] = a & 0xff; ip[1] = (a >> 8) & 0xff; ip[2] = (a >> 16) & 0xff; ip[3] = (a >> 24) & 0xff;
    }
}

// Broadcast the Breath's stable descriptor so a console labels it "DragonBreath"
// (kind/name/firmware) instead of inferring a nameless device from heater frames.
static void publish_announce(void)
{
    dc_peer_announce_t a = {0};
    a.kind = DC_PEER_KIND_BREATH;
    a.caps = DC_PEER_CAP_BIT(DC_PEER_CAP_ANNOUNCE) | DC_PEER_CAP_BIT(DC_PEER_CAP_HEATER);
    fill_ip(a.ip);
    strlcpy(a.name, "DragonBreath", sizeof(a.name));
    strlcpy(a.fw, esp_app_get_description()->version, sizeof(a.fw));
    dc_peer_publish(DC_PEER_CAP_ANNOUNCE, &a, sizeof(a));
}

static void peer_task(void *arg)
{
    (void)arg;
    int since_announce = ANNOUNCE_MS;   // announce on the first tick
    for (;;) {
        pb_policy_snapshot_t s;
        pb_policy_get_snapshot(&s);

        dc_peer_heater_t h = {0};
        h.mode  = mode_code(pb_policy_mode_str(s.mode));
        h.flags = (s.heater_demand ? DC_PEER_HEATER_DEMAND : 0)
                | (s.fault_latched ? DC_PEER_HEATER_FAULT : 0)
                | (s.inhibited     ? DC_PEER_HEATER_INHIBITED : 0);
        float tgt = s.effective_target_c > 0.0f ? s.effective_target_c : s.requested_target_c;
        h.target_dc  = tgt > 0.0f ? (int16_t)(tgt * 10.0f) : 0;
        h.chamber_dc = (s.chamber_status == PB_NTC_OK && isfinite(s.chamber_c))
                     ? (int16_t)(s.chamber_c * 10.0f) : DC_PEER_TEMP_UNKNOWN;
        h.state_revision = s.state_revision;

        dc_peer_publish(DC_PEER_CAP_HEATER, &h, sizeof(h));

        since_announce += PUBLISH_MS;
        if (since_announce >= ANNOUNCE_MS) { publish_announce(); since_announce = 0; }
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_MS));
    }
}

esp_err_t pb_peer_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char self_id[DC_PEER_ID_MAX];
    snprintf(self_id, sizeof(self_id), "dragonbreath-%02x%02x", mac[4], mac[5]);

    esp_err_t err = dc_peer_start(self_id);
    if (err != ESP_OK) return err;

    if (xTaskCreate(peer_task, "pb_peer", 3072, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "heater capability provider up as '%s'", self_id);
    return ESP_OK;
}
