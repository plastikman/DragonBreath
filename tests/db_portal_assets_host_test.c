#include "db_portal_assets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *sent_type, *sent_cache, *sent_body;
static ssize_t sent_length;
static esp_err_t header_result;

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req;
    sent_type = type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value)
{
    (void)req;
    assert(strcmp(field, "Cache-Control") == 0);
    sent_cache = value;
    return header_result;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t length)
{
    (void)req;
    sent_body = buf;
    sent_length = length;
    return ESP_OK;
}

int main(void)
{
    httpd_req_t req = {0};
    const unsigned char text[] = "hello";
    const unsigned char empty[] = "";
    const unsigned char raw[] = {'j', 's'};
    assert(db_portal_send_text(&req, "text/html", DB_PORTAL_HTML_CACHE,
                               text, text + sizeof(text)) == ESP_OK);
    assert(sent_length == 5 && memcmp(sent_body, "hello", 5) == 0);
    assert(strcmp(sent_type, "text/html") == 0);
    assert(strcmp(sent_cache, "no-store") == 0);
    assert(db_portal_send_text(&req, "text/javascript", DB_PORTAL_JS_CACHE,
                               raw, raw + sizeof(raw)) == ESP_OK);
    assert(sent_length == 2 && memcmp(sent_body, "js", 2) == 0);
    assert(strcmp(sent_type, "text/javascript") == 0);
    assert(strcmp(sent_cache, "public, max-age=60, must-revalidate") == 0);
    assert(db_portal_send_text(&req, "text/html", DB_PORTAL_HTML_CACHE,
                               empty, empty + sizeof(empty)) == ESP_OK);
    assert(sent_length == 0);
    assert(db_portal_send_text(&req, "text/html", DB_PORTAL_HTML_CACHE,
                               empty, empty) == ESP_OK);
    assert(sent_length == 0);
    header_result = ESP_FAIL;
    sent_length = -1;
    assert(db_portal_send_text(&req, "text/html", DB_PORTAL_HTML_CACHE,
                               text, text + sizeof(text)) == ESP_FAIL);
    assert(sent_length == -1);
    puts("embedded TEXT length and cache policy: PASS");
    return 0;
}
