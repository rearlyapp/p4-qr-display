#include "network_setup.h"
#include "network_credentials.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "secrets.h"
#include "stockin_ui.h"

#define WIFI_READY BIT0
#define ETH_READY BIT1
#define UPLINK_READY (WIFI_READY | ETH_READY)
#define FALLBACK_MS 20000
#define CONNECT_TIMEOUT_MS 45000
#define AP_GRACE_MS 120000

typedef network_wifi_credentials_t credentials_t;
static const char *TAG = "network";
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static QueueHandle_t s_requests;
static network_status_t s_status;
static credentials_t s_saved;
static network_profiles_t s_profiles;
static char s_code[9];
static int64_t s_code_retry_at;
static unsigned s_bad_codes;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

void network_get_status(network_status_t *status)
{
    lock();
    *status = s_status;
    unlock();
}

static void message(const char *text)
{
    lock();
    strlcpy(s_status.message, text, sizeof(s_status.message));
    unlock();
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    lock();
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        ip_event_got_ip_t *got = data;
        bool eth = id == IP_EVENT_ETH_GOT_IP;
        char *ip = eth ? s_status.ethernet_ip : s_status.wifi_ip;
        snprintf(ip, 16, IPSTR, IP2STR(&got->ip_info.ip));
        if (eth) s_status.ethernet = true;
        else s_status.wifi = true;
        xEventGroupSetBits(s_events, eth ? ETH_READY : WIFI_READY);
        ESP_LOGI(TAG, "PASS: %s DHCP IP=%s", eth ? "Ethernet" : "Wi-Fi", ip);
    } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        s_status.wifi = false;
        s_status.wifi_ip[0] = 0;
        xEventGroupClearBits(s_events, WIFI_READY);
    } else if ((base == ETH_EVENT && (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP)) ||
               (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP)) {
        s_status.ethernet = false;
        s_status.ethernet_ip[0] = 0;
        xEventGroupClearBits(s_events, ETH_READY);
        ESP_LOGI(TAG, "Ethernet offline; Wi-Fi remains available");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "PASS: Ethernet link up, requesting DHCP");
    }
    unlock();
}

static esp_err_t ethernet_start(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = 31;
    emac.smi_gpio.mdio_num = 52;
    /* Waveshare P4 RMII defaults: clock input 50; TX49/34/35, RX28/29/30. */
    emac.dma_burst_len = ETH_DMA_BURST_LEN_4;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    if (!mac) return ESP_ERR_NO_MEM;
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = 51;
    /* IP101 supports the standard IEEE 802.3 registers used by this driver. */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    if (!phy) { mac->del(mac); return ESP_ERR_NO_MEM; }
    esp_eth_handle_t handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &handle);
    if (err != ESP_OK) { phy->del(phy); mac->del(mac); return err; }
    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.route_prio = 150;  /* Wi-Fi STA is 100; automatic route failover. */
    esp_netif_config_t net_cfg = { .base = &inherent, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    esp_netif_t *netif = esp_netif_new(&net_cfg);
    if (!netif) { esp_eth_driver_uninstall(handle); phy->del(phy); mac->del(mac); return ESP_ERR_NO_MEM; }
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(handle);
    if (!glue) err = ESP_ERR_NO_MEM;
    else err = esp_netif_attach(netif, glue);
    if (err == ESP_OK) err = esp_eth_start(handle);
    if (err != ESP_OK) {
        if (glue) esp_eth_del_netif_glue(glue);
        esp_netif_destroy(netif);
        esp_eth_driver_uninstall(handle);
        phy->del(phy);
        mac->del(mac);
    } else ESP_LOGI(TAG, "PASS: Ethernet started (DHCP, preferred over Wi-Fi)");
    return err;
}

static void publish_profiles(void)
{
    lock();
    s_status.known_count = s_profiles.count;
    memset(s_status.known_ssids, 0, sizeof(s_status.known_ssids));
    for (unsigned i = 0; i < s_profiles.count; ++i)
        strlcpy(s_status.known_ssids[i], s_profiles.entries[i].ssid, sizeof(s_status.known_ssids[i]));
    unlock();
}

static esp_err_t save_credentials(const credentials_t *credentials)
{
    network_profiles_t updated = s_profiles;
    if (!network_profile_put(&updated, credentials, true)) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("net_setup", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "wifi_list_v2", &updated, sizeof(updated));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_profiles = updated; /* keep a working RAM profile even if NVS fails */
    memset(&updated, 0, sizeof(updated));
    publish_profiles();
    return err;
}

