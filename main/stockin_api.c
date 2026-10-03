#include "stockin_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "secrets.h"
#include "stockin_ui.h"
#include "wireless.h"

static const char *TAG = "stockin_api";

#ifndef WAREHOUSE_LABELS_URL
#define WAREHOUSE_LABELS_URL "http://77.42.39.77:8000/wms/labels"
#endif
#ifndef WAREHOUSE_ANTE_STOCKIN_URL
#define WAREHOUSE_ANTE_STOCKIN_URL "http://77.42.39.77:8000/packing/ante-stock-in"
#endif
#ifndef PACKING_API_ORIGIN
#define PACKING_API_ORIGIN "https://65.108.56.155"
#endif
#ifndef PACKING_API_ROUTE_HOST
#define PACKING_API_ROUTE_HOST "marine.rearlytech.com"
#endif
#ifndef PACKING_STATION_ID
#define PACKING_STATION_ID "packing-station-01"
#endif

#define PACKING_API_ROOT "/api/v3/marine/packing"
#define SCAN_QUEUE_LENGTH 64
#define RECENT_SCAN_SLOTS 32
#define DUPLICATE_WINDOW_US (3000LL * 1000LL)
#define LABEL_CACHE_MAX 512
#define LABEL_CACHE_TTL_US (30000LL * 1000LL)
#define LABEL_RESPONSE_MAX (256U * 1024U)
#define JSON_RESPONSE_MAX (16U * 1024U)
#define SESSION_RESPONSE_MAX (64U * 1024U)
#define SESSION_REFRESH_US (30000LL * 1000LL)

typedef struct {
    uint32_t id;
    char qr[32];
    int64_t enqueued_at_us;
} scan_request_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t used;
    bool truncated;
    int status;
} http_result_t;

typedef struct {
    char qr[32];
    char item_code[192];
    char item_name[192];
    double quantity;
    bool available;
} label_entry_t;

typedef struct {
    char qr[32];
    char order_ref[128];
    char product_name[192];
    char grade[48];
    char trace_code[96];
    char declared_net_kg[32];
    char label_state[48];
    char warehouse[64];
    int actual_blocks;
    int blocks_per_carton;
} carton_details_t;

typedef struct {
    char qr[32];
    int64_t timestamp_us;
} recent_scan_t;

static QueueHandle_t s_scan_queue;
static unsigned s_packed_count;
static unsigned s_rejected_count;
static unsigned s_available_count;
static uint32_t s_next_scan_id = 1;
static recent_scan_t s_recent_scans[RECENT_SCAN_SLOTS];
static size_t s_recent_scan_next;
static label_entry_t *s_labels;
static size_t s_label_count;
static int64_t s_label_cache_time_us;
static char s_supervisor_token[192];
static stockin_packing_session_view_t s_packing_session;
static bool s_packing_started;
static int64_t s_session_refresh_time_us;

static bool persisted_packing_on(const char *sheet_ref)
{
    nvs_handle_t handle;
    if (nvs_open("packing_ui", NVS_READONLY, &handle) != ESP_OK) return false;
    char stored[sizeof(s_packing_session.sheet_ref)] = {0};
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_str(handle, "on_sheet", stored, &size);
    nvs_close(handle);
    return err == ESP_OK && strcmp(stored, sheet_ref) == 0;
}

static void persist_packing_on(const char *sheet_ref)
{
    nvs_handle_t handle;
    if (nvs_open("packing_ui", NVS_READWRITE, &handle) != ESP_OK) return;
    if (sheet_ref && sheet_ref[0]) nvs_set_str(handle, "on_sheet", sheet_ref);
    else nvs_erase_key(handle, "on_sheet");
    nvs_commit(handle);
    nvs_close(handle);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data ||
        event->data_len <= 0) return ESP_OK;

    http_result_t *result = (http_result_t *)event->user_data;
    size_t available = result->capacity - result->used - 1;
    size_t incoming = (size_t)event->data_len;
    size_t copy = incoming < available ? incoming : available;
    if (copy) {
        memcpy(result->data + result->used, event->data, copy);
        result->used += copy;
        result->data[result->used] = 0;
    }
    if (copy < incoming) result->truncated = true;
    return ESP_OK;
}

static void http_result_free(http_result_t *result)
{
    free(result->data);
    memset(result, 0, sizeof(*result));
}

