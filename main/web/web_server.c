#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "web_server.h"

#define TAG "WEB_SERVER"

/* =========================================================
 * WIFI NVS SETTINGS
 * ========================================================= */

#define WIFI_NAMESPACE      "wifi_config"
#define WIFI_SSID_KEY       "ssid"
#define WIFI_PASSWORD_KEY   "password"

/* =========================================================
 * HTTP SERVER
 * ========================================================= */

static httpd_handle_t server = NULL;

/* =========================================================
 * REGISTRATION DATA
 * ========================================================= */

#define REGISTRATION_ID_MAX_LEN      31
#define REGISTRATION_NAME_MAX_LEN    95

static char registration_employee_id[
    REGISTRATION_ID_MAX_LEN + 1
];

static char registration_employee_name[
    REGISTRATION_NAME_MAX_LEN + 1
];

static volatile bool registration_request = false;

static volatile bool registration_active = false;

static char registration_status[64] = "READY";

static int registration_face_id = -1;

/* =========================================================
 * SETUP HTML
 * ========================================================= */

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
 * ADMIN HTML
 * ========================================================= */

static const char *admin_html =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Attendance Admin</title>"
"<style>"
"body{font-family:Arial;background:#f2f2f2;margin:0;padding:20px;}"
".box{max-width:450px;margin:auto;background:white;padding:25px;"
"border-radius:15px;box-shadow:0 4px 15px rgba(0,0,0,.15);}"
"h1{text-align:center;color:#222;margin-bottom:8px;}"
".sub{text-align:center;color:#666;margin-bottom:20px;}"
"label{display:block;margin-top:16px;font-weight:bold;}"
"input{width:100%;box-sizing:border-box;padding:12px;margin-top:7px;"
"border:1px solid #ccc;border-radius:8px;font-size:16px;}"
"button{width:100%;padding:14px;margin-top:22px;border:0;"
"border-radius:8px;background:#1677ff;color:white;font-size:17px;"
"font-weight:bold;}"
".status{margin-top:20px;padding:14px;border-radius:8px;"
"background:#eef5ff;font-size:15px;}"
".success{background:#e8f7e8;}"
".warning{background:#fff4d6;}"
"</style>"
"</head>"
"<body>"
"<div class='box'>"
"<h1>ATTENDANCE ADMIN</h1>"
"<div class='sub'>Employee Registration</div>"

"<label>Employee ID</label>"
"<input id='employee_id' type='text' maxlength='31' "
"placeholder='Example: D01'>"

"<label>Employee Name</label>"
"<input id='employee_name' type='text' maxlength='95' "
"placeholder='Example: Rahul Kumar'>"

"<button onclick='registerEmployee()'>"
"REGISTER EMPLOYEE"
"</button>"

"<div id='status' class='status'>"
"Status: READY"
"</div>"

"</div>"

"<script>"

"function updateStatus(){"
"fetch('/status')"
".then(r=>r.json())"
".then(d=>{"
"let s=document.getElementById('status');"
"s.innerHTML='Status: '+d.status;"
"if(d.status.indexOf('SUCCESS')>=0){"
"s.className='status success';"
"}else if(d.status.indexOf('WAIT')>=0 || "
"d.status.indexOf('DETECT')>=0 || "
"d.status.indexOf('CAPTUR')>=0){"
"s.className='status warning';"
"}else{"
"s.className='status';"
"}"
"})"
".catch(()=>{});"
"}"

"function registerEmployee(){"

"let id=document.getElementById('employee_id').value.trim();"
"let name=document.getElementById('employee_name').value.trim();"

"if(id.length===0){"
"alert('Please enter Employee ID');"
"return;"
"}"

"if(name.length===0){"
"alert('Please enter Employee Name');"
"return;"
"}"

"let body='employee_id='+encodeURIComponent(id)+"
"'&employee_name='+encodeURIComponent(name);"

"fetch('/register',{"
"method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:body"
"})"
".then(r=>r.text())"
".then(t=>{"
"document.getElementById('status').innerHTML=t;"
"updateStatus();"
"})"
".catch(()=>{"
"document.getElementById('status').innerHTML="
"'Registration request failed';"
"});"

"}"

"setInterval(updateStatus,1000);"
"updateStatus();"

"</script>"

"</body>"
"</html>";

/* =========================================================
 * HEX CHARACTER TO INTEGER
 * ========================================================= */

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

