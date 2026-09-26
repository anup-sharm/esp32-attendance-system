#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/ip4_addr.h"

#include "web_server.h"


#define TAG "WIFI_MANAGER"


/* =========================================================
 * Wi-Fi Setup AP
 * ========================================================= */

#define SETUP_AP_SSID       "Attendance-Setup"
#define SETUP_AP_PASSWORD   "12345678"
#define SETUP_AP_CHANNEL    1
#define SETUP_AP_MAX_CONN   4


/* =========================================================
 * NVS
 * ========================================================= */

#define WIFI_NVS_NAMESPACE  "wifi_config"
#define WIFI_NVS_SSID_KEY   "ssid"
#define WIFI_NVS_PASS_KEY   "password"


/* =========================================================
 * Connection Settings
 * ========================================================= */

#define WIFI_MAX_RETRY              12
#define WIFI_CONNECT_TIMEOUT_MS     30000

/* Number of APs to display during diagnostic scan */
#define WIFI_SCAN_MAX_APS           30


/* =========================================================
 * Event Bits
 * ========================================================= */

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1


/* =========================================================
 * Internal State
 * ========================================================= */

static EventGroupHandle_t wifi_event_group = NULL;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static bool wifi_initialized = false;
static bool wifi_connected = false;
static bool setup_mode = false;

static int retry_count = 0;

static char connected_ip[16] = "0.0.0.0";


/* =========================================================
 * Forward Declarations
 * ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);

static esp_err_t load_wifi_credentials(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
);

static void clear_connection_state(void);

static esp_err_t wifi_scan_networks(void);


/* =========================================================
 * Convert Authentication Mode To Text
 * ========================================================= */

static const char *wifi_auth_mode_to_string(
    wifi_auth_mode_t authmode
)
{
    switch (authmode) {

        case WIFI_AUTH_OPEN:
            return "OPEN";

        case WIFI_AUTH_WEP:
            return "WEP";

        case WIFI_AUTH_WPA_PSK:
            return "WPA";

        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";

        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";

        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "WPA2-ENTERPRISE";

        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";

        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";

        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";

        default:
            return "UNKNOWN";
    }
}


/* =========================================================
 * Wi-Fi Diagnostic Scan
 *
 * This scans all available Wi-Fi networks and prints:
 *
 * - SSID
 * - RSSI
 * - Channel
 * - Authentication
 *
 * Most importantly, it tells us whether the ESP32-S3
 * can actually SEE the phone hotspot "Esp8266".
 * ========================================================= */

