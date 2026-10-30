#pragma once

typedef enum {
    PB_LED_AUTO = 0,
    PB_LED_ON,
    PB_LED_DRY,
    PB_LED_POWER,
    PB_LED_COUNT,
} pb_led_id_t;

typedef enum {
    PB_LED_OFF = 0,
    PB_LED_SOLID,
    PB_LED_BLINK,
    PB_LED_BLINK_SLOW,
    PB_LED_CODE,
    PB_LED_CODE_ONCE,
} pb_led_pattern_t;

void pb_leds_set(pb_led_id_t id, pb_led_pattern_t pattern);
void pb_leds_set_code_once(pb_led_id_t id, uint8_t pulses);
pb_led_pattern_t pb_leds_get(pb_led_id_t id);
