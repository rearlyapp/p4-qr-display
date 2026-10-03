#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t wireless_start(void);
bool wireless_wait_connected(uint32_t timeout_ms);
