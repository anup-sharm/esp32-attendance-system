#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Initialize Wi-Fi driver, STA/AP network interfaces
 * and Wi-Fi event handlers.
 *
 * Must be called only once.
 */
esp_err_t wifi_manager_init(void);


/**
 * Start the Wi-Fi manager.
 *
 * Behaviour:
 *
 * 1. If saved Wi-Fi credentials exist:
 *      -> Try connecting to saved Wi-Fi.
 *
 * 2. If connection succeeds:
 *      -> Normal Wi-Fi mode.
 *
 * 3. If credentials do not exist OR connection fails:
 *      -> Start Attendance-Setup AP.
 */
esp_err_t wifi_manager_start(void);


/**
 * Start Wi-Fi Setup Access Point.
 *
 * SSID:
 *     Attendance-Setup
 *
 * Password:
 *     12345678
 *
 * IP:
 *     192.168.4.1
 */
esp_err_t wifi_manager_start_setup_mode(void);


/**
 * Connect to Wi-Fi credentials stored in NVS.
 *
 * Returns:
 *     ESP_OK       -> Connected successfully
 *     ESP_ERR_NOT_FOUND -> No saved credentials
 *     ESP_ERR_TIMEOUT   -> Connection timeout
 *     ESP_FAIL          -> Connection failed
 */
esp_err_t wifi_manager_connect_saved(void);


/**
 * Returns true when ESP32 has received an IP
 * address from the Wi-Fi network.
 */
bool wifi_manager_is_connected(void);


/**
 * Returns true when Attendance-Setup AP is active.
 */
bool wifi_manager_is_setup_mode(void);


/**
 * Returns the current STA IP address.
 *
 * Returns:
 *     "0.0.0.0"
 *
 * when not connected.
 */
const char *wifi_manager_get_ip(void);


#ifdef __cplusplus
}
#endif

#endif