static esp_err_t perform_request(const char *url, esp_http_client_method_t method,
                                 const char *route_host, const char *bearer,
                                 const char *payload, size_t response_capacity,
                                 http_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->capacity = response_capacity;
    result->data = calloc(1, response_capacity);
    if (!result->data) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .event_handler = http_event,
        .user_data = result,
        .disable_auto_redirect = false,
        .user_agent = "SmartWorks-P4-QR/2.0",
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        http_result_free(result);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    if (route_host && route_host[0]) {
        esp_http_client_set_header(client, "Host", route_host);
    }
    char authorization[240];
    if (bearer && bearer[0]) {
        snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (payload) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, strlen(payload));
    }

    esp_err_t err = esp_http_client_perform(client);
    result->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (result->truncated) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void json_copy(cJSON *object, const char *key, char *out, size_t out_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        strlcpy(out, value->valuestring, out_size);
    }
}

static void copy_error_message(const char *body, char *out, size_t out_size)
{
    out[0] = 0;
    cJSON *root = cJSON_Parse(body);
    if (!root) return;
    const char *keys[] = {"detail", "message", "error", "status"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsString(value) && value->valuestring) {
            strlcpy(out, value->valuestring, out_size);
            break;
        }
    }
    cJSON_Delete(root);
}

static void update_activity(void)
{
    unsigned queued = s_scan_queue ? uxQueueMessagesWaiting(s_scan_queue) : 0;
    stockin_ui_set_activity(s_packed_count, s_rejected_count,
                            queued, s_available_count);
}

static esp_err_t refresh_labels(void)
{
    http_result_t response;
    esp_err_t err = perform_request(WAREHOUSE_LABELS_URL, HTTP_METHOD_GET,
                                    NULL, NULL, NULL,
                                    LABEL_RESPONSE_MAX, &response);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Labels-V request failed: %s", esp_err_to_name(err));
        http_result_free(&response);
        return err;
    }
    if (response.status != 200) {
        ESP_LOGE(TAG, "Labels-V request returned HTTP %d", response.status);
        http_result_free(&response);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        http_result_free(&response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    label_entry_t *fresh = calloc(LABEL_CACHE_MAX, sizeof(*fresh));
    if (!fresh) {
        cJSON_Delete(root);
        http_result_free(&response);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (count == LABEL_CACHE_MAX) break;
        label_entry_t *entry = &fresh[count];
        json_copy(item, "batch_id", entry->qr, sizeof(entry->qr));
        json_copy(item, "item_code", entry->item_code, sizeof(entry->item_code));
        json_copy(item, "item_name", entry->item_name, sizeof(entry->item_name));
        cJSON *qty = cJSON_GetObjectItemCaseSensitive(item, "qty");
        if (cJSON_IsNumber(qty)) entry->quantity = qty->valuedouble;
        if (entry->qr[0]) {
            entry->available = true;
            ++count;
        }
    }

    free(s_labels);
    s_labels = fresh;
    s_label_count = count;
    s_available_count = (unsigned)count;
    s_label_cache_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "PASS: loaded %u available carton QR IDs from Labels-V",
             (unsigned)count);
    update_activity();
    cJSON_Delete(root);
    http_result_free(&response);
    return ESP_OK;
}

static label_entry_t *find_cached_label(const char *qr)
{
    for (size_t i = 0; i < s_label_count; ++i) {
        if (s_labels[i].available && strcmp(s_labels[i].qr, qr) == 0) {
            return &s_labels[i];
        }
    }
    return NULL;
}

static esp_err_t find_label(const char *qr, label_entry_t *result)
{
    int64_t age = esp_timer_get_time() - s_label_cache_time_us;
    if (!s_labels || age > LABEL_CACHE_TTL_US) {
        esp_err_t err = refresh_labels();
        if (err != ESP_OK && !s_labels) return err;
    }

    label_entry_t *entry = find_cached_label(qr);
    if (!entry) {
        esp_err_t err = refresh_labels();
        if (err != ESP_OK) return err;
        entry = find_cached_label(qr);
    }
    if (!entry) return ESP_ERR_NOT_FOUND;
    *result = *entry;
    return ESP_OK;
}

static void mark_label_used(const char *qr)
{
    label_entry_t *entry = find_cached_label(qr);
    if (!entry) return;
    entry->available = false;
    if (s_available_count) --s_available_count;
}

