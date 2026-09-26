#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * DS3231 CONFIGURATION
 * ============================================================ */

#define DS3231_I2C_PORT        I2C_NUM_0

#define DS3231_SDA_IO          3
#define DS3231_SCL_IO          2

#define DS3231_I2C_FREQ_HZ     100000

#define DS3231_I2C_ADDRESS     0x68


/* ============================================================
 * RTC DATE/TIME STRUCTURE
 * ============================================================ */

typedef struct
{
    uint16_t year;

    uint8_t month;
    uint8_t day;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;

    uint8_t weekday;

} ds3231_datetime_t;


/* ============================================================
 * INITIALIZATION
 * ============================================================ */

/**
 * @brief Initialize DS3231 I2C bus and RTC device.
 *
 * @return
 *      ESP_OK on success
 *      ESP_ERR_* on failure
 */
esp_err_t ds3231_init(void);


/* ============================================================
 * DATE/TIME READ
 * ============================================================ */

/**
 * @brief Read current date and time from DS3231.
 *
 * @param datetime Pointer to datetime structure.
 *
 * @return
 *      ESP_OK on success
 *      ESP_ERR_INVALID_ARG if datetime is NULL
 *      ESP_ERR_* on I2C failure
 */
esp_err_t ds3231_get_datetime(ds3231_datetime_t *datetime);


/* ============================================================
 * DATE/TIME WRITE
 * ============================================================ */

/**
 * @brief Set date and time in DS3231.
 *
 * @param datetime Date/time to write.
 *
 * @return
 *      ESP_OK on success
 *      ESP_ERR_INVALID_ARG for invalid date/time
 *      ESP_ERR_* on I2C failure
 */
esp_err_t ds3231_set_datetime(
    const ds3231_datetime_t *datetime
);


/* ============================================================
 * RTC STATUS
 * ============================================================ */

/**
 * @brief Check whether DS3231 oscillator is stopped.
 *
 * @param stopped Pointer to boolean result.
 *
 * @return ESP_OK on success.
 */
esp_err_t ds3231_is_oscillator_stopped(bool *stopped);


/**
 * @brief Clear DS3231 oscillator-stop flag.
 *
 * @return ESP_OK on success.
 */
esp_err_t ds3231_clear_oscillator_stop_flag(void);


/* ============================================================
 * TEMPERATURE
 * ============================================================ */

/**
 * @brief Read DS3231 internal temperature sensor.
 *
 * @param temperature Pointer to float.
 *
 * @return ESP_OK on success.
 */
esp_err_t ds3231_get_temperature(float *temperature);


/* ============================================================
 * UTILITY
 * ============================================================ */

/**
 * @brief Print current RTC date/time to ESP-IDF log.
 */
void ds3231_print_datetime(
    const ds3231_datetime_t *datetime
);

#ifdef __cplusplus
}
#endif

#endif