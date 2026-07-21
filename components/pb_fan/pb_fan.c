// SPDX-License-Identifier: MIT
// AC blower speed control via TRIAC phase-angle firing.
//
// *** TRIAC SAFETY — READ THIS ***
// The gate is fired with EXACTLY ONE short pulse per mains half-cycle, timed off
// the zero-cross ISR. It is NEVER driven with PWM / free-running switching. PWM
// on a TRIAC gate fires it at random points on the AC wave (high dV/dt, no clean
// commutation) and destroys it — it fails SHORTED, leaving the fan stuck at 100%.
// (This happened to another dev experimenting with an ESPHome PWM output.) Any
// change here must preserve the "one zero-cross-synced pulse per half-cycle" rule.
//
// Firing model + timing values mirror the field-proven implementation in Justin
// Hayes' klipper-esp32 panda_breath/fan.c (zero-cross ISR -> one-shot esp_timer
// -> ~150us gate pulse; delay = (1-duty)*half_cycle, min 500us; half-cycle period
// measured adaptively from the ZCD). Reimplemented here as a pure driver; the
// fan-follows-heater policy lives in pb_policy, not here.
#include "pb_fan.h"
#include "pb_board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pb_fan";

#define PB_GATE_PULSE_US       150     // long enough to latch, short enough not to cook the gate
#define PB_MIN_PHASE_DELAY_US  500     // never fire too close to the zero cross
#define PB_HALF_CYCLE_DEFAULT  8333    // 60 Hz until measured; updated adaptively

static volatile float    s_duty;                 // 0.0 .. 1.0
static volatile uint32_t s_half_cycle_us = PB_HALF_CYCLE_DEFAULT;
static volatile uint64_t s_last_zcd_us;
static volatile uint32_t s_zc_count;

static esp_timer_handle_t s_gate_fire_timer;
static esp_timer_handle_t s_gate_off_timer;

static void gate_off_cb(void *arg) { gpio_set_level(PB_GPIO_FAN_GATE, 0); }

static void gate_fire_cb(void *arg)
{
    gpio_set_level(PB_GPIO_FAN_GATE, 1);
    esp_timer_start_once(s_gate_off_timer, PB_GATE_PULSE_US);  // end the pulse
}

static void IRAM_ATTR zcd_isr(void *arg)
{
    uint64_t now = esp_timer_get_time();
    if (s_last_zcd_us > 0) {
        uint32_t period = (uint32_t)(now - s_last_zcd_us);
        if (period >= 4000 && period <= 12000) s_half_cycle_us = period;  // 50-120 Hz sanity
    }
    s_last_zcd_us = now;
    s_zc_count++;

    float duty = s_duty;
    if (duty <= 0.0f) return;                      // off: never fire the gate

    // Leading-edge phase angle: full duty fires right after the cross, low duty late.
    uint32_t delay_us = (uint32_t)((1.0f - duty) * (float)s_half_cycle_us);
    if (delay_us < PB_MIN_PHASE_DELAY_US) delay_us = PB_MIN_PHASE_DELAY_US;

    esp_timer_stop(s_gate_fire_timer);             // cancel any pending fire, reschedule
    esp_timer_start_once(s_gate_fire_timer, delay_us);
}

esp_err_t pb_fan_init(void)
{
    const gpio_config_t gate = {
        .pin_bit_mask = (1ULL << PB_GPIO_FAN_GATE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gate);
    gpio_set_level(PB_GPIO_FAN_GATE, 0);           // TRIAC idle off

    const esp_timer_create_args_t fire_args = { .callback = gate_fire_cb, .name = "fan_fire" };
    const esp_timer_create_args_t off_args  = { .callback = gate_off_cb,  .name = "fan_off"  };
    esp_err_t err = esp_timer_create(&fire_args, &s_gate_fire_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_create(&off_args, &s_gate_off_timer);
    if (err != ESP_OK) return err;

    const gpio_config_t zc = {
        .pin_bit_mask = (1ULL << PB_GPIO_ZERO_CROSS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,            // TLP785 output rises at each zero cross
    };
    gpio_config(&zc);
    esp_err_t iserr = gpio_install_isr_service(0);
    if (iserr != ESP_OK && iserr != ESP_ERR_INVALID_STATE) return iserr;
    err = gpio_isr_handler_add(PB_GPIO_ZERO_CROSS, zcd_isr, NULL);
    if (err != ESP_OK) return err;

    s_duty = 0.0f;
    ESP_LOGI(TAG, "init: gate GPIO%d, ZCD GPIO%d (phase-angle only; gate is NEVER PWM'd)",
             PB_GPIO_FAN_GATE, PB_GPIO_ZERO_CROSS);
    return ESP_OK;
}

void pb_fan_set_level(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_duty = (float)percent / 100.0f;
    if (percent == 0) gpio_set_level(PB_GPIO_FAN_GATE, 0);
}

uint8_t pb_fan_get_level(void) { return (uint8_t)(s_duty * 100.0f + 0.5f); }

void pb_fan_zc_diag(uint32_t *count_out, uint32_t *interval_us_out)
{
    if (count_out) *count_out = s_zc_count;
    if (interval_us_out) *interval_us_out = s_half_cycle_us;
}
