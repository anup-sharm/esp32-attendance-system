#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#define TAG "ATTENDANCE"

/* =========================================================
   WIFI SETTINGS
   ========================================================= */

#define SETUP_AP_SSID       "Attendance-Setup"
#define SETUP_AP_PASSWORD   "12345678"

#define MAX_RETRY           15

#define WIFI_NAMESPACE      "wifi_config"
#define WIFI_SSID_KEY       "ssid"
#define WIFI_PASSWORD_KEY   "password"

static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int retry_count = 0;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif  = NULL;

static httpd_handle_t server = NULL;


/* =========================================================
   HTML PAGE
   ========================================================= */

static const char *setup_html =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Attendance System Setup</title>"
"<style>"
"body{font-family:Arial;background:#f2f2f2;margin:0;padding:20px;}"
".box{max-width:450px;margin:auto;background:white;padding:25px;"
"border-radius:15px;box-shadow:0 4px 15px rgba(0,0,0,.15);}"
"h1{text-align:center;color:#222;}"
"label{display:block;margin-top:18px;font-weight:bold;}"
"input{width:100%;box-sizing:border-box;padding:12px;margin-top:7px;"
"border:1px solid #ccc;border-radius:8px;font-size:16px;}"
"button{width:100%;padding:13px;margin-top:25px;border:0;"
"border-radius:8px;background:#1677ff;color:white;font-size:17px;}"
".info{background:#eef5ff;padding:12px;border-radius:8px;"
"margin-top:15px;font-size:14px;}"
"</style>"
"</head>"
"<body>"
"<div class='box'>"
"<h1>Attendance System</h1>"
"<p style='text-align:center;'>Wi-Fi Configuration</p>"

"<form action='/save' method='POST'>"

"<label>Office Wi-Fi Name (SSID)</label>"
"<input name='ssid' type='text' maxlength='31' required "
"placeholder='Enter Wi-Fi name'>"

"<label>Wi-Fi Password</label>"
"<input name='password' type='password' maxlength='63' "
"placeholder='Enter Wi-Fi password'>"

"<button type='submit'>Save Wi-Fi & Restart</button>"

"</form>"

"<div class='info'>"
"<b>After saving:</b><br>"
"ESP32-S3 will restart automatically and connect to the saved Wi-Fi."
"</div>"

"</div>"
"</body>"
"</html>";


/* =========================================================
   URL DECODE
   ========================================================= */

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    return -1;
}


static void url_decode(char *src)
{
    char *dst = src;

    while (*src)
    {
        if (*src == '+')
        {
            *dst++ = ' ';
            src++;
        }
        else if (*src == '%' &&
                 src[1] &&
                 src[2])
        {
            int high = hex_to_int(src[1]);
            int low  = hex_to_int(src[2]);

            if (high >= 0 && low >= 0)
            {
                *dst++ = (char)((high << 4) | low);
                src += 3;
            }
            else
            {
                *dst++ = *src++;
            }
        }
        else
        {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}


/* =========================================================
   SAVE WIFI CREDENTIALS
   ========================================================= */

static esp_err_t save_wifi_credentials(
    const char *ssid,
    const char *password)
{
    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open(
        WIFI_NAMESPACE,
        NVS_READWRITE,
        &nvs_handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS open failed: %s",
                 esp_err_to_name(err));

        return err;
    }

    err = nvs_set_str(
        nvs_handle,
        WIFI_SSID_KEY,
        ssid
    );

    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(
        nvs_handle,
        WIFI_PASSWORD_KEY,
        password
    );

    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Wi-Fi credentials saved successfully");
    }

    return err;
}


/* =========================================================
   LOAD WIFI CREDENTIALS
   ========================================================= */

static bool load_wifi_credentials(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size)
{
    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open(
        WIFI_NAMESPACE,
        NVS_READONLY,
        &nvs_handle
    );

    if (err != ESP_OK)
    {
        return false;
    }

    size_t ssid_len = ssid_size;
    size_t pass_len = password_size;

    err = nvs_get_str(
        nvs_handle,
        WIFI_SSID_KEY,
        ssid,
        &ssid_len
    );

    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_str(
        nvs_handle,
        WIFI_PASSWORD_KEY,
        password,
        &pass_len
    );

    nvs_close(nvs_handle);

    if (err != ESP_OK)
    {
        return false;
    }

    if (strlen(ssid) == 0)
    {
        return false;
    }

    return true;
}


/* =========================================================
   CLEAR WIFI CREDENTIALS
   ========================================================= */

