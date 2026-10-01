/*
 * Provisioning mode: WPA2 SoftAP + captive-portal DNS + config web page.
 *
 * - SoftAP "HerdrPanel-XXXX" with a random 8-char WPA2 password
 * - QR code encodes only the AP join payload
 * - Form fields: Wi-Fi SSID/password, gateway host/port, device token
 * - On submit: validate → NVS → respond → tear down → restart
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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "app_config.h"

static const char *TAG = "provisioning";

#define AP_CHANNEL      1
#define DNS_PORT        53
#define HTTP_PORT       80
#define AP_PASS_LEN     8

static bool s_active;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static char s_ap_ssid[24];
static char s_ap_pass[AP_PASS_LEN + 1];

/* ------------------------------------------------------------------ */
/* HTML page                                                           */
/* ------------------------------------------------------------------ */

static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Herdr Panel Setup</title><style>"
"body{font-family:sans-serif;background:#080D12;color:#F2F6FA;margin:0;padding:24px}"
"h1{font-size:20px;margin:0 0 8px}"
"p.hint{color:#AEBCC9;font-size:13px;margin:0 0 16px}"
"label{display:block;font-size:13px;color:#AEBCC9;margin:12px 0 4px}"
"input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
"border:1px solid #293643;background:#151E27;color:#F2F6FA;font-size:15px}"
".row{display:flex;gap:8px;align-items:center}"
".row input{flex:1}"
".scan{margin:0;flex:0 0 auto;width:auto;padding:10px 16px;border-radius:8px;"
"border:1px solid #69B5FF;background:transparent;color:#69B5FF;font-size:14px}"
".scan:disabled{opacity:.5}"
"#ssids{list-style:none;padding:0;margin:8px 0 0;max-height:180px;overflow:auto}"
"#ssids li{padding:10px 12px;background:#151E27;border:1px solid #293643;"
"border-radius:8px;margin-bottom:6px;font-size:15px}"
"button[type=submit]{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;"
"background:#69B5FF;color:#080D12;font-size:16px;font-weight:600}"
"#msg{margin-top:12px;font-size:13px;color:#FF6B68}"
".token{font-family:monospace;letter-spacing:.5px}"
"</style></head><body><h1>Herdr Panel Setup</h1>"
"<p class='hint'>Enter your home Wi-Fi and gateway info. Generate the device token on the host gateway first.</p>"
"<form method='POST' action='/save'>"
"<label>Wi-Fi SSID</label><div class='row'>"
"<input name='ssid' id='ssid' required maxlength='32'>"
"<button type='button' class='scan' id='scanBtn' onclick='doScan()'>Scan</button></div>"
"<ul id='ssids'></ul>"
"<label>Wi-Fi Password</label><input name='pass' type='password' maxlength='64'>"
"<label>Gateway host (IP or hostname)</label><input name='host' required maxlength='63'>"
"<label>Gateway port</label><input name='port' type='number' value='8080' required min='1' max='65535'>"
"<label>Device gateway token (required)</label>"
"<input name='token' type='password' class='token' required maxlength='95' "
"placeholder='Per-device token from the host gateway'>"
"<button type='submit'>Save and restart</button><div id='msg'></div></form>"
"<script>"
"function doScan(){"
" var b=document.getElementById('scanBtn');b.disabled=true;b.textContent='Scanning...';"
" var ul=document.getElementById('ssids');ul.innerHTML='<li>Scanning...</li>';"
" fetch('/scan').then(function(r){return r.json()}).then(function(d){"
"  ul.innerHTML='';var ss=d.ssids||[];"
"  if(!ss.length){ul.innerHTML='<li>No networks found</li>';return}"
"  ss.forEach(function(n){var li=document.createElement('li');"
"   li.textContent=n;li.onclick=function(){document.getElementById('ssid').value=n};"
"   ul.appendChild(li)});"
" }).catch(function(){ul.innerHTML='<li>Scan failed</li>'})"
"  .then(function(){b.disabled=false;b.textContent='Scan'});"
"}"
"</script></body></html>";

