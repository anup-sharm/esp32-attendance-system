#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * Start local HTTP web server.
 *
 * Setup mode:
 *   GET  /
 *   POST /save
 *
 * Attendance/Admin mode:
 *   GET  /
 *   POST /register
 *   GET  /status
 */
esp_err_t web_server_start(void);

/**
 * Stop the HTTP web server.
 */
void web_server_stop(void);

/**
 * Check whether web server is running.
 */
bool web_server_is_running(void);

/**
 * Check whether an employee registration request
 * has been submitted from the phone.
 *
 * Returns true only when a new request is waiting.
 */
bool web_server_registration_requested(void);

/**
 * Get the requested Employee ID and Employee Name.
 *
 * The returned strings remain valid until the next
 * registration request is received.
 */
esp_err_t web_server_get_registration_request(
    char *employee_id,
    size_t employee_id_size,
    char *employee_name,
    size_t employee_name_size
);

/**
 * Clear the current registration request.
 *
 * Called by main.c after the request has been accepted
 * and enrollment has started.
 */
void web_server_clear_registration_request(void);

/**
 * Update registration state shown on phone.
 */
void web_server_set_registration_status(
    const char *status,
    int face_id
);

/**
 * Get current registration status.
 */
const char *web_server_get_registration_status(void);

/**
 * Check whether the system is currently in
 * phone-controlled registration mode.
 */
bool web_server_is_registration_active(void);

#endif