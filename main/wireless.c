#include "wireless.h"
#include "network_setup.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_bt_host_stack.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "secrets.h"
#include "services/gap/ble_svc_gap.h"
#include "stockin_ui.h"

static const char *TAG = "wireless";
static uint8_t s_ble_addr_type;

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertisement data failed: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER,
                           &params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertising failed: %d", rc);
        return;
    }
    stockin_ui_set_bluetooth(true);
    ESP_LOGI(TAG, "PASS: BLE enabled and advertising as SmartWorks-Cartons");
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) ble_advertise();
    return 0;
}

static void ble_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    ble_advertise();
}

static void ble_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    stockin_ui_set_bluetooth(false);
}

static void ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t bluetooth_start(void)
{
    esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t err = esp_hosted_bt_host_stack_setup(&bt);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.reset_cb = ble_reset;
    ble_hs_cfg.sync_cb = ble_sync;
    ble_svc_gap_init();
    int rc = ble_svc_gap_device_name_set("SmartWorks-Cartons");
    if (rc != 0) return ESP_FAIL;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static void wireless_task(void *arg)
{
    (void)arg;
    stockin_ui_set_wifi(false, "RADIO STARTING");
    stockin_ui_set_bluetooth(false);

    esp_err_t err = esp_hosted_connect_to_slave();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 coprocessor link failed: %s", esp_err_to_name(err));
        stockin_ui_set_wifi(false, "C6 LINK ERROR");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "PASS: P4-to-C6 ESP-Hosted link ready");

    err = network_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        stockin_ui_set_wifi(false, "WI-FI ERROR");
    }

    err = bluetooth_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth start failed: %s", esp_err_to_name(err));
        stockin_ui_set_bluetooth(false);
    }
    vTaskDelete(NULL);
}

esp_err_t wireless_start(void)
{
    ESP_RETURN_ON_ERROR(network_start(), TAG, "Network startup failed");
    if (xTaskCreatePinnedToCore(wireless_task, "wireless", 8192, NULL,
                                6, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool wireless_wait_connected(uint32_t timeout_ms)
{
    return network_wait_connected(timeout_ms);
}
