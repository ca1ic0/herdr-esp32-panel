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
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "app_config.h"

static const char *TAG = "provisioning";

#define AP_CHANNEL      1
#define DNS_PORT        53
#define HTTP_PORT       80
#define AP_PASS_LEN     8
#define EDIT_TIMEOUT_MS (5 * 60 * 1000)

static bool s_active;
static bool s_edit_mode;
static int64_t s_started_ms;
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
"<title>Herdr 面板设置</title><style>"
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
"textarea{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
"border:1px solid #293643;background:#151E27;color:#F2F6FA;font-size:15px}"
"button[type=submit]{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;"
"background:#69B5FF;color:#080D12;font-size:16px;font-weight:600}"
"#msg{margin-top:12px;font-size:13px;color:#FF6B68}"
".token{font-family:monospace;letter-spacing:.5px}"
"</style></head><body><h1>Herdr 面板设置</h1>"
"<p class='hint'>首次配网请填写连接信息；再次编辑时留空表示保留原值。编辑热点开放 5 分钟。</p>"
"<form method='POST' action='/save'>"
"<label>Wi-Fi 名称</label><div class='row'>"
"<input name='ssid' id='ssid' maxlength='32'>"
"<button type='button' class='scan' id='scanBtn' onclick='doScan()'>扫描</button></div>"
"<ul id='ssids'></ul>"
"<label>Wi-Fi 密码</label><input name='pass' type='password' maxlength='64'>"
"<label>网关地址（IP 或主机名）</label><input name='host' maxlength='63'>"
"<label>网关端口</label><input name='port' type='number' min='1' max='65535' placeholder='留空保留原端口'>"
"<label>设备专用网关令牌</label>"
"<input name='token' type='password' class='token' maxlength='95' "
"placeholder='留空保留原令牌'>"
"<label>继续提示词（UTF-8 最多 160 字节）</label>"
"<textarea name='prompt' rows='3' maxlength='160' placeholder='留空保留原提示词或使用网关默认值'></textarea>"
"<label><input name='reset_prompt' type='checkbox' value='1' style='width:auto'>恢复网关默认提示词</label>"
"<button type='submit'>保存并重新连接</button><div id='msg'></div></form>"
"<script>"
"function doScan(){"
" var b=document.getElementById('scanBtn');b.disabled=true;b.textContent='扫描中…';"
" var ul=document.getElementById('ssids');ul.innerHTML='<li>扫描中…</li>';"
" fetch('/scan').then(function(r){return r.json()}).then(function(d){"
"  ul.innerHTML='';var ss=d.ssids||[];"
"  if(!ss.length){ul.innerHTML='<li>未找到网络</li>';return}"
"  ss.forEach(function(n){var li=document.createElement('li');"
"   li.textContent=n;li.onclick=function(){document.getElementById('ssid').value=n};"
"   ul.appendChild(li)});"
" }).catch(function(){ul.innerHTML='<li>扫描失败</li>'})"
"  .then(function(){b.disabled=false;b.textContent='扫描'});"
"}"
"</script></body></html>";

static const char DONE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;background:#080D12;color:#F2F6FA;padding:24px'>"
"<h2>配置已保存</h2>"
"<p>设备正在重新连接。若网关不可达，请在屏幕上查看状态并重新编辑。</p>"
"<p>现在可以断开设备热点。</p></body></html>";

static const char ERR_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;background:#080D12;color:#FF6B68;padding:24px'>"
"<h2>保存失败</h2><p id='e'>请检查字段长度、地址和令牌。</p>"
"<p><a href='/' style='color:#69B5FF'>返回</a></p></body></html>";

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

static bool url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%') {
            if (!isxdigit((unsigned char)p[1]) ||
                !isxdigit((unsigned char)p[2])) return false;
            char hex[3] = { p[1], p[2], 0 };
            int value = (int)strtol(hex, NULL, 16);
            if (value == 0) return false;
            *o++ = (char)value;
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return true;
}

/* 0 = absent, 1 = present, -1 = malformed or too long. */
static int form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    for (const char *p = body; *p; ) {
        const char *end = strchr(p, '&');
        if (end == NULL) end = p + strlen(p);
        if ((size_t)(end - p) > key_len &&
            strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t len = (size_t)(end - p);
            if (len >= out_len) return -1;
            memcpy(out, p, len);
            out[len] = '\0';
            return url_decode(out) ? 1 : -1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    static char body[2048];  /* httpd serialises handlers; avoid a large task stack frame */
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

    char tmp[512];
    int field = form_get(body, "ssid", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        if (strlen(tmp) > CFG_SSID_MAX) goto bad_fields;
        snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", tmp);
    }
    field = form_get(body, "pass", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        if (strlen(tmp) > CFG_PASS_MAX) goto bad_fields;
        snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", tmp);
    }
    field = form_get(body, "host", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        if (strlen(tmp) > CFG_HOST_MAX) goto bad_fields;
        snprintf(cfg.backend_host, sizeof(cfg.backend_host), "%s", tmp);
    }
    field = form_get(body, "port", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        char *end;
        long port = strtol(tmp, &end, 10);
        if (*end != '\0' || port < 1 || port > 65535) goto bad_fields;
        cfg.backend_port = (uint16_t)port;
    }
    field = form_get(body, "token", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        if (strlen(tmp) > CFG_TOKEN_MAX) goto bad_fields;
        snprintf(cfg.gateway_token, sizeof(cfg.gateway_token), "%s", tmp);
    }
    field = form_get(body, "prompt", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && tmp[0]) {
        if (strlen(tmp) > CFG_PROMPT_MAX) goto bad_fields;
        snprintf(cfg.continue_prompt, sizeof(cfg.continue_prompt), "%s", tmp);
    }
    field = form_get(body, "reset_prompt", tmp, sizeof(tmp));
    if (field < 0) goto bad_fields;
    if (field && strcmp(tmp, "1") == 0) cfg.continue_prompt[0] = '\0';

    /* Required fields (PRODUCT_LOGIC §2.1) */
    if (cfg.wifi_ssid[0] == '\0' || cfg.backend_host[0] == '\0' ||
        cfg.backend_port == 0 || cfg.gateway_token[0] == '\0') {
        goto bad_fields;
    }

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ?
                              "400 Bad Request" : "500 Internal Server Error");
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

bad_fields:
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ERR_HTML, HTTPD_RESP_USE_STRLEN);
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

static void edit_timeout_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(EDIT_TIMEOUT_MS));
    if (s_active && s_edit_mode) {
        ESP_LOGI(TAG, "connection editor timed out");
        esp_restart();
    }
    vTaskDelete(NULL);
}

void provisioning_start(bool editing)
{
    if (s_active) return;
    s_active = true;
    s_edit_mode = editing;
    s_started_ms = esp_timer_get_time() / 1000;

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
    if (editing) {
        if (xTaskCreatePinnedToCore(edit_timeout_task, "edit_timeout", 2048,
                                    NULL, 3, NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "unable to enforce editor timeout");
            esp_restart();
        }
    }
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

int provisioning_remaining_seconds(void)
{
    if (!s_active || !s_edit_mode) return -1;
    int64_t elapsed = esp_timer_get_time() / 1000 - s_started_ms;
    int64_t left = EDIT_TIMEOUT_MS - elapsed;
    return left > 0 ? (int)((left + 999) / 1000) : 0;
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
