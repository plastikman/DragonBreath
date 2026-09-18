#pragma once

#include "esp_http_server.h"

#define DB_PORTAL_HTML_CACHE "no-store"
// URLs are stable across OTA: bound freshness rather than promising immutable
// content for a year. Reload after 60 s to guarantee the new firmware's JS.
#define DB_PORTAL_JS_CACHE "public, max-age=60, must-revalidate"

esp_err_t db_portal_send_text(httpd_req_t *req, const char *content_type,
                               const char *cache_control,
                               const unsigned char *start,
                               const unsigned char *end);