static esp_err_t request_supervisor_token(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s/demo/session",
             PACKING_API_ORIGIN, PACKING_API_ROOT);
    http_result_t response;
    esp_err_t err = perform_request(url, HTTP_METHOD_POST,
                                    PACKING_API_ROUTE_HOST, NULL,
                                    "{\"role\":\"supervisor\"}",
                                    JSON_RESPONSE_MAX, &response);
    if (err != ESP_OK) {
        http_result_free(&response);
        return err;
    }
    if (response.status != 200) {
        ESP_LOGE(TAG, "Packing demo session returned HTTP %d", response.status);
        http_result_free(&response);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (!root) {
        http_result_free(&response);
        return ESP_ERR_INVALID_RESPONSE;
    }
    json_copy(root, "token", s_supervisor_token, sizeof(s_supervisor_token));
    cJSON_Delete(root);
    http_result_free(&response);
    if (!s_supervisor_token[0]) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static esp_err_t parse_packing_session(const char *body,
                                       stockin_packing_session_view_t *parsed)
{
    memset(parsed, 0, sizeof(*parsed));
    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    if (cJSON_IsNull(session) || !session) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (!cJSON_IsObject(session)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    parsed->active = true;
    json_copy(session, "sheetRef", parsed->sheet_ref, sizeof(parsed->sheet_ref));
    json_copy(session, "state", parsed->state, sizeof(parsed->state));
    json_copy(session, "startedAt", parsed->started_at, sizeof(parsed->started_at));
    cJSON *elapsed = cJSON_GetObjectItemCaseSensitive(session, "elapsedSeconds");
    cJSON *labels = cJSON_GetObjectItemCaseSensitive(session, "labelsPrepared");
    if (cJSON_IsNumber(elapsed)) parsed->elapsed_seconds = (uint32_t)elapsed->valuedouble;
    if (cJSON_IsNumber(labels)) parsed->labels_prepared = labels->valueint;

    cJSON *products = cJSON_GetObjectItemCaseSensitive(session, "products");
    cJSON *product = NULL;
    cJSON_ArrayForEach(product, products) {
        char product_name[96] = {0};
        char trace[48] = {0};
        json_copy(product, "productName", product_name, sizeof(product_name));
        json_copy(product, "traceCode", trace, sizeof(trace));

        cJSON *grades = cJSON_GetObjectItemCaseSensitive(product, "grades");
        cJSON *grade = NULL;
        cJSON_ArrayForEach(grade, grades) {
            ++parsed->grade_total;
            if (parsed->grade_count >= STOCKIN_UI_MAX_GRADES) continue;
            stockin_grade_view_t *target = &parsed->grades[parsed->grade_count++];
            strlcpy(target->product, product_name, sizeof(target->product));
            strlcpy(target->trace, trace, sizeof(target->trace));
            json_copy(grade, "grade", target->grade, sizeof(target->grade));
            json_copy(grade, "state", target->state, sizeof(target->state));
            cJSON *blocks = cJSON_GetObjectItemCaseSensitive(grade, "expectedBlocks");
            cJSON *prepared = cJSON_GetObjectItemCaseSensitive(grade, "labelsPrepared");
            cJSON *expected = cJSON_GetObjectItemCaseSensitive(grade, "expectedLabels");
            if (cJSON_IsNumber(blocks)) target->expected_blocks = blocks->valueint;
            if (cJSON_IsNumber(prepared)) target->labels_prepared = prepared->valueint;
            if (cJSON_IsNumber(expected)) target->expected_labels = expected->valueint;
        }
    }

    cJSON_Delete(root);
    return parsed->sheet_ref[0] ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t refresh_packing_session(void)
{
    s_session_refresh_time_us = esp_timer_get_time();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!s_supervisor_token[0]) {
            esp_err_t err = request_supervisor_token();
            if (err != ESP_OK) return err;
        }

        char url[384];
        snprintf(url, sizeof(url), "%s%s/load-notes/current?stationId=%s",
                 PACKING_API_ORIGIN, PACKING_API_ROOT, PACKING_STATION_ID);
        http_result_t response;
        esp_err_t err = perform_request(url, HTTP_METHOD_GET,
                                        PACKING_API_ROUTE_HOST,
                                        s_supervisor_token, NULL,
                                        SESSION_RESPONSE_MAX, &response);
        if (err != ESP_OK) {
            http_result_free(&response);
            return err;
        }
        if (response.status == 401 && attempt == 0) {
            s_supervisor_token[0] = 0;
            http_result_free(&response);
            continue;
        }
        if (response.status != 200) {
            ESP_LOGE(TAG, "Load-note request returned HTTP %d", response.status);
            http_result_free(&response);
            return ESP_FAIL;
        }

        stockin_packing_session_view_t parsed;
        err = parse_packing_session(response.data, &parsed);
        http_result_free(&response);
        if (err != ESP_OK) return err;

        if (!parsed.active) {
            memset(&s_packing_session, 0, sizeof(s_packing_session));
            s_packing_started = false;
            persist_packing_on(NULL);
            stockin_ui_set_packing_session(STOCKIN_PACKING_NONE, NULL);
            ESP_LOGI(TAG, "Packing status: no packing planned");
            return ESP_OK;
        }

        if (!s_packing_session.active ||
            strcmp(s_packing_session.sheet_ref, parsed.sheet_ref) != 0) {
            s_packing_started = persisted_packing_on(parsed.sheet_ref);
        }
        s_packing_session = parsed;
        stockin_ui_set_packing_session(
            s_packing_started ? STOCKIN_PACKING_ON : STOCKIN_PACKING_PLANNED,
            &s_packing_session);
        ESP_LOGI(TAG, "Packing status: %s, load note=%s, varieties/grades=%u",
                 s_packing_started ? "on" : "planned",
                 s_packing_session.sheet_ref,
                 (unsigned)s_packing_session.grade_total);
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t parse_carton_details(const char *body, carton_details_t *details)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    json_copy(root, "cartonNumber", details->qr, sizeof(details->qr));
    json_copy(root, "orderRef", details->order_ref, sizeof(details->order_ref));
    json_copy(root, "productName", details->product_name, sizeof(details->product_name));
    json_copy(root, "grade", details->grade, sizeof(details->grade));
    json_copy(root, "traceCode", details->trace_code, sizeof(details->trace_code));
    json_copy(root, "declaredNetKg", details->declared_net_kg,
              sizeof(details->declared_net_kg));
    json_copy(root, "labelState", details->label_state, sizeof(details->label_state));
    json_copy(root, "warehouse", details->warehouse, sizeof(details->warehouse));
    cJSON *actual = cJSON_GetObjectItemCaseSensitive(root, "actualBlockCount");
    cJSON *planned = cJSON_GetObjectItemCaseSensitive(root, "blocksPerCarton");
    if (cJSON_IsNumber(actual)) details->actual_blocks = actual->valueint;
    if (cJSON_IsNumber(planned)) details->blocks_per_carton = planned->valueint;
    cJSON_Delete(root);
    return strlen(details->qr) == 24 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t get_carton_details(const char *qr, carton_details_t *details,
                                    int *http_status)
{
    memset(details, 0, sizeof(*details));
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!s_supervisor_token[0]) {
            esp_err_t err = request_supervisor_token();
            if (err != ESP_OK) return err;
        }

        char url[320];
        snprintf(url, sizeof(url), "%s%s/track-a/cartons/%s",
                 PACKING_API_ORIGIN, PACKING_API_ROOT, qr);
        http_result_t response;
        esp_err_t err = perform_request(url, HTTP_METHOD_GET,
                                        PACKING_API_ROUTE_HOST,
                                        s_supervisor_token, NULL,
                                        JSON_RESPONSE_MAX, &response);
        *http_status = response.status;
        if (err != ESP_OK) {
            http_result_free(&response);
            return err;
        }
        if (response.status == 401 && attempt == 0) {
            s_supervisor_token[0] = 0;
            http_result_free(&response);
            continue;
        }
        if (response.status == 404) {
            http_result_free(&response);
            return ESP_ERR_NOT_FOUND;
        }
        if (response.status != 200) {
            http_result_free(&response);
            return ESP_FAIL;
        }
        err = parse_carton_details(response.data, details);
        http_result_free(&response);
        return err;
    }
    return ESP_FAIL;
}

static esp_err_t post_ante_stockin(const char *qr, const char *item_code,
                                   char *message, size_t message_size,
                                   int *http_status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "rfid_tag", qr);
    cJSON_AddStringToObject(root, "item_code", item_code);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_ERR_NO_MEM;

    http_result_t response;
    esp_err_t err = perform_request(WAREHOUSE_ANTE_STOCKIN_URL,
                                    HTTP_METHOD_POST, NULL, NULL, payload,
                                    JSON_RESPONSE_MAX, &response);
    free(payload);
    *http_status = response.status;
    if (response.data) copy_error_message(response.data, message, message_size);
    if (err == ESP_OK && (response.status < 200 || response.status >= 300)) {
        err = ESP_FAIL;
    }
    http_result_free(&response);
    return err;
}

