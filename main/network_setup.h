#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "network_credentials.h"

typedef struct {
    bool wifi, ethernet, hotspot, configuring, radio_ready;
    char wifi_ip[16], ethernet_ip[16], hotspot_ip[16];
    char ssid[33], hotspot_ssid[33], message[128];
    unsigned known_count;
    char known_ssids[NETWORK_MAX_PROFILES][33];
} network_status_t;

esp_err_t network_start(void);
esp_err_t network_wifi_start(void);
bool network_wait_connected(uint32_t timeout_ms);
void network_get_status(network_status_t *status);
/* ESP_ERR_INVALID_ARG: invalid credentials; INVALID_STATE: busy/not ready;
 * INVALID_RESPONSE: wrong setup code; TIMEOUT: code attempts rate limited. */
esp_err_t network_submit_wifi(const char *ssid, const char *password, const char *code);
esp_err_t network_web_start(void);
