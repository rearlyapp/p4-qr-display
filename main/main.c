#include <inttypes.h>

#include "barcode_scanner.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "stockin_api.h"
#include "stockin_ui.h"
#include "wireless.h"

static const char *TAG = "stockin_terminal";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();

    esp_chip_info_t chip;
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "SmartWorks packing-room activity terminal");
    ESP_LOGI(TAG, "ESP32-P4 rev=%u.%u flash=%" PRIu32 " MB PSRAM=%u MB",
             chip.revision / 100, chip.revision % 100,
             flash_size / (1024U * 1024U),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024U * 1024U)));
    ESP_LOGI(TAG, "Touch/input UI disabled; USB QR scanner is the only operational input");

    ESP_ERROR_CHECK(stockin_ui_start());
    ESP_ERROR_CHECK(wireless_start());
    ESP_ERROR_CHECK(stockin_api_start());
    ESP_ERROR_CHECK(barcode_scanner_start());
    stockin_ui_show_ready();

    ESP_LOGI(TAG, "PASS: display, scanner, Ethernet/Wi-Fi/BLE and carton transition worker launched");
}