static esp_err_t wifi_scan_networks(void)
{
    if (!wifi_initialized) {

        ESP_LOGE(
            TAG,
            "Cannot scan: Wi-Fi manager is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "STARTING WI-FI DIAGNOSTIC SCAN"
    );

    ESP_LOGI(
        TAG,
        "Looking for nearby 2.4 GHz networks..."
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    /* -----------------------------------------------------
     * Scan Configuration
     *
     * channel = 0
     * means scan all available channels.
     *
     * scan_type = ACTIVE
     * means ESP32 actively searches for APs.
     * ----------------------------------------------------- */

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            },
            .passive = 300
        }
    };


    /* -----------------------------------------------------
     * Start Blocking Scan
     *
     * true = wait here until scan completes.
     * ----------------------------------------------------- */

    esp_err_t err = esp_wifi_scan_start(
        &scan_config,
        true
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Wi-Fi scan failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Get Number Of APs
     * ----------------------------------------------------- */

    uint16_t ap_count = 0;

    err = esp_wifi_scan_get_ap_num(
        &ap_count
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to get AP count: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    ESP_LOGI(
        TAG,
        "Wi-Fi networks found: %u",
        ap_count
    );


    if (ap_count == 0) {

        ESP_LOGW(
            TAG,
            "NO WI-FI NETWORKS FOUND!"
        );

        ESP_LOGW(
            TAG,
            "ESP32-S3 cannot currently see any nearby AP."
        );

        return ESP_OK;
    }


    /* -----------------------------------------------------
     * Limit Records
     * ----------------------------------------------------- */

    uint16_t record_count = ap_count;

    if (record_count > WIFI_SCAN_MAX_APS) {
        record_count = WIFI_SCAN_MAX_APS;
    }


    wifi_ap_record_t *records =
        calloc(
            record_count,
            sizeof(wifi_ap_record_t)
        );

    if (records == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to allocate memory for scan results"
        );

        /*
         * Still try to clear driver's scan records.
         */
        wifi_ap_record_t dummy[1];

        uint16_t dummy_count = 1;

        esp_wifi_scan_get_ap_records(
            &dummy_count,
            dummy
        );

        return ESP_ERR_NO_MEM;
    }


    /* -----------------------------------------------------
     * Get AP Records
     * ----------------------------------------------------- */

    uint16_t actual_count = record_count;

    err = esp_wifi_scan_get_ap_records(
        &actual_count,
        records
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to get Wi-Fi scan records: %s",
            esp_err_to_name(err)
        );

        free(records);

        return err;
    }


    /* -----------------------------------------------------
     * Print Results
     * ----------------------------------------------------- */

    bool target_found = false;


    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "WI-FI SCAN RESULTS"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    for (uint16_t i = 0; i < actual_count; i++) {

        const char *ssid =
            (const char *)records[i].ssid;


        ESP_LOGI(
            TAG,
            "[%02u] SSID: %s | RSSI: %d | CH: %d | AUTH: %s",
            i + 1,
            strlen(ssid) > 0 ? ssid : "<HIDDEN>",
            records[i].rssi,
            records[i].primary,
            wifi_auth_mode_to_string(
                records[i].authmode
            )
        );


        /* -------------------------------------------------
         * Check target SSID
         * ------------------------------------------------- */

        if (
            strlen(ssid) > 0 &&
            strcmp(ssid, "Esp8266") == 0
        ) {

            target_found = true;

            ESP_LOGI(
                TAG,
                ">>> TARGET FOUND: Esp8266 <<<"
            );

            ESP_LOGI(
                TAG,
                ">>> RSSI: %d dBm",
                records[i].rssi
            );

            ESP_LOGI(
                TAG,
                ">>> CHANNEL: %d",
                records[i].primary
            );

            ESP_LOGI(
                TAG,
                ">>> AUTH: %s",
                wifi_auth_mode_to_string(
                    records[i].authmode
                )
            );
        }
    }


    ESP_LOGI(
        TAG,
        "======================================"
    );


    if (target_found) {

        ESP_LOGI(
            TAG,
            "RESULT: Esp8266 WAS FOUND"
        );

        ESP_LOGI(
            TAG,
            "The ESP32-S3 can see the phone hotspot."
        );

    } else {

        ESP_LOGW(
            TAG,
            "RESULT: Esp8266 NOT FOUND"
        );

        ESP_LOGW(
            TAG,
            "The ESP32-S3 cannot see the target hotspot."
        );
    }


    ESP_LOGI(
        TAG,
        "======================================"
    );


    free(records);

    return ESP_OK;
}


/* =========================================================
 * Load Wi-Fi Credentials From NVS
 * ========================================================= */

static esp_err_t load_wifi_credentials(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }


    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open(
        WIFI_NVS_NAMESPACE,
        NVS_READONLY,
        &nvs_handle
    );

    if (err != ESP_OK) {

        ESP_LOGI(
            TAG,
            "No Wi-Fi configuration found in NVS"
        );

        return err;
    }


    /* -----------------------------------------------------
     * Read SSID
     * ----------------------------------------------------- */

    size_t required_ssid_size = ssid_size;

    err = nvs_get_str(
        nvs_handle,
        WIFI_NVS_SSID_KEY,
        ssid,
        &required_ssid_size
    );

    if (err != ESP_OK) {

        nvs_close(nvs_handle);

        ESP_LOGI(
            TAG,
            "No saved SSID found"
        );

        return err;
    }


    /* -----------------------------------------------------
     * Read Password
     * ----------------------------------------------------- */

    size_t required_password_size = password_size;

    err = nvs_get_str(
        nvs_handle,
        WIFI_NVS_PASS_KEY,
        password,
        &required_password_size
    );

    nvs_close(nvs_handle);

    if (err != ESP_OK) {

        ESP_LOGI(
            TAG,
            "No saved Wi-Fi password found"
        );

        return err;
    }


    /* -----------------------------------------------------
     * Validate SSID
     * ----------------------------------------------------- */

    if (strlen(ssid) == 0) {

        ESP_LOGW(
            TAG,
            "Saved SSID is empty"
        );

        return ESP_ERR_NOT_FOUND;
    }


    return ESP_OK;
}