static stockin_carton_view_t carton_view(const carton_details_t *details)
{
    return (stockin_carton_view_t) {
        .qr = details->qr,
        .order_ref = details->order_ref,
        .product_name = details->product_name,
        .grade = details->grade,
        .trace_code = details->trace_code,
        .declared_net_kg = details->declared_net_kg,
        .label_state = details->label_state,
        .warehouse = details->warehouse,
        .actual_blocks = details->actual_blocks,
        .blocks_per_carton = details->blocks_per_carton,
    };
}

static void worker_task(void *arg)
{
    (void)arg;
    if (wireless_wait_connected(20000)) {
        if (refresh_labels() != ESP_OK) {
            ESP_LOGW(TAG, "Startup Labels-V preload failed; first scan will retry");
        }
        esp_err_t token_err = request_supervisor_token();
        if (token_err == ESP_OK) {
            ESP_LOGI(TAG, "PASS: packing-session metadata service ready");
            esp_err_t session_err = refresh_packing_session();
            if (session_err != ESP_OK) {
                ESP_LOGW(TAG, "Initial load-note read failed (%s); will retry",
                         esp_err_to_name(session_err));
            }
        } else {
            ESP_LOGW(TAG, "Packing-session metadata preload failed (%s); first scan will retry",
                     esp_err_to_name(token_err));
        }
    }

    scan_request_t scan;
    while (true) {
        if (xQueueReceive(s_scan_queue, &scan, pdMS_TO_TICKS(500)) != pdTRUE) {
            /* Give scanned cartons priority over periodic home-screen data. */
            int64_t session_age = esp_timer_get_time() - s_session_refresh_time_us;
            if (!uxQueueMessagesWaiting(s_scan_queue) && wireless_wait_connected(0) &&
                (!s_session_refresh_time_us || session_age >= SESSION_REFRESH_US)) {
                esp_err_t session_err = refresh_packing_session();
                if (session_err != ESP_OK) {
                    ESP_LOGW(TAG, "Load-note refresh failed (%s); keeping last status",
                             esp_err_to_name(session_err));
                }
            }
            continue;
        }
        ESP_LOGI(TAG, "QR id=%" PRIu32 " processing; queue wait=%" PRId64 " ms",
                 scan.id, (esp_timer_get_time() - scan.enqueued_at_us) / 1000);
        update_activity();
        stockin_ui_show_processing(scan.id, scan.qr);
        if (!wireless_wait_connected(5000)) {
            ++s_rejected_count;
            stockin_ui_show_error(scan.id, scan.qr, "NETWORK OFFLINE",
                "Carton not moved. Restore Wi-Fi or Ethernet, then scan this QR again.");
            update_activity();
            continue;
        }

        label_entry_t label = {0};
        esp_err_t err = find_label(scan.qr, &label);
        if (err == ESP_ERR_NOT_FOUND) {
            ++s_rejected_count;
            stockin_ui_show_not_in_labels(scan.id);
            update_activity();
            continue;
        }
        if (err != ESP_OK) {
            ++s_rejected_count;
            stockin_ui_show_error(scan.id, scan.qr, "CARTON DATA UNAVAILABLE",
                "Could not read the available carton list. Nothing was moved.");
            update_activity();
            continue;
        }

        carton_details_t details = {0};
        strlcpy(details.qr, scan.qr, sizeof(details.qr));
        strlcpy(details.product_name, label.item_name,
                sizeof(details.product_name));
        strlcpy(details.label_state, "available", sizeof(details.label_state));
        strlcpy(details.warehouse, "Labels - V", sizeof(details.warehouse));

        int detail_status = 0;
        carton_details_t rich_details;
        esp_err_t detail_err = get_carton_details(scan.qr, &rich_details,
                                                  &detail_status);
        if (detail_err == ESP_OK) {
            details = rich_details;
        } else {
            ESP_LOGW(TAG, "Packing metadata lookup unavailable (%s, HTTP %d); continuing with Labels-V data",
                     esp_err_to_name(detail_err), detail_status);
        }

        stockin_carton_view_t view = carton_view(&details);
        stockin_ui_show_carton(scan.id, &view, "STOCKING IN", 0xFBBF24);

        char message[320] = {0};
        int stockin_status = 0;
        err = post_ante_stockin(scan.qr, label.item_code, message,
                                sizeof(message), &stockin_status);
        if (err == ESP_OK) {
            ++s_packed_count;
            mark_label_used(scan.qr);
            strlcpy(details.warehouse, "Stores - V", sizeof(details.warehouse));
            view = carton_view(&details);
            if (s_packing_session.active) {
                s_packing_started = true;
                persist_packing_on(s_packing_session.sheet_ref);
                stockin_ui_set_packing_session(STOCKIN_PACKING_ON,
                                               &s_packing_session);
            }
            ESP_LOGI(TAG, "PASS: QR id=%" PRIu32 " carton moved Labels-V -> Stores-V, HTTP %d",
                     scan.id, stockin_status);
            stockin_ui_show_success(scan.id, &view,
                "Carton verified and moved to Ante-Room.");
        } else {
            ++s_rejected_count;
            ESP_LOGE(TAG, "FAIL: QR id=%" PRIu32 " Ante Room stock-in: %s, HTTP %d",
                     scan.id, esp_err_to_name(err), stockin_status);
            if (stockin_status == 0) {
                stockin_ui_show_error(scan.id, scan.qr, "CONNECTION TIMEOUT",
                    "Result unknown. Verify this carton before scanning it again.");
            } else if (stockin_status == 404 || stockin_status == 409) {
                stockin_ui_show_not_in_labels(scan.id);
            } else if (stockin_status == 400 || stockin_status == 422) {
                stockin_ui_show_error(scan.id, scan.qr, "QR NOT ACCEPTED",
                    message[0] ? message :
                    "The Ante Room service rejected this carton. Nothing was moved.");
            } else {
                stockin_ui_show_error(scan.id, scan.qr, "STOCK-IN FAILED",
                    message[0] ? message :
                    "The Ante Room service could not move this carton.");
            }
        }
        update_activity();
    }
}

