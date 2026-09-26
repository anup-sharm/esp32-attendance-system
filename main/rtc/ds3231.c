#include "ds3231.h"

#include <stdio.h>

#include "driver/i2c_master.h"

#include "esp_log.h"
#include "esp_check.h"


/* ============================================================
 * TAG
 * ============================================================ */

static const char *TAG = "DS3231";


/* ============================================================
 * DS3231 REGISTERS
 * ============================================================ */

#define DS3231_REG_SECONDS       0x00
#define DS3231_REG_MINUTES       0x01
#define DS3231_REG_HOURS         0x02
#define DS3231_REG_WEEKDAY       0x03
#define DS3231_REG_DAY           0x04
#define DS3231_REG_MONTH         0x05
#define DS3231_REG_YEAR          0x06

#define DS3231_REG_CONTROL       0x0E
#define DS3231_REG_STATUS        0x0F

#define DS3231_REG_TEMP_MSB      0x11
#define DS3231_REG_TEMP_LSB      0x12


/* ============================================================
 * STATUS REGISTER BITS
 * ============================================================ */

#define DS3231_STATUS_OSF        (1 << 7)


/* ============================================================
 * CONTROL REGISTER BITS
 * ============================================================ */

#define DS3231_CONTROL_EOSC      (1 << 7)


/* ============================================================
 * INTERNAL I2C HANDLES
 * ============================================================ */

static i2c_master_bus_handle_t s_i2c_bus = NULL;

static i2c_master_dev_handle_t s_ds3231_device = NULL;

static bool s_initialized = false;


/* ============================================================
 * BCD CONVERSION
 * ============================================================ */

static uint8_t bcd_to_decimal(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}


static uint8_t decimal_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}


/* ============================================================
 * DAYS IN MONTH
 * ============================================================ */

static uint8_t days_in_month(
    uint16_t year,
    uint8_t month
)
{
    static const uint8_t days[] =
    {
        31,
        28,
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31
    };

    if (month < 1 || month > 12) {
        return 0;
    }

    if (month == 2) {

        bool leap_year =
            ((year % 4 == 0) &&
             (year % 100 != 0)) ||
            (year % 400 == 0);

        return leap_year ? 29 : 28;
    }

    return days[month - 1];
}


/* ============================================================
 * DATETIME VALIDATION
 * ============================================================ */

static bool validate_datetime(
    const ds3231_datetime_t *datetime
)
{
    if (datetime == NULL) {
        return false;
    }

    if (datetime->year < 2000 ||
        datetime->year > 2099) {
        return false;
    }

    if (datetime->month < 1 ||
        datetime->month > 12) {
        return false;
    }

    uint8_t max_day =
        days_in_month(
            datetime->year,
            datetime->month
        );

    if (datetime->day < 1 ||
        datetime->day > max_day) {
        return false;
    }

    if (datetime->hour > 23) {
        return false;
    }

    if (datetime->minute > 59) {
        return false;
    }

    if (datetime->second > 59) {
        return false;
    }

    if (datetime->weekday < 1 ||
        datetime->weekday > 7) {
        return false;
    }

    return true;
}


/* ============================================================
 * REGISTER WRITE
 * ============================================================ */

static esp_err_t ds3231_write_register(
    uint8_t reg,
    uint8_t value
)
{
    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2];

    data[0] = reg;
    data[1] = value;

    return i2c_master_transmit(
        s_ds3231_device,
        data,
        sizeof(data),
        1000
    );
}


/* ============================================================
 * REGISTER READ
 * ============================================================ */

static esp_err_t ds3231_read_register(
    uint8_t reg,
    uint8_t *value
)
{
    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        s_ds3231_device,
        &reg,
        1,
        value,
        1,
        1000
    );
}


/* ============================================================
 * INITIALIZATION
 * ============================================================ */

