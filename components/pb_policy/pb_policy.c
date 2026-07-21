// SPDX-License-Identifier: MIT
#include "pb_policy.h"
#include "pb_heater.h"
#include "pb_fan.h"

#include "esp_log.h"

static const char *TAG = "pb_policy";

static uint8_t s_requested_fan;

esp_err_t pb_policy_init(void)
{
    s_requested_fan = 0;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

void pb_policy_apply(const pb_policy_input_t *in)
{
    if (!in) return;
    if (in->link_alive) pb_heater_notify_link_alive();
    pb_heater_set_target_c(in->chamber_target_c);
    s_requested_fan = in->fan_percent;
}

void pb_policy_tick(void)
{
    pb_heater_tick();

    // Simple autonomous rule: ensure some airflow whenever the heater is driving,
    // otherwise honor the requested fan level. TODO: refine (filter mode, spool-down).
    if (pb_heater_is_on() && s_requested_fan < 30) {
        pb_fan_set_level(30);
    } else {
        pb_fan_set_level(s_requested_fan);
    }
}
