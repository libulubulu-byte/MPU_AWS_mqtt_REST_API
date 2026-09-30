/* webprov.c - SoftAP web provisioning: WiFi only (SSID + password)
 *
 * The page has exactly two input fields. Backend selection, the REST server
 * host/port/paths, the AWS endpoint and the Thing name are all compile-time
 * macros and are never exposed on the page:
 *   main/app_config.h  (backend, REST parameters, firmware version, intervals)
 *   main/aws_certs.h   (AWS endpoint / Thing name / the three PEM blobs)
 * To change any of them edit the header and reflash - one less round trip
 * than typing them into a form over the provisioning hotspot.
 *
 * The upside is straightforward:
 *   - the page stays tiny, fast, and needs no long address typed by hand;
 *   - private keys and server addresses never travel over the unencrypted
 *     SoftAP HTTP link;
 *   - swapping the router on site only means re-entering WiFi, while
 *     "which server does this device talk to" is part of the firmware.
 *
 * Note: this httpd runs inside the SoftAP with no auth and no TLS. It is only
 * meant for a short session while connected to the device hotspot; the device
 * reboots and shuts the hotspot down once the form is saved.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "webprov.h"
#include "app_cfg.h"
#include "app_version.h"

static const char *TAG = "webprov";
static httpd_handle_t s_server = NULL;

/* Page head, stopping right at the <form> start tag; send_input() emits the
 * actual fields so that their values can be injected at request time. */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>WiFi Setup</title></head>"
    "<body style='font-family:sans-serif;background:#f3f4f6;margin:0;padding:20px;'>"
    "<div style='max-width:430px;margin:auto;background:#fff;border-radius:12px;"
    "padding:24px;box-shadow:0 2px 10px rgba(0,0,0,.1);'>"
    "<h2 style='margin-top:0;'>WiFi Setup</h2>"
    "<p style='color:#666;font-size:13px;margin-top:-6px;'>"
    "Only the WiFi credentials are entered here. The server address and the "
    "reporting mode are compiled into the firmware.</p>"
    "<form method='POST' action='/save'>";

static const char PAGE_TAIL[] =
    "<input type='submit' value='Save &amp; Reboot' "
    "style='width:100%;padding:12px;background:#0b57d0;color:#fff;border:none;"
    "border-radius:6px;font-size:16px;'></input>"
    "</form>"
    "<p style='color:#666;font-size:12px;margin-bottom:0;'>Firmware " APP_FW_VERSION
    ". Server settings: main/app_config.h."
    "</p></div></body></html>";

/* Decode a form-urlencoded value (handles %xx and '+') */
static void url_decode(const char *src, char *dst, int dst_size)
{
    int o = 0;
    for (int i = 0; src[i] && o < dst_size - 1; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* Pull one field out of a raw "a=1&b=2" body */
static void field_of(const char *body, const char *name, char *dst, int size)
{
    dst[0] = '\0';
    const char *p = body;
    size_t nl = strlen(name);
    while (p && *p) {
        const char *e = strchr(p, '&');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            char tmp[512];
            size_t cl = seg - nl - 1;
            if (cl >= sizeof(tmp)) {
                cl = sizeof(tmp) - 1;
            }
            memcpy(tmp, p + nl + 1, cl);
            tmp[cl] = '\0';
            url_decode(tmp, dst, size);
            return;
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
}

/* Strip characters that would break out of an HTML attribute */
static void html_safe(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_sz; i++) {
        char c = in[i];
        if (c != '\'' && c != '"' && c != '<' && c != '>' && c != '&') {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* Emit one input field; value is pre-filled, placeholder shows when empty */
static void send_input(httpd_req_t *req, const char *label, const char *name,
                       const char *value, const char *placeholder,
                       bool password, bool required)
{
    char safe[CFG_SSID_LEN * 2 + 1];
    html_safe(value, safe, sizeof(safe));

    char buf[720];
    snprintf(buf, sizeof(buf),
             "<label><b>%s</b></label><br>"
             "<input name='%s' %s%svalue='%s' "
             "style='width:100%%;padding:10px;margin:6px 0 14px;border:1px solid "
             "#ccc;border-radius:6px;box-sizing:border-box;' "
             "placeholder='%s'></input><br>",
             label, name,
             password ? "type='password' " : "",
             required ? "required " : "",
             safe, placeholder);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Config page: pre-fill the current SSID, never pre-fill the password
 * (leaving it empty keeps the stored one) */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    app_cfg_t *cfg = malloc(sizeof(app_cfg_t));
    if (!cfg) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    cfg_load(cfg);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);

    send_input(req, "WiFi SSID", "ssid", cfg->ssid, "e.g. my-wifi", false, true);
    send_input(req, "WiFi Password", "pass", "", "leave empty to keep current",
               true, false);

    free(cfg);

    httpd_resp_send_chunk(req, PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked response */
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[256] = { 0 };
    int total = req->content_len;
    if (total >= (int)sizeof(body)) {
        total = sizeof(body) - 1;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    body[got] = '\0';

    /* Each field gets a temp buffer the same size as its destination: with
     * source and target equally large, gcc has no reason to warn about
     * -Wformat-truncation (which this project promotes to an error). */
    char f_ssid[CFG_SSID_LEN];
    char f_pass[CFG_PASS_LEN];

    field_of(body, "ssid", f_ssid, sizeof(f_ssid));
    field_of(body, "pass", f_pass, sizeof(f_pass));

    if (f_ssid[0] == '\0') {
        return httpd_resp_send(req, "<h3>SSID must not be empty</h3>",
                               HTTPD_RESP_USE_STRLEN);
    }

    /* An empty password field means "keep the stored password" */
    esp_err_t err = ESP_OK;
    nvs_handle_t h;
    err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", f_ssid);
        if (err == ESP_OK && f_pass[0]) {
            err = nvs_set_str(h, "pass", f_pass);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }

    /* Also dump the compile-time server settings to the console so it is easy
     * to confirm where this particular device is about to report to. */
    app_cfg_t *cfg = malloc(sizeof(app_cfg_t));
    if (cfg) {
        cfg_load(cfg);
        ESP_LOGI(TAG, "rebooting: ssid=%s backend=%s rest=%s://%s:%d%s",
                 cfg->ssid, backend_name(cfg->backend),
                 cfg->rest_use_tls ? "https" : "http", cfg->rest_host,
                 cfg->rest_port, cfg->rest_path);
        free(cfg);
    }

    const char *html = (err == ESP_OK)
        ? "<html><meta charset='utf-8'><body style='text-align:center;"
          "padding:60px;font-family:sans-serif;'><h2 style='color:#0b57d0;'>"
          "Saved! Rebooting...</h2></body></html>"
        : "<html><body style='text-align:center;padding:60px;'>"
          "<h2>Save failed, please retry</h2></body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return ESP_OK;
}

esp_err_t webprov_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;   /* default 4096 is tight once page building is added */
    cfg.max_uri_handlers = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    /* Register the exact URI first and the GET wildcard last: httpd matches in
     * registration order, so a leading wildcard would swallow everything. */
    httpd_uri_t up_save = { .uri = "/save", .method = HTTP_POST,
                            .handler = save_post_handler };
    /* GET wildcard: any other URI (including phone captive-portal probes such
     * as /generate_204) returns the config page. */
    httpd_uri_t up_page = { .uri = "/*", .method = HTTP_GET,
                            .handler = page_get_handler };

    httpd_register_uri_handler(s_server, &up_save);
    httpd_register_uri_handler(s_server, &up_page);

    ESP_LOGI(TAG, "wifi config page ready: http://192.168.4.1/");
    return ESP_OK;
}