esp_err_t ds3231_init(void)
{
    if (s_initialized) {

        ESP_LOGI(
            TAG,
            "DS3231 already initialized"
        );

        return ESP_OK;
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "        DS3231 RTC INITIALIZATION"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "I2C Port: I2C_NUM_0"
    );

    ESP_LOGI(
        TAG,
        "SDA: GPIO %d",
        DS3231_SDA_IO
    );

    ESP_LOGI(
        TAG,
        "SCL: GPIO %d",
        DS3231_SCL_IO
    );

    ESP_LOGI(
        TAG,
        "Frequency: %d Hz",
        DS3231_I2C_FREQ_HZ
    );

    ESP_LOGI(
        TAG,
        "DS3231 Address: 0x%02X",
        DS3231_I2C_ADDRESS
    );


    /* ========================================================
     * I2C MASTER BUS
     * ======================================================== */

    i2c_master_bus_config_t bus_config =
    {
        .i2c_port = DS3231_I2C_PORT,

        .sda_io_num = DS3231_SDA_IO,

        .scl_io_num = DS3231_SCL_IO,

        .clk_source = I2C_CLK_SRC_DEFAULT,

        .glitch_ignore_cnt = 7,

        .flags =
        {
            .enable_internal_pullup = true
        }
    };


    esp_err_t ret =
        i2c_new_master_bus(
            &bus_config,
            &s_i2c_bus
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "I2C bus initialization FAILED: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "I2C master bus initialized"
    );


    /* ========================================================
     * ADD DS3231 DEVICE
     * ======================================================== */

    i2c_device_config_t device_config =
    {
        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,

        .device_address =
            DS3231_I2C_ADDRESS,

        .scl_speed_hz =
            DS3231_I2C_FREQ_HZ
    };


    ret =
        i2c_master_bus_add_device(
            s_i2c_bus,
            &device_config,
            &s_ds3231_device
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to add DS3231 device: %s",
            esp_err_to_name(ret)
        );

        i2c_del_master_bus(
            s_i2c_bus
        );

        s_i2c_bus = NULL;

        return ret;
    }


    /* ========================================================
     * PROBE DEVICE
     * ======================================================== */

    ret =
        i2c_master_probe(
            s_i2c_bus,
            DS3231_I2C_ADDRESS,
            1000
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "DS3231 NOT FOUND at address 0x%02X",
            DS3231_I2C_ADDRESS
        );

        ESP_LOGE(
            TAG,
            "Check SDA, SCL, VCC and GND"
        );

        i2c_master_bus_rm_device(
            s_ds3231_device
        );

        s_ds3231_device = NULL;

        i2c_del_master_bus(
            s_i2c_bus
        );

        s_i2c_bus = NULL;

        return ret;
    }


    ESP_LOGI(
        TAG,
        "DS3231 detected successfully at 0x%02X",
        DS3231_I2C_ADDRESS
    );


    s_initialized = true;


    /* ========================================================
     * CHECK OSCILLATOR STOP FLAG
     * ======================================================== */

    bool oscillator_stopped = false;

    ret =
        ds3231_is_oscillator_stopped(
            &oscillator_stopped
        );

    if (ret == ESP_OK) {

        if (oscillator_stopped) {

            ESP_LOGW(
                TAG,
                "DS3231 oscillator STOP FLAG is set"
            );

            ESP_LOGW(
                TAG,
                "RTC time may need to be set"
            );

        } else {

            ESP_LOGI(
                TAG,
                "DS3231 oscillator status: RUNNING"
            );
        }
    }


    ESP_LOGI(
        TAG,
        "DS3231 initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}


/* ============================================================
 * GET DATETIME
 * ============================================================ */

esp_err_t ds3231_get_datetime(
    ds3231_datetime_t *datetime
)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t reg =
        DS3231_REG_SECONDS;

    uint8_t data[7] = {0};


    esp_err_t ret =
        i2c_master_transmit_receive(
            s_ds3231_device,
            &reg,
            1,
            data,
            sizeof(data),
            1000
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to read DS3231 datetime: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* ========================================================
     * DECODE
     * ======================================================== */

    datetime->second =
        bcd_to_decimal(
            data[DS3231_REG_SECONDS]
            & 0x7F
        );

    datetime->minute =
        bcd_to_decimal(
            data[DS3231_REG_MINUTES]
            & 0x7F
        );


    /* ========================================================
     * 24-HOUR MODE
     * ======================================================== */

    uint8_t hour_reg =
        data[DS3231_REG_HOURS];

    if (hour_reg & 0x40) {

        /*
         * RTC is accidentally in 12-hour mode.
         * Convert to 24-hour representation.
         */

        uint8_t hour =
            bcd_to_decimal(
                hour_reg & 0x1F
            );

        bool pm =
            (hour_reg & 0x20) != 0;

        if (hour == 12) {
            hour = 0;
        }

        if (pm) {
            hour += 12;
        }

        datetime->hour = hour;

    } else {

        datetime->hour =
            bcd_to_decimal(
                hour_reg & 0x3F
            );
    }


    datetime->weekday =
        bcd_to_decimal(
            data[DS3231_REG_WEEKDAY]
            & 0x07
        );

    datetime->day =
        bcd_to_decimal(
            data[DS3231_REG_DAY]
            & 0x3F
        );


    uint8_t month_reg =
        data[DS3231_REG_MONTH];

    datetime->month =
        bcd_to_decimal(
            month_reg & 0x1F
        );


    uint16_t year =
        bcd_to_decimal(
            data[DS3231_REG_YEAR]
        );

    /*
     * This implementation uses 2000-2099.
     */

    datetime->year =
        2000 + year;


    return ESP_OK;
}


/* ============================================================
 * SET DATETIME
 * ============================================================ */

esp_err_t ds3231_set_datetime(
    const ds3231_datetime_t *datetime
)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!validate_datetime(datetime)) {

        ESP_LOGE(
            TAG,
            "Invalid date/time supplied"
        );

        ESP_LOGE(
            TAG,
            "Year: %u",
            datetime->year
        );

        ESP_LOGE(
            TAG,
            "Month: %u",
            datetime->month
        );

        ESP_LOGE(
            TAG,
            "Day: %u",
            datetime->day
        );

        ESP_LOGE(
            TAG,
            "Hour: %u",
            datetime->hour
        );

        ESP_LOGE(
            TAG,
            "Minute: %u",
            datetime->minute
        );

        ESP_LOGE(
            TAG,
            "Second: %u",
            datetime->second
        );

        ESP_LOGE(
            TAG,
            "Weekday: %u",
            datetime->weekday
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t data[8];


    data[0] =
        DS3231_REG_SECONDS;

    data[1] =
        decimal_to_bcd(
            datetime->second
        );

    data[2] =
        decimal_to_bcd(
            datetime->minute
        );

    /*
     * Write 24-hour mode.
     */
    data[3] =
        decimal_to_bcd(
            datetime->hour
        );

    data[4] =
        decimal_to_bcd(
            datetime->weekday
        );

    data[5] =
        decimal_to_bcd(
            datetime->day
        );

    data[6] =
        decimal_to_bcd(
            datetime->month
        );

    data[7] =
        decimal_to_bcd(
            datetime->year - 2000
        );


    esp_err_t ret =
        i2c_master_transmit(
            s_ds3231_device,
            data,
            sizeof(data),
            1000
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set DS3231 datetime: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /*
     * Clear oscillator-stop flag after
     * successfully setting the time.
     */

    ret =
        ds3231_clear_oscillator_stop_flag();

    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Datetime written but OSF flag could not be cleared"
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "DS3231 date/time updated successfully"
    );


    return ESP_OK;
}


/* ============================================================
 * CHECK OSCILLATOR STOP FLAG
 * ============================================================ */

esp_err_t ds3231_is_oscillator_stopped(
    bool *stopped
)
{
    if (stopped == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t status = 0;

    esp_err_t ret =
        ds3231_read_register(
            DS3231_REG_STATUS,
            &status
        );

    if (ret != ESP_OK) {
        return ret;
    }


    *stopped =
        (status & DS3231_STATUS_OSF) != 0;


    return ESP_OK;
}


/* ============================================================
 * CLEAR OSCILLATOR STOP FLAG
 * ============================================================ */

esp_err_t ds3231_clear_oscillator_stop_flag(void)
{
    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t status = 0;

    esp_err_t ret =
        ds3231_read_register(
            DS3231_REG_STATUS,
            &status
        );

    if (ret != ESP_OK) {
        return ret;
    }


    status &=
        ~DS3231_STATUS_OSF;


    return ds3231_write_register(
        DS3231_REG_STATUS,
        status
    );
}


/* ============================================================
 * TEMPERATURE
 * ============================================================ */

esp_err_t ds3231_get_temperature(
    float *temperature
)
{
    if (temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized ||
        s_ds3231_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t reg =
        DS3231_REG_TEMP_MSB;

    uint8_t data[2] = {0};


    esp_err_t ret =
        i2c_master_transmit_receive(
            s_ds3231_device,
            &reg,
            1,
            data,
            sizeof(data),
            1000
        );

    if (ret != ESP_OK) {
        return ret;
    }


    int8_t msb =
        (int8_t)data[0];


    /*
     * DS3231 temperature resolution:
     * 0.25 °C
     */

    uint8_t fraction =
        (data[1] >> 6) & 0x03;


    *temperature =
        (float)msb +
        ((float)fraction * 0.25f);


    return ESP_OK;
}


/* ============================================================
 * PRINT DATETIME
 * ============================================================ */

void ds3231_print_datetime(
    const ds3231_datetime_t *datetime
)
{
    if (datetime == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot print NULL datetime"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "RTC DATE/TIME: %04u-%02u-%02u %02u:%02u:%02u | Weekday: %u",
        datetime->year,
        datetime->month,
        datetime->day,
        datetime->hour,
        datetime->minute,
        datetime->second,
        datetime->weekday
    );
}