#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize HTTP web server.
 *
 * In Wi-Fi setup mode:
 *   GET  /
 *   POST /save
 *
 * Returns:
 *   ESP_OK      - server started
 *   error code  - server failed
 */
esp_err_t web_server_start(void);

/**
 * Stop the HTTP web server.
 */
void web_server_stop(void);

/**
 * Check whether the web server is running.
 */
bool web_server_is_running(void);

#endif