static void clear_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;

    if (nvs_open(
            WIFI_NAMESPACE,
            NVS_READWRITE,
            &nvs_handle) == ESP_OK)
    {
        nvs_erase_key(
            nvs_handle,
            WIFI_SSID_KEY
        );

        nvs_erase_key(
            nvs_handle,
            WIFI_PASSWORD_KEY
        );

        nvs_commit(nvs_handle);

        nvs_close(nvs_handle);

        ESP_LOGI(TAG,
                 "Saved Wi-Fi credentials deleted");
    }
}


/* =========================================================
   WIFI EVENT HANDLER
   ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG,
                     "STA started");

            esp_wifi_connect();
        }

        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            if (retry_count < MAX_RETRY)
            {
                esp_wifi_connect();

                retry_count++;

                ESP_LOGW(
                    TAG,
                    "Wi-Fi connection failed. Retry %d/%d",
                    retry_count,
                    MAX_RETRY
                );
            }
            else
            {
                ESP_LOGE(
                    TAG,
                    "Unable to connect to saved Wi-Fi"
                );

                xEventGroupSetBits(
                    wifi_event_group,
                    WIFI_FAIL_BIT
                );
            }
        }
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Wi-Fi connected!"
        );

        ESP_LOGI(
            TAG,
            "IP Address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        retry_count = 0;

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}


/* =========================================================
   HTTP GET /
   ========================================================= */

static esp_err_t root_get_handler(
    httpd_req_t *req)
{
    httpd_resp_set_type(
        req,
        "text/html"
    );

    httpd_resp_send(
        req,
        setup_html,
        HTTPD_RESP_USE_STRLEN
    );

    return ESP_OK;
}


/* =========================================================
   HTTP POST /save
   ========================================================= */

static esp_err_t save_post_handler(
    httpd_req_t *req)
{
    char content[512];

    int total_len = req->content_len;

    if (total_len >= sizeof(content))
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Data too large"
        );

        return ESP_FAIL;
    }

    int received = 0;

    while (received < total_len)
    {
        int ret = httpd_req_recv(
            req,
            content + received,
            total_len - received
        );

        if (ret <= 0)
        {
            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Receive failed"
            );

            return ESP_FAIL;
        }

        received += ret;
    }

    content[received] = '\0';

    char ssid[32] = {0};
    char password[64] = {0};

    char *ssid_start =
        strstr(content, "ssid=");

    char *password_start =
        strstr(content, "&password=");

    if (!ssid_start ||
        !password_start)
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid Wi-Fi data"
        );

        return ESP_FAIL;
    }

    ssid_start += 5;

    size_t ssid_len =
        password_start - ssid_start;

    if (ssid_len >= sizeof(ssid))
        ssid_len = sizeof(ssid) - 1;

    memcpy(
        ssid,
        ssid_start,
        ssid_len
    );

    ssid[ssid_len] = '\0';

    password_start += 10;

    strncpy(
        password,
        password_start,
        sizeof(password) - 1
    );

    url_decode(ssid);
    url_decode(password);

    ESP_LOGI(
        TAG,
        "Received Wi-Fi SSID: %s",
        ssid
    );

    if (strlen(ssid) == 0)
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "SSID cannot be empty"
        );

        return ESP_FAIL;
    }

    esp_err_t err =
        save_wifi_credentials(
            ssid,
            password
        );

    if (err != ESP_OK)
    {
        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to save Wi-Fi"
        );

        return ESP_FAIL;
    }

    const char *response =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>"
        "body{font-family:Arial;text-align:center;"
        "padding:50px;background:#f2f2f2;}"
        ".box{background:white;padding:30px;"
        "border-radius:15px;max-width:450px;"
        "margin:auto;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='box'>"
        "<h2>Wi-Fi Saved</h2>"
        "<p>ESP32-S3 will restart and connect "
        "automatically.</p>"
        "<p>You can close this page.</p>"
        "</div>"
        "</body>"
        "</html>";

    httpd_resp_set_type(
        req,
        "text/html"
    );

    httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );

    vTaskDelay(
        pdMS_TO_TICKS(1500)
    );

    ESP_LOGI(
        TAG,
        "Restarting ESP32-S3..."
    );

    esp_restart();

    return ESP_OK;
}


/* =========================================================
   HTTP SERVER
   ========================================================= */

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;

    httpd_handle_t server_handle = NULL;

    if (httpd_start(
            &server_handle,
            &config) != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP server start failed"
        );

        return NULL;
    }

    httpd_uri_t root_uri =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        server_handle,
        &root_uri
    );

    httpd_uri_t save_uri =
    {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        server_handle,
        &save_uri
    );

    ESP_LOGI(
        TAG,
        "Web server started"
    );

    return server_handle;
}