static const char DONE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;background:#080D12;color:#F2F6FA;padding:24px'>"
"<h2>Configuration saved</h2>"
"<p>The device is connecting to Wi-Fi. If the server is unreachable the screen will say so; re-enter setup to edit.</p>"
"<p>You can now disconnect from the device hotspot.</p></body></html>";

static const char ERR_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;background:#080D12;color:#FF6B68;padding:24px'>"
"<h2>Save failed</h2><p id='e'>Check the fields and try again.</p>"
"<p><a href='/' style='color:#69B5FF'>Back</a></p></body></html>";

/* ------------------------------------------------------------------ */
/* DNS captive portal                                                  */
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
        if (len < 12) continue;
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;
        int off = len;
        if (off + 16 > (int)sizeof(buf)) continue;
        buf[off++] = 0xC0; buf[off++] = 0x0C;
        buf[off++] = 0x00; buf[off++] = 0x01;
        buf[off++] = 0x00; buf[off++] = 0x01;
        buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x3C;
        buf[off++] = 0x00; buf[off++] = 0x04;
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif != NULL) {
            esp_netif_get_ip_info(netif, &ip);
        } else {
            ip.ip.addr = htonl(0xC0A80401);
        }
        memcpy(&buf[off], &ip.ip.addr, 4);
        off += 4;
        sendto(sock, buf, off, 0, (struct sockaddr *)&src, slen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
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
    if (p == NULL) return false;
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
    char body[768];
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

    char tmp[128];
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
    if (form_get(body, "token", tmp, sizeof(tmp))) {
        snprintf(cfg.gateway_token, sizeof(cfg.gateway_token), "%.95s", tmp);
    }

    /* Required fields (PRODUCT_LOGIC §2.1) */
    if (cfg.wifi_ssid[0] == '\0' || cfg.backend_host[0] == '\0' ||
        cfg.backend_port == 0 || cfg.gateway_token[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, ERR_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, ERR_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Never log the token or Wi-Fi password. */
    ESP_LOGI(TAG, "config saved: ssid_len=%u host=%s:%u token=%s",
             (unsigned)strlen(cfg.wifi_ssid), cfg.backend_host, cfg.backend_port,
             cfg.gateway_token[0] ? "set" : "empty");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, DONE_HTML, HTTPD_RESP_USE_STRLEN);

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
/* public API                                                          */
/* ------------------------------------------------------------------ */

static void gen_ap_password(char *out, size_t len)
{
    /* 8 chars from unambiguous alphabet; not derived from MAC */
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    if (len < 9) return;
    for (size_t i = 0; i < 8; i++) {
        out[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    out[8] = '\0';
}

void provisioning_start(void)
{
    if (s_active) return;
    s_active = true;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "HerdrPanel-%02X%02X", mac[4], mac[5]);
    gen_ap_password(s_ap_pass, sizeof(s_ap_pass));

    /* Do not log the temporary password. */
    ESP_LOGI(TAG, "starting WPA2 provisioning AP \"%s\"", s_ap_ssid);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_ap_pass);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreatePinnedToCore(dns_task, "prov_dns", 4 * 1024, NULL, 5, &s_dns_task, 0);
    start_httpd();
}

void provisioning_stop(void)
{
    if (!s_active) return;
    s_active = false;

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    /* wipe temporary password */
    memset(s_ap_pass, 0, sizeof(s_ap_pass));
    ESP_LOGI(TAG, "provisioning stopped, AP credentials wiped");
}

bool provisioning_active(void)
{
    return s_active;
}

void provisioning_get_ap_ssid(char *buf, int buflen)
{
    snprintf(buf, buflen, "%s", s_ap_ssid);
}

void provisioning_get_ap_pass(char *buf, int buflen)
{
    snprintf(buf, buflen, "%s", s_ap_pass);
}

void provisioning_get_qr_payload(char *buf, int buflen)
{
    /* Standard Wi-Fi join payload. WPA2 with the temporary AP password.
     * Never encodes home Wi-Fi credentials or the gateway token. */
    snprintf(buf, buflen, "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
}
