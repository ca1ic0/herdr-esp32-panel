/*
 * WiFi station connection with automatic retry.
 */

#include "wifi_connect.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "provisioning.h"

static const char *TAG = "wifi_connect";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;
static char s_ip[16];
static int s_retry;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_retry < 0) {
            /* unlimited retries: the panel must recover on its own */
            esp_wifi_connect();
        } else if (s_retry < 10) {
            s_retry++;
            esp_wifi_connect();
        }
        ESP_LOGW(TAG, "disconnected, retrying");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = -1;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

void wifi_connect_start(void)
{
    s_wifi_events = xEventGroupCreate();

    /* NVS + config are owned by app_config_init() called from app_main
     * before this function. wifi_connect must not re-init NVS. */

    /* both paths (provisioning AP and normal STA) need the TCP/IP stack
     * and the default event loop — must exist before any netif is created */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* unconfigured device: provisioning mode owns WiFi (AP + portal) */
    bool configured = app_config_is_configured();
    bool editing = configured && app_config_take_edit_request();
    if (!configured || editing) {
        if (editing) ESP_LOGI(TAG, "entering temporary connection editor");
        else ESP_LOGI(TAG, "no WiFi credentials stored, entering provisioning");
        provisioning_start(editing);
        return;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    app_config_t ac;
    app_config_get(&ac);

    wifi_config_t sta_config = { 0 };
    memcpy(sta_config.sta.ssid, ac.wifi_ssid,
           strnlen(ac.wifi_ssid, sizeof(sta_config.sta.ssid)));
    memcpy(sta_config.sta.password, ac.wifi_pass,
           strnlen(ac.wifi_pass, sizeof(sta_config.sta.password)));
    sta_config.sta.threshold.authmode =
        ac.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"", ac.wifi_ssid);
}

bool wifi_is_connected(void)
{
    return s_wifi_events != NULL &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_get_ip(char *buf, int buflen)
{
    if (buf == NULL || buflen <= 0) {
        return;
    }
    snprintf(buf, buflen, "%s", s_ip);
}