/* =========================================================
   STOP WEB SERVER
   ========================================================= */

static void stop_web_server(void)
{
    if (server != NULL)
    {
        httpd_stop(server);
        server = NULL;

        ESP_LOGI(
            TAG,
            "Web server stopped"
        );
    }
}


/* =========================================================
   START SETUP AP
   ========================================================= */

static void start_setup_ap(void)
{
    ESP_LOGI(
        TAG,
        "Starting Wi-Fi Setup AP..."
    );

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config =
    {
        .ap =
        {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = 1,
            .password = SETUP_AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_AP)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &ap_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "SETUP MODE ACTIVE"
    );

    ESP_LOGI(
        TAG,
        "Wi-Fi: %s",
        SETUP_AP_SSID
    );

    ESP_LOGI(
        TAG,
        "Password: %s",
        SETUP_AP_PASSWORD
    );

    ESP_LOGI(
        TAG,
        "Open: http://192.168.4.1"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    server = start_web_server();
}


/* =========================================================
   CONNECT TO SAVED WIFI
   ========================================================= */

static bool connect_saved_wifi(
    const char *ssid,
    const char *password)
{
    ESP_LOGI(
        TAG,
        "Connecting to saved Wi-Fi..."
    );

    ESP_LOGI(
        TAG,
        "SSID: %s",
        ssid
    );

    wifi_config_t wifi_config;

    memset(
        &wifi_config,
        0,
        sizeof(wifi_config)
    );

    strncpy(
        (char *)wifi_config.sta.ssid,
        ssid,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        password,
        sizeof(wifi_config.sta.password) - 1
    );

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    retry_count = 0;

    xEventGroupClearBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT |
        WIFI_FAIL_BIT
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT |
            WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)
        );

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(
            TAG,
            "======================================"
        );

        ESP_LOGI(
            TAG,
            "NORMAL WIFI MODE ACTIVE"
        );

        ESP_LOGI(
            TAG,
            "ESP32-S3 connected to office Wi-Fi"
        );

        ESP_LOGI(
            TAG,
            "======================================"
        );

        return true;
    }

    ESP_LOGE(
        TAG,
        "Saved Wi-Fi connection failed"
    );

    esp_wifi_stop();

    return false;
}


/* =========================================================
   WIFI INITIALIZATION
   ========================================================= */

static void wifi_init_system(void)
{
    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    wifi_event_group =
        xEventGroupCreate();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );
}


/* =========================================================
   APP MAIN
   ========================================================= */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "   ESP32-S3 ATTENDANCE SYSTEM"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    /* -------------------------------------
       NVS
       ------------------------------------- */

    esp_err_t ret =
        nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );
    }

    /* -------------------------------------
       Wi-Fi system
       ------------------------------------- */

    wifi_init_system();

    /* -------------------------------------
       Load saved Wi-Fi
       ------------------------------------- */

    char ssid[32] = {0};
    char password[64] = {0};

    bool saved =
        load_wifi_credentials(
            ssid,
            sizeof(ssid),
            password,
            sizeof(password)
        );

    if (!saved)
    {
        ESP_LOGI(
            TAG,
            "No saved Wi-Fi configuration found"
        );

        ESP_LOGW(
            TAG,
            "Entering Wi-Fi SETUP MODE"
        );

        start_setup_ap();

        return;
    }

    /* -------------------------------------
       Saved Wi-Fi exists
       ------------------------------------- */

    ESP_LOGI(
        TAG,
        "Saved Wi-Fi configuration found"
    );

    bool connected =
        connect_saved_wifi(
            ssid,
            password
        );

    /* -------------------------------------
       If connection failed
       ------------------------------------- */

    if (!connected)
    {
        ESP_LOGW(
            TAG,
            "Falling back to SETUP MODE"
        );

        /*
         * Restart Wi-Fi driver cleanly
         */

        esp_wifi_deinit();

        wifi_init_config_t cfg =
            WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(
            esp_wifi_init(&cfg)
        );

        ESP_ERROR_CHECK(
            esp_event_handler_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &wifi_event_handler,
                NULL
            )
        );

        ESP_ERROR_CHECK(
            esp_event_handler_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &wifi_event_handler,
                NULL
            )
        );

        start_setup_ap();

        return;
    }

    /* -------------------------------------
       NORMAL MODE
       ------------------------------------- */

    ESP_LOGI(
        TAG,
        "Attendance system ready"
    );

    /*
     * Future modules will start here:
     *
     * Camera
     * Face detection
     * Face recognition
     * TFT
     * DS3231
     * SD card
     * Attendance engine
     * Hash chain
     * Email reports
     */

    while (1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}