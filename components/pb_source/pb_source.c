#include "pb_source.h"
#include "nvs.h"

#define NVS_NS  "app_nvs"
#define KEY_SRC "ctl_src"

pb_ctl_source_t pb_source_get(void)
{
    uint8_t v = PB_SRC_KLIPPER;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_SRC, &v);   // leaves v at default if key absent
        nvs_close(h);
    }
    if (v > PB_SRC_HA) v = PB_SRC_KLIPPER;   // fail-safe to the shipped path
    return (pb_ctl_source_t)v;
}

esp_err_t pb_source_set(pb_ctl_source_t src)
{
    if (src > PB_SRC_HA) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_SRC, (uint8_t)src);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const char *pb_source_str(pb_ctl_source_t src)
{
    switch (src) {
    case PB_SRC_BAMBU: return "bambu";
    case PB_SRC_HA:    return "ha";
    default:           return "klipper";
    }
}