/* =========================================================
 * Clear Connection State
 * ========================================================= */

static void clear_connection_state(void)
{
    wifi_connected = false;

    strncpy(
        connected_ip,
        "0.0.0.0",
        sizeof(connected_ip)
    );

    connected_ip[
        sizeof(connected_ip) - 1
    ] = '\0';
}


/* =========================================================
 * Wi-Fi Event Handler
 * ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;


    /* =====================================================
     * STA START
     * ===================================================== */

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    ) {

        ESP_LOGI(
            TAG,
            "STA interface started"
        );

        return;
    }


    /* =====================================================
     * STA DISCONNECTED
     * ===================================================== */

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {

        wifi_connected = false;

        strncpy(
            connected_ip,
            "0.0.0.0",
            sizeof(connected_ip)
        );

        connected_ip[
            sizeof(connected_ip) - 1
        ] = '\0';


        if (event_data != NULL) {

            wifi_event_sta_disconnected_t *event =
                (wifi_event_sta_disconnected_t *)event_data;

            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected. Reason code: %d",
                event->reason
            );
        }


        /* -------------------------------------------------
         * Retry only in normal STA mode
         * ------------------------------------------------- */

        if (!setup_mode) {

            if (retry_count < WIFI_MAX_RETRY) {

                retry_count++;

                ESP_LOGW(
                    TAG,
                    "Wi-Fi connection failed. Retry %d/%d",
                    retry_count,
                    WIFI_MAX_RETRY
                );

                esp_err_t err =
                    esp_wifi_connect();

                if (err != ESP_OK) {

                    ESP_LOGW(
                        TAG,
                        "esp_wifi_connect() failed: %s",
                        esp_err_to_name(err)
                    );
                }

            } else {

                ESP_LOGE(
                    TAG,
                    "Maximum Wi-Fi retry count reached"
                );

                xEventGroupSetBits(
                    wifi_event_group,
                    WIFI_FAIL_BIT
                );
            }
        }

        return;
    }


    /* =====================================================
     * GOT IP ADDRESS
     * ===================================================== */

    if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;


        retry_count = 0;

        wifi_connected = true;


        snprintf(
            connected_ip,
            sizeof(connected_ip),
            IPSTR,
            IP2STR(&event->ip_info.ip)
        );


        ESP_LOGI(
            TAG,
            "======================================"
        );

        ESP_LOGI(
            TAG,
            "Wi-Fi connected successfully"
        );

        ESP_LOGI(
            TAG,
            "IP address: %s",
            connected_ip
        );

        ESP_LOGI(
            TAG,
            "======================================"
        );


        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        return;
    }
}


/* =========================================================
 * Initialize Wi-Fi Manager
 *
 * IMPORTANT:
 *
 * esp_netif_init()
 * esp_event_loop_create_default()
 *
 * are initialized by main.c.
 *
 * They are NOT called here again.
 * ========================================================= */

