// SPDX-License-Identifier: MIT
// AC blower ON/OFF control, matching the stock firmware (see docs/FAN_DRIVE.md).
//
// *** TRIAC SAFETY ***
// Stock does NOT phase-angle or PWM the gate. GPIO3 is a HELD level: HIGH = fan
// on (the gate continuously drives the MOC3021 -> BT136 conducts every half-cycle
// = full speed), LOW = off. The level is switched at a zero crossing for clean AC
// switching. The gate is NEVER pulsed or PWM'd — free-running switching destroys
// the TRIAC (it fails shorted, leaving the fan stuck on). The stock blower has no
// variable speed and this mirrors that. Adding variable speed later would mean
// ONE zero-cross-synced pulse per half-cycle (phase control) — never PWM.
#include "pb_fan.h"
#include "pb_board.h"
#include "pb_fan_zc_filter.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <stdbool.h>

static const char *TAG = "pb_fan";

static volatile bool     s_want_on;
static volatile bool     s_applied;
static pb_fan_zc_filter_t s_zc;
#ifdef CONFIG_PB_DEVBOARD_SAFE
static uint64_t s_hil_now_us;
#endif

#ifndef CONFIG_PB_DEVBOARD_SAFE
static void IRAM_ATTR zcd_isr(void *arg)
{
    uint64_t now = esp_timer_get_time();
    if (!pb_fan_zc_filter_record(&s_zc, now)) return;

    bool on = s_want_on;                 // apply the commanded level at the zero cross
    if (on != s_applied) {
        gpio_set_level(PB_GPIO_FAN_GATE, on ? 1 : 0);
        s_applied = on;
    }
}
#endif

esp_err_t pb_fan_init(void)
{
#ifdef CONFIG_PB_DEVBOARD_SAFE
    s_want_on = false;
    s_applied = false;
    pb_fan_zc_filter_reset(&s_zc);
    s_hil_now_us = 0;
    ESP_LOGW(TAG, "safe dev-board backend: fan gate and ZCD GPIOs compiled out");
    return ESP_OK;
#else
    s_want_on = false;
    s_applied = false;
    pb_fan_zc_filter_reset(&s_zc);

    const gpio_config_t gate = {
        .pin_bit_mask = (1ULL << PB_GPIO_FAN_GATE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gate);
    gpio_set_level(PB_GPIO_FAN_GATE, 0);           // idle off

    const gpio_config_t zc = {
        .pin_bit_mask = (1ULL << PB_GPIO_ZERO_CROSS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,          // stock configures the ZCD input pulled-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&zc);
    esp_err_t iserr = gpio_install_isr_service(0);
    if (iserr != ESP_OK && iserr != ESP_ERR_INVALID_STATE) return iserr;
    esp_err_t err = gpio_isr_handler_add(PB_GPIO_ZERO_CROSS, zcd_isr, NULL);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "init: ON/OFF held-gate (stock model), gate GPIO%d, ZCD GPIO%d; never PWM'd",
             PB_GPIO_FAN_GATE, PB_GPIO_ZERO_CROSS);
    return ESP_OK;
#endif
}

void pb_fan_set_level(uint8_t percent)
{
    bool on = (percent > 0);
    s_want_on = on;
    if (!on) {                                     // turn OFF immediately (don't wait for a ZC)
#ifndef CONFIG_PB_DEVBOARD_SAFE
        gpio_set_level(PB_GPIO_FAN_GATE, 0);
#endif
        s_applied = false;
    }
    // ON is applied at the next zero cross by the ISR (clean AC switching).
}

uint8_t pb_fan_get_level(void) { return s_want_on ? 100 : 0; }

void pb_fan_zc_diag(uint32_t *count_out, uint32_t *interval_us_out,
                    uint32_t *rejected_count_out)
{
    if (count_out) *count_out = s_zc.accepted_count;
    if (interval_us_out) *interval_us_out = s_zc.accepted_interval_us;
    if (rejected_count_out) *rejected_count_out = s_zc.rejected_count;
}

#ifdef CONFIG_PB_DEVBOARD_SAFE
void pb_fan_hil_zero_cross(uint32_t count, uint32_t interval_us)
{
    if (count == 0) return;
    for (uint32_t i = 0; i < count; i++) {
        if (s_hil_now_us == 0)
            s_hil_now_us = 1;
        else
            s_hil_now_us += interval_us;
        if (pb_fan_zc_filter_record(&s_zc, s_hil_now_us))
            s_applied = s_want_on;
    }
}
#endif
