// SPDX-License-Identifier: MIT
// TRIAC phase-angle fan control. Skeleton implements the full timing structure;
// the phase math + clamps are marked for bench tuning.
#include "pb_fan.h"
#include "pb_board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pb_fan";

// Mains half-cycle: 50 Hz -> 10000 us, 60 Hz -> 8333 us. TODO: detect from the
// zero-cross interval instead of assuming. Default 50 Hz.
#define PB_HALF_CYCLE_US    10000

// Don't fire within these guard bands of the zero cross / cycle end.
#define PB_PHASE_MIN_US     300     // near full power
#define PB_PHASE_MAX_US     9200    // near off (but level 0 stops pulses entirely)
#define PB_GATE_PULSE_US    50      // opto-triac trigger width

static volatile uint8_t s_level;          // 0..100
static esp_timer_handle_t s_fire_timer;   // one-shot: fires the gate after the delay
static esp_timer_handle_t s_pulse_timer;  // one-shot: ends the gate pulse

static void IRAM_ATTR gate_off_cb(void *arg) { gpio_set_level(PB_GPIO_FAN_GATE, 0); }

static void IRAM_ATTR gate_on_cb(void *arg)
{
    gpio_set_level(PB_GPIO_FAN_GATE, 1);
    esp_timer_start_once(s_pulse_timer, PB_GATE_PULSE_US);   // schedule pulse end
}

// Map level (1..99) to a firing delay after the zero cross. Leading-edge:
// higher level -> shorter delay -> more of the half-cycle conducts.
// TODO: linear in phase angle is a first approximation; blower power vs phase is
//       non-linear, and the EC fan has its own response. Tune on the bench.
static inline uint32_t level_to_delay_us(uint8_t level)
{
    uint32_t span = PB_PHASE_MAX_US - PB_PHASE_MIN_US;
    return PB_PHASE_MAX_US - (span * level) / 100u;
}

static void IRAM_ATTR zero_cross_isr(void *arg)
{
    uint8_t level = s_level;
    if (level == 0) return;                       // off: never fire
    if (level >= 100) { gate_on_cb(NULL); return; } // full: fire immediately
    esp_timer_start_once(s_fire_timer, level_to_delay_us(level));
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
    gpio_set_level(PB_GPIO_FAN_GATE, 0);          // TRIAC idle off

    const esp_timer_create_args_t fire_args = {
        .callback = gate_on_cb, .dispatch_method = ESP_TIMER_ISR, .name = "fan_fire" };
    const esp_timer_create_args_t pulse_args = {
        .callback = gate_off_cb, .dispatch_method = ESP_TIMER_ISR, .name = "fan_pulse" };
    esp_err_t err = esp_timer_create(&fire_args, &s_fire_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_create(&pulse_args, &s_pulse_timer);
    if (err != ESP_OK) return err;

    const gpio_config_t zc = {
        .pin_bit_mask = (1ULL << PB_GPIO_ZERO_CROSS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,           // TODO: confirm edge polarity of the TLP785 ZCD
    };
    gpio_config(&zc);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PB_GPIO_ZERO_CROSS, zero_cross_isr, NULL);

    s_level = 0;
    ESP_LOGI(TAG, "init: gate GPIO%d, ZCD GPIO%d (bench-tune phase timing before use)",
             PB_GPIO_FAN_GATE, PB_GPIO_ZERO_CROSS);
    return ESP_OK;
}

void pb_fan_set_level(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_level = percent;
    if (percent == 0) gpio_set_level(PB_GPIO_FAN_GATE, 0);
}

uint8_t pb_fan_get_level(void) { return s_level; }