/* =========================================================
 * URL DECODE
 * ========================================================= */

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
        else if (
            *src == '%' &&
            src[1] &&
            src[2]
        )
        {
            int high = hex_to_int(src[1]);
            int low = hex_to_int(src[2]);

            if (high >= 0 && low >= 0)
            {
                *dst++ =
                    (char)((high << 4) | low);

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
 * SAVE WIFI CREDENTIALS
 * ========================================================= */

static esp_err_t save_wifi_credentials(
    const char *ssid,
    const char *password
)
{
    nvs_handle_t nvs_handle;

    esp_err_t err =
        nvs_open(
            WIFI_NAMESPACE,
            NVS_READWRITE,
            &nvs_handle
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "NVS open failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        nvs_set_str(
            nvs_handle,
            WIFI_SSID_KEY,
            ssid
        );

    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    err =
        nvs_set_str(
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
        ESP_LOGI(
            TAG,
            "Wi-Fi credentials saved successfully"
        );
    }

    return err;
}

/* =========================================================
 * HTTP GET /
 * ========================================================= */

static esp_err_t root_get_handler(
    httpd_req_t *req
)
{
    httpd_resp_set_type(
        req,
        "text/html"
    );

    httpd_resp_send(
        req,
        admin_html,
        HTTPD_RESP_USE_STRLEN
    );

    return ESP_OK;
}

/* =========================================================
 * HTTP POST /save
 * ========================================================= */

static esp_err_t save_post_handler(
    httpd_req_t *req
)
{
    char content[512];

    int total_len =
        req->content_len;

    if (
        total_len <= 0 ||
        total_len >= (int)sizeof(content)
    )
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid data size"
        );

        return ESP_FAIL;
    }

    int received = 0;

    while (received < total_len)
    {
        int ret =
            httpd_req_recv(
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

    if (
        !ssid_start ||
        !password_start
    )
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
    {
        ssid_len =
            sizeof(ssid) - 1;
    }

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
 * HTTP POST /register
 * ========================================================= */

static esp_err_t register_post_handler(
    httpd_req_t *req
)
{
    char content[256];

    int total_len =
        req->content_len;

    if (
        total_len <= 0 ||
        total_len >= (int)sizeof(content)
    )
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid registration data"
        );

        return ESP_FAIL;
    }

    int received = 0;

    while (received < total_len)
    {
        int ret =
            httpd_req_recv(
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

    char employee_id[
        REGISTRATION_ID_MAX_LEN + 1
    ] = {0};

    char employee_name[
        REGISTRATION_NAME_MAX_LEN + 1
    ] = {0};

    char *id_start =
        strstr(
            content,
            "employee_id="
        );

    char *name_start =
        strstr(
            content,
            "&employee_name="
        );

    if (
        !id_start ||
        !name_start
    )
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid employee data"
        );

        return ESP_FAIL;
    }

    id_start +=
        strlen("employee_id=");

    size_t id_len =
        name_start - id_start;

    if (
        id_len >=
        sizeof(employee_id)
    )
    {
        id_len =
            sizeof(employee_id) - 1;
    }

    memcpy(
        employee_id,
        id_start,
        id_len
    );

    employee_id[id_len] = '\0';

    name_start +=
        strlen("&employee_name=");

    strncpy(
        employee_name,
        name_start,
        sizeof(employee_name) - 1
    );

    url_decode(employee_id);
    url_decode(employee_name);

    if (
        strlen(employee_id) == 0 ||
        strlen(employee_name) == 0
    )
    {
        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Employee ID and Name are required"
        );

        return ESP_FAIL;
    }

    if (registration_active)
    {
        httpd_resp_set_type(
            req,
            "text/plain"
        );

        httpd_resp_send(
            req,
            "Registration already in progress",
            HTTPD_RESP_USE_STRLEN
        );

        return ESP_OK;
    }

    memset(
        registration_employee_id,
        0,
        sizeof(registration_employee_id)
    );

    memset(
        registration_employee_name,
        0,
        sizeof(registration_employee_name)
    );

    strncpy(
        registration_employee_id,
        employee_id,
        sizeof(registration_employee_id) - 1
    );

    strncpy(
        registration_employee_name,
        employee_name,
        sizeof(registration_employee_name) - 1
    );

    registration_face_id = -1;

    strncpy(
        registration_status,
        "WAITING FOR CAMERA",
        sizeof(registration_status) - 1
    );

    registration_request = true;
    registration_active = true;

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "NEW EMPLOYEE REGISTRATION REQUEST"
    );

    ESP_LOGI(
        TAG,
        "Employee ID   : %s",
        registration_employee_id
    );

    ESP_LOGI(
        TAG,
        "Employee Name : %s",
        registration_employee_name
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    httpd_resp_set_type(
        req,
        "text/plain"
    );

    httpd_resp_send(
        req,
        "Registration started. Look at the camera.",
        HTTPD_RESP_USE_STRLEN
    );

    return ESP_OK;
}

/* =========================================================
 * HTTP GET /status
 * ========================================================= */

