#pragma once

#include "esp_err.h"

esp_err_t stockin_api_start(void);
esp_err_t stockin_submit_qr(const char *qr_value);
