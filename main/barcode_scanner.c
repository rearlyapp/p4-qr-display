#include "barcode_scanner.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "stockin_api.h"
#include "stockin_ui.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

static const char *TAG = "barcode_usb";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_event_t;

static QueueHandle_t s_hid_events;
static char s_qr[128];
static size_t s_qr_len;
static uint8_t s_prev_keys[HID_KEYBOARD_KEY_MAX];

static const uint8_t keycode_ascii[57][2] = {
    {0,0},{0,0},{0,0},{0,0},
    {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},
    {'g','G'},{'h','H'},{'i','I'},{'j','J'},{'k','K'},{'l','L'},
    {'m','M'},{'n','N'},{'o','O'},{'p','P'},{'q','Q'},{'r','R'},
    {'s','S'},{'t','T'},{'u','U'},{'v','V'},{'w','W'},{'x','X'},
    {'y','Y'},{'z','Z'},
    {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},
    {'7','&'},{'8','*'},{'9','('},{'0',')'},
    {'\r','\r'},{0,0},{'\b','\b'},{'\t','\t'},{' ',' '},
    {'-','_'},{'=','+'},{'[','{'},{']','}'},{'\\','|'},{'\\','|'},
    {';',':'},{'\'','"'},{'`','~'},{',','<'},{'.','>'},{'/','?'}
};

static bool key_in_report(const uint8_t *keys, uint8_t key)
{
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; ++i) {
        if (keys[i] == key) return true;
    }
    return false;
}

static char key_to_char(uint8_t modifier, uint8_t key)
{
    bool shift = (modifier & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)) != 0;
    if (key >= HID_KEY_A && key <= HID_KEY_SLASH) {
        return (char)keycode_ascii[key][shift ? 1 : 0];
    }
    if (key >= HID_KEY_KEYPAD_1 && key <= HID_KEY_KEYPAD_9) {
        return (char)('1' + key - HID_KEY_KEYPAD_1);
    }
    if (key == HID_KEY_KEYPAD_0) return '0';
    if (key == HID_KEY_KEYPAD_DELETE) return '.';
    return 0;
}

static void accept_key(uint8_t modifier, uint8_t key)
{
    if (key == HID_KEY_ENTER || key == HID_KEY_KEYPAD_ENTER) {
        if (s_qr_len > 0) {
            s_qr[s_qr_len] = 0;
            ESP_LOGI(TAG, "QR scan complete (%u characters)", (unsigned)s_qr_len);
            esp_err_t err = stockin_submit_qr(s_qr);
            if (err == ESP_ERR_INVALID_STATE) {
                stockin_ui_show_duplicate(s_qr);
            } else if (err == ESP_ERR_INVALID_SIZE) {
                stockin_ui_show_error(0, s_qr, "INVALID QR",
                    "Expected a 24-character carton EPC value. Scan the printed carton QR.");
            } else if (err != ESP_OK) {
                stockin_ui_show_error(0, s_qr, "SCAN QUEUE FULL",
                    "Pause scanning until queued cartons have been processed, then scan this QR again.");
            }
            s_qr_len = 0;
            s_qr[0] = 0;
        }
        return;
    }

    char ch = key_to_char(modifier, key);
    if (ch == '\b') {
        if (s_qr_len) s_qr[--s_qr_len] = 0;
    } else if (ch >= 0x20 && ch <= 0x7e) {
        if (s_qr_len < sizeof(s_qr) - 1) {
            s_qr[s_qr_len++] = ch;
            s_qr[s_qr_len] = 0;
        } else {
            ESP_LOGW(TAG, "QR value too long; input cleared");
            s_qr_len = 0;
            stockin_ui_show_error(0, NULL, "QR TOO LONG",
                "QR value exceeded 127 characters. Scanner input was cleared.");
        }
    }
}

static void handle_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t)) return;
    const hid_keyboard_input_report_boot_t *report =
        (const hid_keyboard_input_report_boot_t *)data;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; ++i) {
        uint8_t key = report->key[i];
        if (key > HID_KEY_ERROR_UNDEFINED && !key_in_report(s_prev_keys, key)) {
            accept_key(report->modifier.val, key);
        }
    }
    memcpy(s_prev_keys, report->key, sizeof(s_prev_keys));
}

static void interface_callback(hid_host_device_handle_t handle,
                               hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[64];
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) == ESP_OK) {
            handle_keyboard_report(data, length);
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Scanner disconnected");
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        s_qr_len = 0;
        stockin_ui_set_usb(false, "SCANNER OFFLINE");
        hid_host_device_close(handle);
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "Scanner USB transfer error");
        stockin_ui_set_usb(false, "SCANNER ERROR");
    }
}

static void driver_callback(hid_host_device_handle_t handle,
                            hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    hid_event_t item = {.handle = handle, .event = event};
    xQueueSend(s_hid_events, &item, 0);
}

static void usb_library_task(void *arg)
{
    TaskHandle_t starter = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
        xTaskNotifyGive(starter);
        vTaskDelete(NULL);
    }
    xTaskNotifyGive(starter);
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void scanner_task(void *arg)
{
    (void)arg;
    hid_event_t evt;
    while (xQueueReceive(s_hid_events, &evt, portMAX_DELAY)) {
        if (evt.event != HID_HOST_DRIVER_EVENT_CONNECTED) continue;

        hid_host_dev_params_t params;
        if (hid_host_device_get_params(evt.handle, &params) != ESP_OK) continue;
        ESP_LOGI(TAG, "HID connected: subclass=%d protocol=%d",
                 params.sub_class, params.proto);

        if (params.proto != HID_PROTOCOL_KEYBOARD) {
            ESP_LOGW(TAG, "Ignoring non-keyboard HID interface");
            continue;
        }

        const hid_host_device_config_t config = {
            .callback = interface_callback,
            .callback_arg = NULL,
        };
        esp_err_t err = hid_host_device_open(evt.handle, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot open scanner: %s", esp_err_to_name(err));
            continue;
        }
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            hid_class_request_set_protocol(evt.handle, HID_REPORT_PROTOCOL_BOOT);
            hid_class_request_set_idle(evt.handle, 0, 0);
        }
        err = hid_host_device_start(evt.handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start scanner: %s", esp_err_to_name(err));
            hid_host_device_close(evt.handle);
            continue;
        }
        stockin_ui_set_usb(true, "SCANNER READY");
        ESP_LOGI(TAG, "PASS: USB HID QR scanner ready");
    }
    vTaskDelete(NULL);
}

esp_err_t barcode_scanner_start(void)
{
    s_hid_events = xQueueCreate(8, sizeof(hid_event_t));
    if (!s_hid_events) return ESP_ERR_NO_MEM;

    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(usb_library_task, "usb_events", 4096,
                                current, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));

    const hid_host_driver_config_t hid = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&hid), TAG, "HID host install failed");
    if (xTaskCreatePinnedToCore(scanner_task, "barcode_hid", 4096,
                                NULL, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    stockin_ui_set_usb(false, "WAITING SCANNER");
    ESP_LOGI(TAG, "Waiting for USB HID QR scanner");
    return ESP_OK;
}