esp_err_t wifi_manager_init(void)
{
    if (wifi_initialized) {

        ESP_LOGW(
            TAG,
            "Wi-Fi manager already initialized"
        );

        return ESP_OK;
    }


    ESP_LOGI(
        TAG,
        "Initializing Wi-Fi manager..."
    );


    /* -----------------------------------------------------
     * Create Event Group
     * ----------------------------------------------------- */

    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create Wi-Fi event group"
        );

        return ESP_ERR_NO_MEM;
    }


    /* -----------------------------------------------------
     * Create Default STA Network Interface
     * ----------------------------------------------------- */

    sta_netif =
        esp_netif_create_default_wifi_sta();

    if (sta_netif == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create STA network interface"
        );

        return ESP_FAIL;
    }


    /* -----------------------------------------------------
     * Create Default AP Network Interface
     * ----------------------------------------------------- */

    ap_netif =
        esp_netif_create_default_wifi_ap();

    if (ap_netif == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create AP network interface"
        );

        return ESP_FAIL;
    }


    /* -----------------------------------------------------
     * Initialize Wi-Fi Driver
     * ----------------------------------------------------- */

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t err =
        esp_wifi_init(&cfg);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "esp_wifi_init failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Register Wi-Fi Event Handler
     * ----------------------------------------------------- */

    err = esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to register Wi-Fi event handler: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Register IP Event Handler
     * ----------------------------------------------------- */

    err = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to register IP event handler: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    wifi_initialized = true;


    ESP_LOGI(
        TAG,
        "Wi-Fi manager initialized"
    );


    return ESP_OK;
}


/* =========================================================
 * Start Setup Access Point
 * ========================================================= */

esp_err_t wifi_manager_start_setup_mode(void)
{
    if (!wifi_initialized) {

        ESP_LOGE(
            TAG,
            "Wi-Fi manager is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(
        TAG,
        "Starting Wi-Fi Setup AP..."
    );


    setup_mode = true;

    clear_connection_state();


    /* -----------------------------------------------------
     * Clear Event Bits
     * ----------------------------------------------------- */

    xEventGroupClearBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT |
        WIFI_FAIL_BIT
    );


    /* -----------------------------------------------------
     * AP Configuration
     * ----------------------------------------------------- */

    wifi_config_t ap_config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = SETUP_AP_CHANNEL,
            .password = SETUP_AP_PASSWORD,
            .max_connection = SETUP_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false
            }
        }
    };


    /* -----------------------------------------------------
     * Set AP Mode
     * ----------------------------------------------------- */

    esp_err_t err =
        esp_wifi_set_mode(WIFI_MODE_AP);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set AP mode: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Configure AP
     * ----------------------------------------------------- */

    err = esp_wifi_set_config(
        WIFI_IF_AP,
        &ap_config
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to configure AP: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Start Wi-Fi
     * ----------------------------------------------------- */

    err = esp_wifi_start();

    if (
        err != ESP_OK &&
        err != ESP_ERR_WIFI_CONN
    ) {

        ESP_LOGE(
            TAG,
            "Failed to start Wi-Fi AP: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Setup Mode Information
     * ----------------------------------------------------- */

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


    /* -----------------------------------------------------
     * Start Web Server
     * ----------------------------------------------------- */

    if (!web_server_is_running()) {

        err = web_server_start();

        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to start web server: %s",
                esp_err_to_name(err)
            );

            return err;
        }
    }


    return ESP_OK;
}


/* =========================================================
 * Connect To Saved Wi-Fi
 * ========================================================= */