static esp_err_t load_settings(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("net_setup", NVS_READWRITE, &nvs), TAG, "open settings");
    size_t size = sizeof(s_profiles);
    bool changed = nvs_get_blob(nvs, "wifi_list_v2", &s_profiles, &size) != ESP_OK ||
                   size != sizeof(s_profiles) || !network_profiles_valid(&s_profiles);
    if (changed) {
        memset(&s_profiles, 0, sizeof(s_profiles));
        credentials_t legacy = {0};
        size = sizeof(legacy);
        if (nvs_get_blob(nvs, "wifi_v1", &legacy, &size) == ESP_OK && size == sizeof(legacy))
            network_profile_put(&s_profiles, &legacy, false);
        memset(&legacy, 0, sizeof(legacy));
    }
    const credentials_t defaults[] = {
        {.ssid = STOCKIN_WIFI_SSID, .password = STOCKIN_WIFI_PASSWORD},
#if defined(STOCKIN_EXTRA_WIFI_SSID) && defined(STOCKIN_EXTRA_WIFI_PASSWORD)
        {.ssid = STOCKIN_EXTRA_WIFI_SSID, .password = STOCKIN_EXTRA_WIFI_PASSWORD},
#endif
    };
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        if (network_credentials_valid(defaults[i].ssid, defaults[i].password) &&
            network_profile_find(&s_profiles, defaults[i].ssid) < 0) {
            if (network_profile_put(&s_profiles, &defaults[i], false)) changed = true;
            else ESP_LOGW(TAG, "Known Wi-Fi list full; could not seed additional profile");
        }
    }
    esp_err_t err = ESP_OK;
    if (changed && s_profiles.count) {
        err = nvs_set_blob(nvs, "wifi_list_v2", &s_profiles, sizeof(s_profiles));
        if (err == ESP_OK) err = nvs_commit(nvs);
        if (err != ESP_OK) { nvs_close(nvs); return err; }
    }
    s_saved = s_profiles.entries[0];
    publish_profiles();
    ESP_LOGI(TAG, "PASS: loaded %u known Wi-Fi profiles", s_profiles.count);
    size = sizeof(s_code);
    err = nvs_get_str(nvs, "setup_code", s_code, &size);
    if (err != ESP_OK || strlen(s_code) != 8 || strspn(s_code, "0123456789") != 8) {
        snprintf(s_code, sizeof(s_code), "%08lu", (unsigned long)(esp_random() % 100000000));
        err = nvs_set_str(nvs, "setup_code", s_code);
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t set_station(const credentials_t *credentials)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(cfg.sta.password, credentials->password, strlen(credentials->password));
    cfg.sta.threshold.authmode = credentials->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        lock();
        strlcpy(s_status.ssid, credentials->ssid, sizeof(s_status.ssid));
        unlock();
    }
    return err;
}

static esp_err_t configure_hotspot(void)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.ap.ssid, s_status.hotspot_ssid, strlen(s_status.hotspot_ssid));
    cfg.ap.ssid_len = strlen(s_status.hotspot_ssid);
    memcpy(cfg.ap.password, s_code, 8);
    cfg.ap.channel = 1;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 3;
    return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

