/*
 * Provisioning mode: SoftAP + captive-portal DNS + embedded config web page.
 *
 * - SoftAP "HerdrPanel-XXXX" (open network; join via on-screen QR code)
 * - DNS server answers every A query with our own IP (captive portal)
 * - HTTP server serves a small form: WiFi SSID/password + backend host/port
 * - On submit the config is saved to NVS and the device reboots
 */

#include "provisioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "app_config.h"

static const char *TAG = "provisioning";

#define AP_CHANNEL      1
#define DNS_PORT        53
#define HTTP_PORT       80

static bool s_active;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static char s_ap_ssid[24];

/* ------------------------------------------------------------------ */
/* HTML page (single file, no external assets)                         */
/* ------------------------------------------------------------------ */

static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Herdr Panel Setup</title><style>"
"body{font-family:sans-serif;background:#0f1419;color:#d7dee7;margin:0;padding:24px}"
"h1{font-size:20px;margin:0 0 16px}"
"label{display:block;font-size:13px;color:#8494a5;margin:12px 0 4px}"
"input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
"border:1px solid #27313c;background:#161c23;color:#d7dee7;font-size:15px}"
"button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;"
"background:#4c9aff;color:#fff;font-size:16px}"
"#msg{margin-top:12px;font-size:13px;color:#57ab5a}"
".row{margin-bottom:4px}"
"</style></head><body><h1>Herdr Panel 配网</h1>"
"<form method='POST' action='/save'>"
"<label>WiFi SSID</label><input name='ssid' id='ssid' required>"
"<label>WiFi 密码</label><input name='pass' type='password'>"
"<label>后端地址 (IP 或主机名)</label><input name='host' required>"
"<label>后端端口</label><input name='port' type='number' value='8080' required>"
"<button type='submit'>保存并重启</button><div id='msg'></div></form>"
"<script>"
"fetch('/scan').then(r=>r.json()).then(d=>{"
" if(d.ssids&&d.ssids.length){var s=document.createElement('select');"
" s.onchange=function(){document.getElementById('ssid').value=this.value};"
" d.ssids.forEach(function(n){var o=document.createElement('option');"
" o.textContent=n;s.appendChild(o)});"
" document.getElementById('ssid').parentNode.insertBefore(s,document.getElementById('ssid').nextSibling);"
" }});"
"</script></body></html>";

static const char DONE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head><body style='font-family:sans-serif'>"
"<h2>已保存，设备正在重启…</h2><p>可以断开此热点，稍后在 herdr 面板屏幕上查看连接状态。</p></body></html>";

/* ------------------------------------------------------------------ */
/* DNS server: answer every A query with our AP address (captive)      */
/* ------------------------------------------------------------------ */

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    uint8_t buf[512];
    while (s_active) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (len < 12) {
            continue;
        }
        /* build a minimal DNS response: copy question, answer = our IP */
        buf[2] = 0x81;              /* response + recursion available */
        buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;   /* 1 answer */
        int off = len;
        if (off + 16 > (int)sizeof(buf)) {
            continue;
        }
        buf[off++] = 0xC0; buf[off++] = 0x0C;   /* pointer to question name */
        buf[off++] = 0x00; buf[off++] = 0x01;   /* type A */
        buf[off++] = 0x00; buf[off++] = 0x01;   /* class IN */
        buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x3C; /* TTL 60 */
        buf[off++] = 0x00; buf[off++] = 0x04;   /* length 4 */
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif != NULL) {
            esp_netif_get_ip_info(netif, &ip);
        } else {
            ip.ip.addr = htonl(0xC0A80401);     /* 192.168.4.1 fallback */
        }
        memcpy(&buf[off], &ip.ip.addr, 4);
        off += 4;
        sendto(sock, buf, off, 0, (struct sockaddr *)&src, slen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* HTTP config page                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    /* synchronous WiFi scan of surrounding APs */
    wifi_scan_config_t scfg = { .show_hidden = false };
    esp_wifi_scan_start(&scfg, true);

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = calloc(n > 0 ? n : 1, sizeof(*recs));
    if (recs == NULL) {
        httpd_resp_sendstr(req, "{\"ssids\":[]}");
        return ESP_OK;
    }
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);

    char out[2048];
    int pos = snprintf(out, sizeof(out), "{\"ssids\":[");
    for (int i = 0; i < got && pos < (int)sizeof(out) - 40; i++) {
        pos += snprintf(out + pos, sizeof(out) - pos, "%s\"%s\"",
                        i ? "," : "", (char *)recs[i].ssid);
    }
    snprintf(out + pos, sizeof(out) - pos, "]}");
    free(recs);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* minimal url-decode of form values */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    url_decode(out);
    return true;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    app_config_t cfg;
    app_config_get(&cfg);

    char tmp[80];
    if (form_get(body, "ssid", tmp, sizeof(tmp))) {
        snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%.32s", tmp);
    }
    if (form_get(body, "pass", tmp, sizeof(tmp))) {
        snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%.64s", tmp);
    }
    if (form_get(body, "host", tmp, sizeof(tmp))) {
        snprintf(cfg.backend_host, sizeof(cfg.backend_host), "%.63s", tmp);
    }
    if (form_get(body, "port", tmp, sizeof(tmp))) {
        int port = atoi(tmp);
        if (port > 0 && port < 65536) {
            cfg.backend_port = (uint16_t)port;
        }
    }

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "config saved: ssid=%s host=%s:%u",
             cfg.wifi_ssid, cfg.backend_host, cfg.backend_port);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, DONE_HTML, HTTPD_RESP_USE_STRLEN);

    /* reboot shortly after the response goes out */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root,
    };
    static const httpd_uri_t scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = handle_scan,
    };
    static const httpd_uri_t save = {
        .uri = "/save", .method = HTTP_POST, .handler = handle_save,
    };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
}

/* ------------------------------------------------------------------ */

void provisioning_start(void)
{
    if (s_active) {
        return;
    }
    s_active = true;

    /* AP SSID derived from MAC: HerdrPanel-XXXX */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "HerdrPanel-%02X%02X", mac[4], mac[5]);
    ESP_LOGI(TAG, "starting provisioning AP \"%s\"", s_ap_ssid);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreatePinnedToCore(dns_task, "prov_dns", 4 * 1024, NULL, 5, &s_dns_task, 0);
    start_httpd();
}

bool provisioning_active(void)
{
    return s_active;
}

void provisioning_get_ap_ssid(char *buf, int buflen)
{
    snprintf(buf, buflen, "%s", s_ap_ssid);
}

void provisioning_get_qr_payload(char *buf, int buflen)
{
    /* WiFi join payload: scanned phones connect to the AP automatically,
     * then the captive portal pops the config page. */
    snprintf(buf, buflen, "WIFI:T:nopass;S:%s;;", s_ap_ssid);
}
