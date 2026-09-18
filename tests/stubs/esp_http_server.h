#pragma once
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"

typedef struct { int unused; } httpd_req_t;
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t length);