esp_err_t network_wifi_start(void)
{
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_ERR_NO_MEM, TAG, "STA netif");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap(), ESP_ERR_NO_MEM, TAG, "AP netif");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi RAM config");
    /* Configure the protected AP before the radio starts. Enabling the AP later
     * must never briefly broadcast a factory-default/open network. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "AP config mode");
    ESP_RETURN_ON_ERROR(configure_hotspot(), TAG, "protected AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(set_station(&s_saved), TAG, "station config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    lock();
    s_status.radio_ready = true;
    unlock();
    ESP_LOGI(TAG, "Wi-Fi station ready; passwords are never logged");
    return ESP_OK;
}

static esp_err_t hotspot_start(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "hotspot mode");
    lock();
    s_status.hotspot = true;
    strlcpy(s_status.hotspot_ip, "192.168.4.1", sizeof(s_status.hotspot_ip));
    unlock();
    ESP_LOGI(TAG, "PASS: setup hotspot started, open http://192.168.4.1 (key on display)");
    return ESP_OK;
}

esp_err_t network_submit_wifi(const char *ssid, const char *password, const char *code)
{
    if (!code || strlen(code) != 8) return ESP_ERR_INVALID_RESPONSE;
    lock();
    if (now_ms() < s_code_retry_at) { unlock(); return ESP_ERR_TIMEOUT; }
    unsigned diff = 0;
    for (unsigned i = 0; i < 8; ++i) diff |= (unsigned char)code[i] ^ (unsigned char)s_code[i];
    if (diff) {
        if (++s_bad_codes >= 5) { s_code_retry_at = now_ms() + 30000; s_bad_codes = 0; }
        unlock();
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_bad_codes = 0;
    if (!network_credentials_valid(ssid, password)) { unlock(); return ESP_ERR_INVALID_ARG; }
    if (s_status.configuring || !s_status.radio_ready) { unlock(); return ESP_ERR_INVALID_STATE; }
    bool existing = false;
    for (unsigned i = 0; i < s_status.known_count; ++i)
        if (!strcmp(s_status.known_ssids[i], ssid)) existing = true;
    if (!existing && s_status.known_count == NETWORK_MAX_PROFILES) { unlock(); return ESP_ERR_NO_MEM; }
    credentials_t request = {0};
    strlcpy(request.ssid, ssid, sizeof(request.ssid));
    strlcpy(request.password, password, sizeof(request.password));
    s_status.configuring = true;
    strlcpy(s_status.message, "Testing new Wi-Fi; previous settings retained until connected.", sizeof(s_status.message));
    BaseType_t sent = xQueueSend(s_requests, &request, 0);
    memset(&request, 0, sizeof(request));
    if (sent != pdTRUE) s_status.configuring = false;
    unlock();
    return sent == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void update_display(const network_status_t *s)
{
    char lines[2][128] = {0};
    if (s->hotspot && !s->wifi && !s->ethernet) {
        snprintf(lines[0], sizeof(lines[0]), "SETUP: %s   KEY: %s", s->hotspot_ssid, s_code);
        snprintf(lines[1], sizeof(lines[1]), "http://192.168.4.1%s%s", s->ethernet ? "   ETH: " : "",
                 s->ethernet ? s->ethernet_ip : "");
    } else {
        const char *ip = s->ethernet ? s->ethernet_ip : s->wifi ? s->wifi_ip : NULL;
        if (ip) snprintf(lines[0], sizeof(lines[0]), "%s  http://%s   SETUP CODE: %s",
                         s->ethernet ? "ETH" : "WI-FI", ip, s_code);
        else strlcpy(lines[0], "NETWORK CONNECTING - SETUP HOTSPOT STARTS AUTOMATICALLY", sizeof(lines[0]));
        if (s->ethernet && s->wifi) snprintf(lines[1], sizeof(lines[1]), "WI-FI  http://%s", s->wifi_ip);
        else if (!ip && !s->radio_ready) strlcpy(lines[1], "WI-FI RADIO STARTING", sizeof(lines[1]));
    }
    static char previous[2][128];
    static bool old_wifi, old_eth;
    if (memcmp(lines, previous, sizeof(lines)) || old_wifi != s->wifi || old_eth != s->ethernet) {
        if (stockin_ui_set_network(s->wifi, s->ethernet, lines[0], lines[1])) {
            memcpy(previous, lines, sizeof(lines));
            old_wifi = s->wifi;
            old_eth = s->ethernet;
        }
    }
}

static void network_task(void *arg)
{
    (void)arg;
    credentials_t pending = {0};
    bool testing = false;
    int64_t deadline = 0, retry_at = 0, offline_since = now_ms(), online_since = 0, ap_retry_at = 0;
    esp_netif_t *selected_route = NULL;
    int64_t known_deadline = 0;
    unsigned known_index = 0;
    for (;;) {
        network_status_t s;
        network_get_status(&s);
        int64_t now = now_ms();
        if (s.radio_ready && !known_deadline) known_deadline = now + FALLBACK_MS;
        if (s.wifi) {
            known_deadline = now + FALLBACK_MS;
            if (!testing && !s.configuring) {
                int index = network_profile_find(&s_profiles, s.ssid);
                if (index >= 0) { known_index = (unsigned)index; s_saved = s_profiles.entries[index]; }
            }
        }
        if (s.wifi || s.ethernet) { offline_since = now; if (!online_since) online_since = now; }
        else online_since = 0;
        if (s.radio_ready && !testing && xQueueReceive(s_requests, &pending, 0) == pdTRUE) {
            /* Respond to the browser before changing its network. Keep a recovery AP. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_err_t err = s.hotspot ? ESP_OK : hotspot_start();
            if (err == ESP_OK) {
                esp_wifi_disconnect();
                lock();
                s_status.wifi = false;
                s_status.wifi_ip[0] = 0;
                xEventGroupClearBits(s_events, WIFI_READY);
                unlock();
                err = set_station(&pending);
                if (err == ESP_OK) {
                    esp_wifi_connect();
                    testing = true;
                    deadline = now_ms() + CONNECT_TIMEOUT_MS;
                    retry_at = now_ms() + 10000;
                    online_since = 0;
                } else {
                    set_station(&s_saved);
                    esp_wifi_connect();
                }
            }
            if (err != ESP_OK) {
                message("Could not apply settings; previous Wi-Fi retained.");
                lock(); s_status.configuring = false; unlock();
                memset(&pending, 0, sizeof(pending));
            }
            network_get_status(&s);
        }
        if (testing && s.wifi) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && !strncmp((char *)ap.ssid, pending.ssid, 32)) {
                esp_err_t err = save_credentials(&pending);
                if (err == ESP_OK) {
                    s_saved = pending;
                    message("Connected. Wi-Fi settings saved for next power-on.");
                    ESP_LOGI(TAG, "PASS: new Wi-Fi validated by DHCP and saved");
                } else {
                    s_saved = pending; /* keep retrying the active network in RAM */
                    message("Connected, but saving failed. Re-enter settings before power-off.");
                }
                testing = false;
                known_index = 0;
                known_deadline = now_ms() + FALLBACK_MS;
                lock(); s_status.configuring = false; unlock();
                memset(&pending, 0, sizeof(pending));
            }
        }
        if (testing && now_ms() >= deadline) {
            esp_wifi_disconnect();
            esp_err_t err = set_station(&s_saved);
            if (err == ESP_OK) esp_wifi_connect();
            message(err == ESP_OK ? "Connection failed. Previous Wi-Fi restored; check SSID/password and try again." :
                    "Connection failed; could not restore Wi-Fi. Re-enter settings through the setup hotspot.");
            testing = false;
            int restored_index = network_profile_find(&s_profiles, s_saved.ssid);
            if (restored_index >= 0) known_index = (unsigned)restored_index;
            known_deadline = now_ms() + FALLBACK_MS;
            lock(); s_status.configuring = false; unlock();
            memset(&pending, 0, sizeof(pending));
            retry_at = now_ms() + 10000;
            ESP_LOGW(TAG, "New Wi-Fi timed out; previous settings retained");
        }
        if (s.radio_ready && !s.wifi && !testing && !s.configuring && s_profiles.count > 1 &&
            now_ms() >= known_deadline) {
            unsigned next = (known_index + 1) % s_profiles.count;
            esp_wifi_disconnect();
            esp_err_t err = set_station(&s_profiles.entries[next]);
            if (err == ESP_OK) {
                known_index = next;
                esp_wifi_connect();
                ESP_LOGI(TAG, "Trying known Wi-Fi profile %u/%u", next + 1, s_profiles.count);
            } else ESP_LOGW(TAG, "Known Wi-Fi switch failed: %s", esp_err_to_name(err));
            retry_at = now_ms() + 10000;
            known_deadline = now_ms() + FALLBACK_MS;
        }
        if (s.radio_ready && !s.wifi && now_ms() >= retry_at) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(err));
            retry_at = now_ms() + 10000;
        }
        if (s.radio_ready && !s.hotspot && !s.wifi && !s.ethernet &&
            now_ms() - offline_since >= FALLBACK_MS && now_ms() >= ap_retry_at) {
            hotspot_start();
            ap_retry_at = now_ms() + 30000;
        }
        if (s.hotspot && !testing && online_since && now_ms() - online_since >= AP_GRACE_MS) {
            wifi_sta_list_t clients = {0};
            if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num == 0 &&
                esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                lock(); s_status.hotspot = false; s_status.hotspot_ip[0] = 0; unlock();
                ESP_LOGI(TAG, "Setup hotspot stopped; network setup remains available on LAN");
            }
            online_since = now_ms();
        }
        network_get_status(&s);
        /* Select only interfaces with a lease, not merely a physical link.
         * esp-netif's priority scan can otherwise select an up/no-DHCP LAN. */
        const char *key = s.ethernet ? "ETH_DEF" : s.wifi ? "WIFI_STA_DEF" :
                          s.hotspot ? "WIFI_AP_DEF" : NULL;
        esp_netif_t *route = key ? esp_netif_get_handle_from_ifkey(key) : NULL;
        if (route && route != selected_route && esp_netif_set_default_netif(route) == ESP_OK) {
            selected_route = route;
            ESP_LOGI(TAG, "Default route: %s", s.ethernet ? "Ethernet" : s.wifi ? "Wi-Fi" : "setup only");
        }
        update_display(&s);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t network_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_requests = xQueueCreate(1, sizeof(credentials_t));
    if (!s_lock || !s_events || !s_requests) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(load_settings(), TAG, "network settings");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_ETH), TAG, "MAC");
    snprintf(s_status.hotspot_ssid, sizeof(s_status.hotspot_ssid), "Rearly-Setup-%02X%02X", mac[4], mac[5]);
    message("Ready to configure Wi-Fi.");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL), TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, network_event, NULL), TAG, "ETH events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL), TAG, "IP events");
    err = ethernet_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "Ethernet unavailable: %s; continuing with Wi-Fi", esp_err_to_name(err));
    err = network_web_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "Network setup server failed: %s", esp_err_to_name(err));
    if (xTaskCreatePinnedToCore(network_task, "network_setup", 6144, NULL, 4, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool network_wait_connected(uint32_t timeout_ms)
{
    return (xEventGroupWaitBits(s_events, UPLINK_READY, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms)) & UPLINK_READY) != 0;
}