esp_err_t wifi_manager_connect_saved(void)
{
    if (!wifi_initialized) {

        ESP_LOGE(
            TAG,
            "Wi-Fi manager is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    char ssid[33] = {0};
    char password[65] = {0};


    /* -----------------------------------------------------
     * Load credentials
     * ----------------------------------------------------- */

    esp_err_t err =
        load_wifi_credentials(
            ssid,
            sizeof(ssid),
            password,
            sizeof(password)
        );

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "No valid saved Wi-Fi configuration"
        );

        return ESP_ERR_NOT_FOUND;
    }


    ESP_LOGI(
        TAG,
        "Saved Wi-Fi configuration found"
    );

    ESP_LOGI(
        TAG,
        "Connecting to saved Wi-Fi..."
    );

    ESP_LOGI(
        TAG,
        "SSID: %s",
        ssid
    );


    setup_mode = false;

    clear_connection_state();

    retry_count = 0;


    /* -----------------------------------------------------
     * Clear previous event bits
     * ----------------------------------------------------- */

    xEventGroupClearBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT |
        WIFI_FAIL_BIT
    );


    /* -----------------------------------------------------
     * Prepare STA Configuration
     * ----------------------------------------------------- */

    wifi_config_t wifi_config = {0};


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


    /*
     * WPA2 is the expected security mode for
     * the current phone hotspot test.
     */

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;


    /* -----------------------------------------------------
     * Set STA Mode
     * ----------------------------------------------------- */

    err = esp_wifi_set_mode(
        WIFI_MODE_STA
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set STA mode: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Configure STA
     * ----------------------------------------------------- */

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to configure STA: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Disable Wi-Fi power save temporarily
     *
     * This helps make the diagnostic test more reliable.
     * ----------------------------------------------------- */

    err = esp_wifi_set_ps(WIFI_PS_NONE);

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Could not disable Wi-Fi power save: %s",
            esp_err_to_name(err)
        );
    }


    /* -----------------------------------------------------
     * Start Wi-Fi
     * ----------------------------------------------------- */

    err = esp_wifi_start();

    if (
        err != ESP_OK &&
        err != ESP_ERR_WIFI_CONN
    ) {

        ESP_LOGE(
            TAG,
            "Failed to start STA: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * IMPORTANT DIAGNOSTIC STEP
     *
     * Scan BEFORE esp_wifi_connect().
     *
     * This tells us whether ESP32-S3 can actually see
     * the target hotspot.
     * ----------------------------------------------------- */

    err = wifi_scan_networks();

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Diagnostic Wi-Fi scan returned: %s",
            esp_err_to_name(err)
        );
    }


    /* -----------------------------------------------------
     * Explicitly Start Connection
     * ----------------------------------------------------- */

    ESP_LOGI(
        TAG,
        "Attempting connection to: %s",
        ssid
    );


    err = esp_wifi_connect();

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "esp_wifi_connect failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /* -----------------------------------------------------
     * Wait For Connection
     * ----------------------------------------------------- */

    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT |
            WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(
                WIFI_CONNECT_TIMEOUT_MS
            )
        );


    /* -----------------------------------------------------
     * Connected
     * ----------------------------------------------------- */

    if (bits & WIFI_CONNECTED_BIT) {

        ESP_LOGI(
            TAG,
            "Saved Wi-Fi connection successful"
        );

        return ESP_OK;
    }


    /* -----------------------------------------------------
     * Failed
     * ----------------------------------------------------- */

    if (bits & WIFI_FAIL_BIT) {

        ESP_LOGW(
            TAG,
            "Saved Wi-Fi connection failed"
        );

        return ESP_FAIL;
    }


    /* -----------------------------------------------------
     * Timeout
     * ----------------------------------------------------- */

    ESP_LOGW(
        TAG,
        "Wi-Fi connection timeout"
    );

    return ESP_ERR_TIMEOUT;
}


/* =========================================================
 * Start Wi-Fi Manager
 * ========================================================= */

esp_err_t wifi_manager_start(void)
{
    if (!wifi_initialized) {

        esp_err_t err =
            wifi_manager_init();

        if (err != ESP_OK) {
            return err;
        }
    }


    /*
     * Do NOT load credentials here.
     *
     * wifi_manager_connect_saved()
     * already handles loading.
     */

    esp_err_t err =
        wifi_manager_connect_saved();


    /* -----------------------------------------------------
     * Normal Wi-Fi connected
     * ----------------------------------------------------- */

    if (err == ESP_OK) {

        ESP_LOGI(
            TAG,
            "Normal Wi-Fi mode active"
        );

        return ESP_OK;
    }


    /* -----------------------------------------------------
     * No credentials / connection failed
     * ----------------------------------------------------- */

    ESP_LOGW(
        TAG,
        "Falling back to SETUP MODE"
    );


    return wifi_manager_start_setup_mode();
}


/* =========================================================
 * Check Connected Status
 * ========================================================= */

bool wifi_manager_is_connected(void)
{
    return wifi_connected;
}


/* =========================================================
 * Check Setup Mode
 * ========================================================= */

bool wifi_manager_is_setup_mode(void)
{
    return setup_mode;
}


/* =========================================================
 * Get Current IP Address
 * ========================================================= */

const char *wifi_manager_get_ip(void)
{
    return connected_ip;
}