esp_err_t stockin_api_start(void)
{
    s_scan_queue = xQueueCreate(SCAN_QUEUE_LENGTH, sizeof(scan_request_t));
    if (!s_scan_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(worker_task, "stockin_http", 16 * 1024,
                                NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool valid_qr(const char *value)
{
    if (!value || strlen(value) != 24) return false;
    for (size_t i = 0; i < 24; ++i) {
        if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

esp_err_t stockin_submit_qr(const char *qr_value)
{
    if (!s_scan_queue || !valid_qr(qr_value)) return ESP_ERR_INVALID_SIZE;

    scan_request_t scan = {.id = s_next_scan_id++};
    if (!s_next_scan_id) s_next_scan_id = 1; /* Zero is reserved for local input errors. */
    for (size_t i = 0; i < 24; ++i) {
        scan.qr[i] = (char)toupper((unsigned char)qr_value[i]);
    }
    scan.qr[24] = 0;

    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < RECENT_SCAN_SLOTS; ++i) {
        if (s_recent_scans[i].qr[0] &&
            strcmp(s_recent_scans[i].qr, scan.qr) == 0 &&
            now - s_recent_scans[i].timestamp_us < DUPLICATE_WINDOW_US) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    scan.enqueued_at_us = now;
    if (xQueueSend(s_scan_queue, &scan, 0) != pdTRUE) {
        ESP_LOGW(TAG, "QR id=%" PRIu32 " rejected: scan queue full", scan.id);
        update_activity();
        return ESP_ERR_TIMEOUT;
    }

    /* Announce only an accepted scan. The UI rejects this notification if the
     * worker already started/completed the same ID while USB was scheduled out.
     */
    unsigned pending = uxQueueMessagesWaiting(s_scan_queue);
    stockin_ui_show_queued(scan.id, scan.qr, pending);
    update_activity();
    ESP_LOGI(TAG, "QR id=%" PRIu32 " accepted; waiting=%u", scan.id, pending);

    recent_scan_t *recent = &s_recent_scans[s_recent_scan_next];
    strlcpy(recent->qr, scan.qr, sizeof(recent->qr));
    recent->timestamp_us = now;
    s_recent_scan_next = (s_recent_scan_next + 1) % RECENT_SCAN_SLOTS;
    return ESP_OK;
}
