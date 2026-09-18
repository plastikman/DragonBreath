#include "db_portal_assets.h"

esp_err_t db_portal_send_text(httpd_req_t *req, const char *content_type,
                               const char *cache_control,
                               const unsigned char *start,
                               const unsigned char *end)
{
    // ESP-IDF TEXT embedding places _end after the appended NUL. Only strip a
    // terminator that exists; empty ranges must not underflow.
    size_t length = (size_t)(end - start);
    if (length > 0 && start[length - 1] == '\0') --length;
    esp_err_t err = httpd_resp_set_type(req, content_type);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    if (err != ESP_OK) return err;
    return httpd_resp_send(req, (const char *)start, length);
}
