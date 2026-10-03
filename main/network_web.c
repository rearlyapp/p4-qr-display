#include "network_setup.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

extern const unsigned char setup_html_start[] asm("_binary_network_setup_html_start");
extern const unsigned char setup_html_end[] asm("_binary_network_setup_html_end");

static void headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; form-action 'self'");
}

static esp_err_t json_response(httpd_req_t *req, const char *status, const char *body)
{
    headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t page_get(httpd_req_t *req)
{
    headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)setup_html_start, setup_html_end - setup_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    network_status_t s;
    network_get_status(&s);
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(json, "wifi", s.wifi);
    cJSON_AddBoolToObject(json, "ethernet", s.ethernet);
    cJSON_AddBoolToObject(json, "hotspot", s.hotspot);
    cJSON_AddBoolToObject(json, "configuring", s.configuring);
    cJSON_AddBoolToObject(json, "radio_ready", s.radio_ready);
    cJSON_AddStringToObject(json, "wifi_ip", s.wifi_ip);
    cJSON_AddStringToObject(json, "ethernet_ip", s.ethernet_ip);
    cJSON_AddStringToObject(json, "hotspot_ip", s.hotspot_ip);
    cJSON_AddStringToObject(json, "ssid", s.ssid);
    cJSON_AddStringToObject(json, "hotspot_ssid", s.hotspot_ssid);
    cJSON_AddStringToObject(json, "message", s.message);
    cJSON *known = cJSON_AddArrayToObject(json, "known_ssids");
    if (known) for (unsigned i = 0; i < s.known_count; ++i)
        cJSON_AddItemToArray(known, cJSON_CreateString(s.known_ssids[i]));
    /* No password or setup code is returned over the status endpoint. */
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t err = json_response(req, "200 OK", body);
    free(body);
    return err;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        (strcasecmp(type, "application/json") && strcasecmp(type, "application/json; charset=utf-8")))
        return json_response(req, "415 Unsupported Media Type", "{\"message\":\"Use application/json.\"}");
    if (req->content_len <= 0 || req->content_len > 512)
        return json_response(req, "413 Content Too Large", "{\"message\":\"Request too large or empty.\"}");
    char body[513] = {0};
    size_t used = 0;
    while (used < req->content_len) {
        int got = httpd_req_recv(req, body + used, req->content_len - used);
        if (got <= 0) {
            memset(body, 0, sizeof(body));
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Incomplete request");
            return ESP_FAIL; /* close, do not leave unread bytes on the connection */
        }
        used += got;
    }
    /* Reject embedded NULs, including JSON escapes, instead of silently truncating credentials. */
    cJSON *json = (memchr(body, 0, used) || strstr(body, "\\u0000")) ? NULL :
                 cJSON_ParseWithLengthOpts(body, used + 1, NULL, true);
    memset(body, 0, sizeof(body));
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(json, "code");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(json) && cJSON_GetArraySize(json) == 3 && cJSON_IsString(ssid) &&
        cJSON_IsString(password) && cJSON_IsString(code))
        err = network_submit_wifi(ssid->valuestring, password->valuestring, code->valuestring);
    if (cJSON_IsString(password)) memset(password->valuestring, 0, strlen(password->valuestring));
    if (cJSON_IsString(code)) memset(code->valuestring, 0, strlen(code->valuestring));
    cJSON_Delete(json);
    switch (err) {
    case ESP_OK:
        return json_response(req, "202 Accepted", "{\"message\":\"Testing Wi-Fi. If disconnected, join the setup hotspot shown on the display and open http://192.168.4.1. Settings save only after DHCP succeeds.\"}");
    case ESP_ERR_INVALID_RESPONSE:
        return json_response(req, "403 Forbidden", "{\"message\":\"Enter the eight-digit setup code shown on the display.\"}");
    case ESP_ERR_TIMEOUT:
        httpd_resp_set_hdr(req, "Retry-After", "30");
        return json_response(req, "429 Too Many Requests", "{\"message\":\"Too many incorrect codes. Wait 30 seconds.\"}");
    case ESP_ERR_INVALID_STATE:
        return json_response(req, "409 Conflict", "{\"message\":\"Wi-Fi is starting or another change is still being tested.\"}");
    case ESP_ERR_NO_MEM:
        return json_response(req, "409 Conflict", "{\"message\":\"Five networks are already saved. Update an existing SSID.\"}");
    default:
        return json_response(req, "400 Bad Request", "{\"message\":\"SSID must be 1-32 bytes; password must be 8-63 printable ASCII characters (or empty for an open network).\"}");
    }
}

esp_err_t network_web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/network", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) { httpd_stop(server); return err; }
    }
    ESP_LOGI("network_web", "PASS: local network setup server ready on port 80");
    return ESP_OK;
}