static esp_err_t status_get_handler(
    httpd_req_t *req
)
{
    char response[256];

    snprintf(
        response,
        sizeof(response),
        "{"
        "\"status\":\"%s\","
        "\"face_id\":%d"
        "}",
        registration_status,
        registration_face_id
    );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );

    return ESP_OK;
}

/* =========================================================
 * START WEB SERVER
 * ========================================================= */

esp_err_t web_server_start(void)
{
    if (server != NULL)
    {
        ESP_LOGW(
            TAG,
            "Web server already running"
        );

        return ESP_OK;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;

    esp_err_t err =
        httpd_start(
            &server,
            &config
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP server start failed: %s",
            esp_err_to_name(err)
        );

        server = NULL;

        return err;
    }

    /* =====================================================
     * GET /
     * ===================================================== */

    httpd_uri_t root_uri =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };

    err =
        httpd_register_uri_handler(
            server,
            &root_uri
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register GET /"
        );

        httpd_stop(server);
        server = NULL;

        return err;
    }

    /* =====================================================
     * POST /save
     * ===================================================== */

    httpd_uri_t save_uri =
    {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL
    };

    err =
        httpd_register_uri_handler(
            server,
            &save_uri
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register POST /save"
        );

        httpd_stop(server);
        server = NULL;

        return err;
    }

    /* =====================================================
     * POST /register
     * ===================================================== */

    httpd_uri_t register_uri =
    {
        .uri = "/register",
        .method = HTTP_POST,
        .handler = register_post_handler,
        .user_ctx = NULL
    };

    err =
        httpd_register_uri_handler(
            server,
            &register_uri
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register POST /register"
        );

        httpd_stop(server);
        server = NULL;

        return err;
    }

    /* =====================================================
     * GET /status
     * ===================================================== */

    httpd_uri_t status_uri =
    {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };

    err =
        httpd_register_uri_handler(
            server,
            &status_uri
        );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register GET /status"
        );

        httpd_stop(server);
        server = NULL;

        return err;
    }

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "LOCAL ADMIN WEB SERVER STARTED"
    );

    ESP_LOGI(
        TAG,
        "HTTP port: 80"
    );

    ESP_LOGI(
        TAG,
        "GET  /        -> Admin panel"
    );

    ESP_LOGI(
        TAG,
        "POST /register -> Employee registration"
    );

    ESP_LOGI(
        TAG,
        "GET  /status  -> Registration status"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    return ESP_OK;
}

/* =========================================================
 * STOP WEB SERVER
 * ========================================================= */

void web_server_stop(void)
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
 * SERVER STATUS
 * ========================================================= */

bool web_server_is_running(void)
{
    return (server != NULL);
}

/* =========================================================
 * REGISTRATION REQUEST
 * ========================================================= */

bool web_server_registration_requested(void)
{
    return registration_request;
}

/* =========================================================
 * GET REGISTRATION REQUEST
 * ========================================================= */

esp_err_t web_server_get_registration_request(
    char *employee_id,
    size_t employee_id_size,
    char *employee_name,
    size_t employee_name_size
)
{
    if (
        employee_id == NULL ||
        employee_name == NULL ||
        employee_id_size == 0 ||
        employee_name_size == 0
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!registration_request)
    {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(
        employee_id,
        registration_employee_id,
        employee_id_size - 1
    );

    employee_id[
        employee_id_size - 1
    ] = '\0';

    strncpy(
        employee_name,
        registration_employee_name,
        employee_name_size - 1
    );

    employee_name[
        employee_name_size - 1
    ] = '\0';

    return ESP_OK;
}

/* =========================================================
 * CLEAR REGISTRATION REQUEST
 * ========================================================= */

void web_server_clear_registration_request(void)
{
    registration_request = false;
}

/* =========================================================
 * REGISTRATION STATUS
 * ========================================================= */

void web_server_set_registration_status(
    const char *status,
    int face_id
)
{
    if (status == NULL)
    {
        return;
    }

    strncpy(
        registration_status,
        status,
        sizeof(registration_status) - 1
    );

    registration_status[
        sizeof(registration_status) - 1
    ] = '\0';

    registration_face_id = face_id;

    ESP_LOGI(
        TAG,
        "Registration status: %s | Face ID: %d",
        registration_status,
        registration_face_id
    );

    if (
        strcmp(
            registration_status,
            "SUCCESS"
        ) == 0
    )
    {
        registration_active = false;
    }
}

/* =========================================================
 * GET REGISTRATION STATUS
 * ========================================================= */

const char *web_server_get_registration_status(void)
{
    return registration_status;
}

/* =========================================================
 * REGISTRATION ACTIVE
 * ========================================================= */

bool web_server_is_registration_active(void)
{
    return registration_active;
}