#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "nvs_flash.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "wifi_manager.h"
#include "camera.h"
#include "tft.h"

#include "face_detection.h"
#include "face_recognition/face_recognition.h"

#include "storage.h"
#include "ds3231.h"

#include "employee_db.h"
#include "attendance.h"


/* ============================================================
 * TAG
 * ============================================================ */

#define TAG "ATTENDANCE"


/* ============================================================
 * UART CONSOLE
 * ============================================================ */

#define CONSOLE_UART_NUM       UART_NUM_0
#define CONSOLE_UART_BAUDRATE  115200
#define CONSOLE_UART_RX_BUFFER 2048


/* ============================================================
 * RTC DISPLAY INTERVAL
 * ============================================================ */

#define RTC_DISPLAY_INTERVAL_MS     5000


/* ============================================================
 * FACE ENROLLMENT MODE
 * ============================================================ */

#define FACE_ENROLLMENT_MODE        true


/* ============================================================
 * SERIAL INPUT BUFFER
 * ============================================================ */

#define SERIAL_EMPLOYEE_ID_SIZE     32
#define SERIAL_EMPLOYEE_NAME_SIZE   96


/* ============================================================
 * UART CONSOLE INITIALIZATION
 *
 * ESP-IDF UART VFS normally uses non-blocking read.
 *
 * We explicitly install the UART driver and tell VFS to use
 * the interrupt-driven blocking UART driver.
 *
 * This makes Serial Monitor input interactive.
 * ============================================================ */

static esp_err_t initialize_uart_console(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "       INITIALIZING UART CONSOLE");
    ESP_LOGI(TAG, "========================================");

    /*
     * Disable stdio buffering.
     *
     * This is important for interactive Serial Monitor input.
     */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * UART configuration.
     */
    const uart_config_t uart_config = {
        .baud_rate = CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /*
     * Configure UART0.
     */
    esp_err_t ret = uart_param_config(
        CONSOLE_UART_NUM,
        &uart_config
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "uart_param_config failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * Install interrupt-driven UART driver.
     *
     * RX buffer is required because Serial Monitor sends
     * characters asynchronously.
     *
     * TX buffer = 0 because stdout can use direct TX.
     */
    ret = uart_driver_install(
        CONSOLE_UART_NUM,
        CONSOLE_UART_RX_BUFFER,
        0,
        0,
        NULL,
        0
    );

    if (ret != ESP_OK) {
        /*
         * If the driver is already installed, continue by
         * trying to use the existing driver.
         */
        if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(
                TAG,
                "uart_driver_install failed: %s",
                esp_err_to_name(ret)
            );

            return ret;
        }

        ESP_LOGW(
            TAG,
            "UART0 driver already installed"
        );
    }

    /*
     * Make UART VFS use the interrupt-driven driver.
     *
     * This is the critical fix for interactive stdin.
     */
    uart_vfs_dev_use_driver(
        CONSOLE_UART_NUM
    );

    /*
     * idf_monitor / Serial Monitor commonly sends CR
     * when ENTER is pressed.
     *
     * Convert received CR into normal newline.
     */
    uart_vfs_dev_port_set_rx_line_endings(
        CONSOLE_UART_NUM,
        ESP_LINE_ENDINGS_CR
    );

    /*
     * Send CRLF to terminal for clean display.
     */
    uart_vfs_dev_port_set_tx_line_endings(
        CONSOLE_UART_NUM,
        ESP_LINE_ENDINGS_CRLF
    );

    ESP_LOGI(
        TAG,
        "UART console initialized successfully"
    );

    ESP_LOGI(
        TAG,
        "UART     : UART0"
    );

    ESP_LOGI(
        TAG,
        "Baudrate : %d",
        CONSOLE_UART_BAUDRATE
    );

    ESP_LOGI(
        TAG,
        "Mode     : BLOCKING / INTERRUPT-DRIVEN"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    return ESP_OK;
}


/* ============================================================
 * REMOVE NEWLINE
 * ============================================================ */

static void remove_newline(char *str)
{
    if (str == NULL) {
        return;
    }

    str[strcspn(str, "\r\n")] = '\0';
}


/* ============================================================
 * WAIT FOR SERIAL INPUT
 *
 * UART VFS has already been switched to the blocking driver.
 *
 * We therefore read one character at a time.
 *
 * Characters are echoed back to the Serial Monitor so that
 * when the user types:
 *
 * D02
 *
 * the terminal visibly shows:
 *
 * D02
 *
 * ENTER completes the input.
 * ============================================================ */

static bool read_serial_line(
    const char *prompt,
    char *buffer,
    size_t buffer_size
)
{
    if (
        prompt == NULL ||
        buffer == NULL ||
        buffer_size < 2
    ) {
        return false;
    }

    memset(
        buffer,
        0,
        buffer_size
    );

    ESP_LOGI(
        TAG,
        "%s",
        prompt
    );

    fflush(stdout);

    size_t position = 0;

    while (1) {

        char character = '\0';

        ssize_t bytes_read = read(
            STDIN_FILENO,
            &character,
            1
        );

        /*
         * One character received.
         */
        if (bytes_read == 1) {

            /*
             * ENTER
             */
            if (
                character == '\r' ||
                character == '\n'
            ) {

                /*
                 * Move to next terminal line.
                 */
                printf("\r\n");

                buffer[position] = '\0';

                remove_newline(buffer);

                /*
                 * Empty input is not accepted.
                 */
                if (position == 0) {

                    ESP_LOGW(
                        TAG,
                        "Input cannot be empty"
                    );

                    return false;
                }

                ESP_LOGI(
                    TAG,
                    "Input received: %s",
                    buffer
                );

                return true;
            }


            /*
             * BACKSPACE
             *
             * Support both:
             *
             * ASCII 8
             * ASCII 127
             */
            if (
                character == '\b' ||
                character == 127
            ) {

                if (position > 0) {

                    position--;

                    buffer[position] = '\0';

                    /*
                     * Erase character visually.
                     */
                    printf("\b \b");
                    fflush(stdout);
                }

                continue;
            }


            /*
             * Ignore other control characters.
             */
            if (
                (unsigned char)character < 32
            ) {
                continue;
            }


            /*
             * Normal character.
             */
            if (
                position <
                buffer_size - 1
            ) {

                buffer[position] = character;

                position++;

                /*
                 * Echo typed character.
                 */
                putchar(character);

                fflush(stdout);
            }
            else {

                /*
                 * Buffer full.
                 */
                printf("\r\n");

                ESP_LOGW(
                    TAG,
                    "Input is too long. Maximum %u characters.",
                    (unsigned int)(buffer_size - 1)
                );

                buffer[position] = '\0';

                return false;
            }

            continue;
        }


        /*
         * Unexpected read result.
         */
        if (bytes_read < 0) {

            ESP_LOGE(
                TAG,
                "Serial input read failed"
            );

            return false;
        }


        /*
         * Zero bytes should not normally happen with blocking
         * UART driver, but handle it safely.
         */
        if (bytes_read == 0) {

            vTaskDelay(
                pdMS_TO_TICKS(10)
            );

            continue;
        }
    }
}


/* ============================================================
 * ENROLLMENT EMPLOYEE MAPPING
 * ============================================================ */

static bool enroll_employee_mapping(
    int face_id
)
{
    if (face_id < 0) {

        ESP_LOGE(
            TAG,
            "Invalid enrolled Face ID: %d",
            face_id
        );

        return false;
    }


    char employee_id[
        SERIAL_EMPLOYEE_ID_SIZE
    ] = {0};


    char employee_name[
        SERIAL_EMPLOYEE_NAME_SIZE
    ] = {0};


    ESP_LOGI(
        TAG,
        ""
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "      NEW FACE ENROLLED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "New Face ID : %d",
        face_id
    );

    ESP_LOGI(
        TAG,
        ""
    );


    /* ========================================================
     * EMPLOYEE ID
     * ======================================================== */

    while (1) {

        if (
            read_serial_line(
                "Enter Employee ID:",
                employee_id,
                sizeof(employee_id)
            )
        ) {

            break;
        }


        ESP_LOGW(
            TAG,
            "Please enter a valid Employee ID"
        );

        vTaskDelay(
            pdMS_TO_TICKS(100)
        );
    }


    /* ========================================================
     * EMPLOYEE NAME
     * ======================================================== */

    while (1) {

        if (
            read_serial_line(
                "Enter Employee Name:",
                employee_name,
                sizeof(employee_name)
            )
        ) {

            break;
        }


        ESP_LOGW(
            TAG,
            "Please enter a valid Employee Name"
        );

        vTaskDelay(
            pdMS_TO_TICKS(100)
        );
    }


    ESP_LOGI(
        TAG,
        ""
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       EMPLOYEE MAPPING"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "Face ID     : %d",
        face_id
    );

    ESP_LOGI(
        TAG,
        "Employee ID : %s",
        employee_id
    );

    ESP_LOGI(
        TAG,
        "Name        : %s",
        employee_name
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    /* ========================================================
     * SAVE MAPPING
     * ======================================================== */

    esp_err_t ret =
        employee_db_add_mapping(
            face_id,
            employee_id,
            employee_name
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "========================================"
        );

        ESP_LOGE(
            TAG,
            "EMPLOYEE MAPPING FAILED"
        );

        ESP_LOGE(
            TAG,
            "Error: %s",
            esp_err_to_name(ret)
        );

        ESP_LOGE(
            TAG,
            "========================================"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "   EMPLOYEE MAPPING COMPLETED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    /* ========================================================
     * VERIFY MAPPING
     * ======================================================== */

    employee_record_t employee;


    ret =
        employee_db_get_by_face_id(
            face_id,
            &employee
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Mapping was saved but verification failed"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "Verified mapping:"
    );

    ESP_LOGI(
        TAG,
        "Face ID     : %d",
        employee.face_id
    );

    ESP_LOGI(
        TAG,
        "Employee ID : %s",
        employee.employee_id
    );

    ESP_LOGI(
        TAG,
        "Name        : %s",
        employee.name
    );


    return true;
}


/* ============================================================
 * ATTENDANCE PROCESSING
 * ============================================================ */

static void process_attendance(
    int recognized_face_id,
    float recognized_similarity,
    bool rtc_ready,
    bool employee_db_ready,
    bool attendance_ready
)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       RECOGNIZED EMPLOYEE"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "Face ID       : %d",
        recognized_face_id
    );

    ESP_LOGI(
        TAG,
        "Similarity    : %.4f",
        recognized_similarity
    );


    /* ========================================================
     * EMPLOYEE DATABASE CHECK
     * ======================================================== */

    if (!employee_db_ready) {

        ESP_LOGW(
            TAG,
            "Employee DB is not ready"
        );

        ESP_LOGW(
            TAG,
            "Attendance cannot be mapped to employee"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        return;
    }


    employee_record_t employee;


    esp_err_t employee_ret =
        employee_db_get_by_face_id(
            recognized_face_id,
            &employee
        );


    if (employee_ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "========================================"
        );

        ESP_LOGW(
            TAG,
            "EMPLOYEE MAPPING NOT FOUND"
        );

        ESP_LOGW(
            TAG,
            "Face ID: %d",
            recognized_face_id
        );

        ESP_LOGW(
            TAG,
            "No attendance record created"
        );

        ESP_LOGW(
            TAG,
            "========================================"
        );

        return;
    }


    /* ========================================================
     * EMPLOYEE MAPPING FOUND
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "Employee mapping found"
    );

    ESP_LOGI(
        TAG,
        "Face ID      : %d",
        employee.face_id
    );

    ESP_LOGI(
        TAG,
        "Employee ID  : %s",
        employee.employee_id
    );

    ESP_LOGI(
        TAG,
        "Name         : %s",
        employee.name
    );


    /* ========================================================
     * RTC CHECK
     * ======================================================== */

    if (!rtc_ready) {

        ESP_LOGW(
            TAG,
            "Attendance skipped: RTC not ready"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        return;
    }


    /* ========================================================
     * ATTENDANCE DATABASE CHECK
     * ======================================================== */

    if (!attendance_ready) {

        ESP_LOGW(
            TAG,
            "Attendance skipped: attendance DB not ready"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        return;
    }


    /* ========================================================
     * READ RTC
     * ======================================================== */

    ds3231_datetime_t datetime;


    esp_err_t rtc_ret =
        ds3231_get_datetime(
            &datetime
        );


    if (rtc_ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot read RTC for attendance: %s",
            esp_err_to_name(rtc_ret)
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "RTC time for attendance:"
    );

    ESP_LOGI(
        TAG,
        "%04u-%02u-%02u %02u:%02u:%02u",
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second
    );


    /* ========================================================
     * MARK ATTENDANCE
     * ======================================================== */

    attendance_result_t attendance_result =
        attendance_mark(
            &employee,
            &datetime
        );


    if (
        attendance_result ==
        ATTENDANCE_RESULT_MARKED
    ) {

        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "       ATTENDANCE SUCCESS"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "Employee ID : %s",
            employee.employee_id
        );

        ESP_LOGI(
            TAG,
            "Name        : %s",
            employee.name
        );

        ESP_LOGI(
            TAG,
            "Status      : PRESENT"
        );

        ESP_LOGI(
            TAG,
            "Date        : %04u-%02u-%02u",
            datetime.year,
            datetime.month,
            datetime.day
        );

        ESP_LOGI(
            TAG,
            "Time        : %02u:%02u:%02u",
            datetime.hour,
            datetime.minute,
            datetime.second
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

    } else if (
        attendance_result ==
        ATTENDANCE_RESULT_ALREADY_MARKED
    ) {

        ESP_LOGI(
            TAG,
            "Attendance already recorded for today"
        );

        ESP_LOGI(
            TAG,
            "Employee ID : %s",
            employee.employee_id
        );

        ESP_LOGI(
            TAG,
            "Name        : %s",
            employee.name
        );

    } else {

        ESP_LOGE(
            TAG,
            "Attendance marking failed"
        );
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );
}


/* ============================================================
 * APPLICATION
 * ============================================================ */

void app_main(void)
{
    /*
     * IMPORTANT:
     *
     * UART console must be initialized BEFORE any interactive
     * Employee ID / Name input is requested.
     *
     * We do this first.
     */
    esp_err_t console_ret =
        initialize_uart_console();

    if (console_ret != ESP_OK) {

        /*
         * Do not continue into an application that requires
         * Serial input if UART initialization failed.
         */
        ESP_LOGE(
            TAG,
            "UART console initialization failed"
        );

        while (1) {

            vTaskDelay(
                pdMS_TO_TICKS(5000)
            );
        }
    }


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
        "   GC2145 -> ILI9341"
    );

    ESP_LOGI(
        TAG,
        "   DS3231 RTC ENABLED"
    );

    ESP_LOGI(
        TAG,
        "   EMPLOYEE DATABASE ENABLED"
    );

    ESP_LOGI(
        TAG,
        "   ATTENDANCE DATABASE ENABLED"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    /* ========================================================
     * NVS
     * ======================================================== */

    esp_err_t ret =
        nvs_flash_init();


    if (
        ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {

        ESP_LOGW(
            TAG,
            "NVS requires erase and reinitialization"
        );


        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );


        ret =
            nvs_flash_init();
    }


    ESP_ERROR_CHECK(ret);


    ESP_LOGI(
        TAG,
        "NVS initialized"
    );


    /* ========================================================
     * NETWORK
     * ======================================================== */

    ret =
        esp_netif_init();


    if (
        ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE
    ) {

        ESP_LOGE(
            TAG,
            "esp_netif_init failed: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Network interface initialized"
    );


    /* ========================================================
     * EVENT LOOP
     * ======================================================== */

    ret =
        esp_event_loop_create_default();


    if (
        ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE
    ) {

        ESP_LOGE(
            TAG,
            "esp_event_loop_create_default failed: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Default event loop initialized"
    );


    /* ========================================================
     * WIFI
     * ======================================================== */

    ret =
        wifi_manager_init();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Wi-Fi manager initialization failed: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    ret =
        wifi_manager_start();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Wi-Fi manager start failed: %s",
            esp_err_to_name(ret)
        );
    }


    /* ========================================================
     * RTC
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       STARTING DS3231 RTC"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    bool rtc_ready = false;


    ret =
        ds3231_init();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "DS3231 initialization FAILED: %s",
            esp_err_to_name(ret)
        );

        ESP_LOGW(
            TAG,
            "Attendance timestamps will NOT use RTC"
        );

    } else {

        rtc_ready = true;


        ESP_LOGI(
            TAG,
            "DS3231 RTC initialized successfully"
        );


        ds3231_datetime_t datetime;


        ret =
            ds3231_get_datetime(
                &datetime
            );


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to read DS3231 date/time: %s",
                esp_err_to_name(ret)
            );

        } else {

            ESP_LOGI(
                TAG,
                "======================================"
            );

            ESP_LOGI(
                TAG,
                "          CURRENT RTC TIME"
            );

            ESP_LOGI(
                TAG,
                "======================================"
            );


            ds3231_print_datetime(
                &datetime
            );


            ESP_LOGI(
                TAG,
                "RTC Year   : %04u",
                datetime.year
            );

            ESP_LOGI(
                TAG,
                "RTC Month  : %02u",
                datetime.month
            );

            ESP_LOGI(
                TAG,
                "RTC Day    : %02u",
                datetime.day
            );

            ESP_LOGI(
                TAG,
                "RTC Hour   : %02u",
                datetime.hour
            );

            ESP_LOGI(
                TAG,
                "RTC Minute : %02u",
                datetime.minute
            );

            ESP_LOGI(
                TAG,
                "RTC Second : %02u",
                datetime.second
            );

            ESP_LOGI(
                TAG,
                "======================================"
            );
        }
    }


    /* ========================================================
     * TFT
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       STARTING ILI9341 TFT"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    ret =
        tft_init();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "ILI9341 TFT initialization failed: %s",
            esp_err_to_name(ret)
        );

        ESP_LOGE(
            TAG,
            "Camera system will continue without TFT."
        );

    } else {

        ESP_LOGI(
            TAG,
            "ILI9341 TFT initialized successfully"
        );

        ESP_LOGI(
            TAG,
            "TFT resolution: 240x320"
        );

        ESP_LOGI(
            TAG,
            "TFT SPI speed: 27 MHz"
        );
    }


    /* ========================================================
     * CAMERA
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       STARTING GC2145 CAMERA"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    ret =
        camera_module_init();


    bool camera_ready = false;
    bool face_detection_ready = false;
    bool face_recognition_ready = false;
    bool employee_db_ready = false;
    bool attendance_ready = false;


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GC2145 camera initialization failed: %s",
            esp_err_to_name(ret)
        );

    } else {

        camera_ready = true;


        ESP_LOGI(
            TAG,
            "GC2145 camera initialized successfully"
        );


        /* ====================================================
         * FACE DETECTION
         * ==================================================== */

        ESP_LOGI(
            TAG,
            "======================================"
        );

        ESP_LOGI(
            TAG,
            "     STARTING HUMAN FACE DETECTION"
        );

        ESP_LOGI(
            TAG,
            "======================================"
        );


        ret =
            face_detection_init();


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Human face detection initialization failed: %s",
                esp_err_to_name(ret)
            );

        } else {

            face_detection_ready = true;


            ESP_LOGI(
                TAG,
                "Human face detection initialized successfully"
            );


            /* =================================================
             * STORAGE
             * ================================================= */

            ESP_LOGI(
                TAG,
                "======================================"
            );

            ESP_LOGI(
                TAG,
                "       STARTING SPI FLASH STORAGE"
            );

            ESP_LOGI(
                TAG,
                "======================================"
            );


            ret =
                storage_init();


            if (ret != ESP_OK) {

                ESP_LOGE(
                    TAG,
                    "SPI flash storage initialization failed: %s",
                    esp_err_to_name(ret)
                );

            } else {

                ESP_LOGI(
                    TAG,
                    "SPI flash storage initialized successfully"
                );


                /* =============================================
                 * EMPLOYEE DATABASE
                 * ============================================= */

                ESP_LOGI(
                    TAG,
                    "======================================"
                );

                ESP_LOGI(
                    TAG,
                    "       STARTING EMPLOYEE DATABASE"
                );

                ESP_LOGI(
                    TAG,
                    "======================================"
                );


                ret =
                    employee_db_init();


                if (ret != ESP_OK) {

                    ESP_LOGE(
                        TAG,
                        "Employee database initialization failed: %s",
                        esp_err_to_name(ret)
                    );

                } else {

                    employee_db_ready = true;


                    ESP_LOGI(
                        TAG,
                        "Employee database initialized successfully"
                    );

                    ESP_LOGI(
                        TAG,
                        "Employee database path:"
                    );

                    ESP_LOGI(
                        TAG,
                        "/spiflash/employees.csv"
                    );


                    employee_db_print_all();
                }


                /* =============================================
                 * ATTENDANCE DATABASE
                 * ============================================= */

                ESP_LOGI(
                    TAG,
                    "======================================"
                );

                ESP_LOGI(
                    TAG,
                    "       STARTING ATTENDANCE DATABASE"
                );

                ESP_LOGI(
                    TAG,
                    "======================================"
                );


                ret =
                    attendance_init();


                if (ret != ESP_OK) {

                    ESP_LOGE(
                        TAG,
                        "Attendance database initialization failed: %s",
                        esp_err_to_name(ret)
                    );

                } else {

                    attendance_ready = true;


                    ESP_LOGI(
                        TAG,
                        "Attendance database initialized successfully"
                    );

                    ESP_LOGI(
                        TAG,
                        "Attendance database path:"
                    );

                    ESP_LOGI(
                        TAG,
                        "/spiflash/attendance.csv"
                    );


                    attendance_print_all();
                }


                /* =============================================
                 * FACE RECOGNITION
                 * ============================================= */

                ESP_LOGI(
                    TAG,
                    "======================================"
                );

                ESP_LOGI(
                    TAG,
                    "    STARTING FACE RECOGNITION"
                );

                ESP_LOGI(
                    TAG,
                    "======================================"
                );


                ret =
                    face_recognition_init();


                if (ret != ESP_OK) {

                    ESP_LOGE(
                        TAG,
                        "Face recognition initialization failed: %s",
                        esp_err_to_name(ret)
                    );

                } else {

                    face_recognition_ready = true;


                    ESP_LOGI(
                        TAG,
                        "Face recognition initialized successfully"
                    );

                    ESP_LOGI(
                        TAG,
                        "Enrolled faces: %d",
                        face_recognition_get_num_faces()
                    );
                }
            }
        }
    }


    /* ========================================================
     * SYSTEM STATUS
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "SYSTEM INITIALIZATION COMPLETE"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    ESP_LOGI(
        TAG,
        "RTC              : %s",
        rtc_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Camera           : %s",
        camera_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Face Detection   : %s",
        face_detection_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Face Recognition : %s",
        face_recognition_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Employee DB      : %s",
        employee_db_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Attendance DB    : %s",
        attendance_ready ? "ENABLED" : "DISABLED"
    );


    if (face_recognition_ready) {

        ESP_LOGI(
            TAG,
            "Enrolled faces   : %d",
            face_recognition_get_num_faces()
        );
    }


#if FACE_ENROLLMENT_MODE

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       FACE ENROLLMENT MODE"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "Automatic enrollment: ENABLED"
    );

    ESP_LOGI(
        TAG,
        "Keep ONLY ONE person in front of camera"
    );

    ESP_LOGI(
        TAG,
        "After enrollment, Serial Monitor will ask:"
    );

    ESP_LOGI(
        TAG,
        "Employee ID"
    );

    ESP_LOGI(
        TAG,
        "Employee Name"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

#else

    ESP_LOGI(
        TAG,
        "Face Enrollment Mode: DISABLED"
    );

#endif


    /* ========================================================
     * CAMERA LOOP
     * ======================================================== */

    if (camera_ready) {

        ESP_LOGI(
            TAG,
            "======================================"
        );

        ESP_LOGI(
            TAG,
            "       LIVE CAMERA STARTING"
        );

        ESP_LOGI(
            TAG,
            "======================================"
        );

        ESP_LOGI(
            TAG,
            "Camera: 320x240 RGB565"
        );

        ESP_LOGI(
            TAG,
            "Display: 240x320 portrait"
        );

        ESP_LOGI(
            TAG,
            "SPI: 27 MHz"
        );


        if (face_detection_ready) {

            ESP_LOGI(
                TAG,
                "Face Detection: ENABLED"
            );

            ESP_LOGI(
                TAG,
                "Detection interval: every 5th frame"
            );

        } else {

            ESP_LOGI(
                TAG,
                "Face Detection: DISABLED"
            );
        }


        if (face_recognition_ready) {

            ESP_LOGI(
                TAG,
                "Face Recognition: ENABLED"
            );

            ESP_LOGI(
                TAG,
                "Recognition model: MFN_S8_V1"
            );

            ESP_LOGI(
                TAG,
                "Recognition database: /spiflash/face.db"
            );

            ESP_LOGI(
                TAG,
                "Enrolled faces: %d",
                face_recognition_get_num_faces()
            );

        } else {

            ESP_LOGI(
                TAG,
                "Face Recognition: DISABLED"
            );
        }


        if (employee_db_ready) {

            ESP_LOGI(
                TAG,
                "Employee DB: /spiflash/employees.csv"
            );

        } else {

            ESP_LOGI(
                TAG,
                "Employee DB: DISABLED"
            );
        }


        if (attendance_ready) {

            ESP_LOGI(
                TAG,
                "Attendance DB: /spiflash/attendance.csv"
            );

        } else {

            ESP_LOGI(
                TAG,
                "Attendance DB: DISABLED"
            );
        }


        if (rtc_ready) {

            ESP_LOGI(
                TAG,
                "RTC: DS3231 @ 0x68"
            );

            ESP_LOGI(
                TAG,
                "RTC: GPIO3 SDA / GPIO2 SCL"
            );

        } else {

            ESP_LOGI(
                TAG,
                "RTC: DISABLED"
            );
        }


        ESP_LOGI(
            TAG,
            "======================================"
        );


        /* ====================================================
         * RUNTIME VARIABLES
         * ==================================================== */

        uint32_t frame_count = 0;

        uint32_t detection_frame_counter = 0;


        TickType_t fps_start =
            xTaskGetTickCount();


        TickType_t rtc_last_read =
            xTaskGetTickCount();


        /* ====================================================
         * ENROLLMENT STATE
         * ==================================================== */

        bool enrollment_completed = false;

        bool employee_mapping_completed = false;

        bool employee_mapping_pending = false;

        int pending_face_id = -1;


        /*
         * If recognition is unavailable,
         * enrollment cannot be performed.
         */

        if (!face_recognition_ready) {

            enrollment_completed = true;

            employee_mapping_completed = true;


            ESP_LOGW(
                TAG,
                "Enrollment disabled because face recognition"
                " is not ready"
            );
        }


        /* ====================================================
         * CAMERA LOOP
         * ==================================================== */

        while (1) {

            /* =================================================
             * CAMERA CAPTURE
             * ================================================= */

            camera_fb_t *fb =
                camera_module_capture();


            if (fb == NULL) {

                ESP_LOGE(
                    TAG,
                    "No camera frame received"
                );


                vTaskDelay(
                    pdMS_TO_TICKS(10)
                );


                continue;
            }


            /* =================================================
             * TFT DISPLAY
             * ================================================= */

            bool display_ok =
                tft_display_camera_frame(
                    fb
                );


            /* =================================================
             * FACE DETECTION
             * ================================================= */

            detection_frame_counter++;


            if (
                face_detection_ready &&
                detection_frame_counter >= 5
            ) {

                detection_frame_counter = 0;


                bool face_found =
                    face_detection_process(
                        fb
                    );


                /* =============================================
                 * FACE ENROLLMENT
                 * ============================================= */

#if FACE_ENROLLMENT_MODE

                if (
                    face_recognition_ready &&
                    !enrollment_completed &&
                    !employee_mapping_pending &&
                    face_found
                ) {

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGI(
                        TAG,
                        "      ENROLLMENT FACE DETECTED"
                    );

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );


                    bool enroll_success =
                        face_recognition_enroll(
                            fb
                        );


                    if (enroll_success) {

                        enrollment_completed = true;

                        employee_mapping_pending = true;


                        int new_face_id = -1;


                        bool face_id_available =
                            face_recognition_get_last_enrolled_face_id(
                                &new_face_id
                            );


                        if (!face_id_available) {

                            ESP_LOGE(
                                TAG,
                                "Face enrolled but new Face ID could not be obtained"
                            );

                            employee_mapping_pending = false;

                        } else {

                            pending_face_id =
                                new_face_id;


                            ESP_LOGI(
                                TAG,
                                "========================================"
                            );

                            ESP_LOGI(
                                TAG,
                                "       ENROLLMENT COMPLETED"
                            );

                            ESP_LOGI(
                                TAG,
                                "========================================"
                            );

                            ESP_LOGI(
                                TAG,
                                "New Face ID : %d",
                                pending_face_id
                            );

                            ESP_LOGI(
                                TAG,
                                "Total enrolled faces: %d",
                                face_recognition_get_num_faces()
                            );

                            ESP_LOGI(
                                TAG,
                                "Face database: /spiflash/face.db"
                            );

                            ESP_LOGI(
                                TAG,
                                "Employee mapping is now required"
                            );

                            ESP_LOGI(
                                TAG,
                                "========================================"
                            );
                        }


                    } else {

                        ESP_LOGW(
                            TAG,
                            "Face enrollment attempt failed"
                        );

                        ESP_LOGW(
                            TAG,
                            "Waiting for another valid face frame..."
                        );
                    }
                }


                /* =============================================
                 * NORMAL FACE RECOGNITION
                 * ============================================= */

                if (
                    face_recognition_ready &&
                    face_found &&
                    enrollment_completed &&
                    employee_mapping_completed &&
                    !employee_mapping_pending
                ) {

                    bool recognized =
                        face_recognition_process(
                            fb
                        );


                    if (recognized) {

                        int recognized_face_id = -1;

                        float recognized_similarity = 0.0f;


                        bool result_available =
                            face_recognition_get_last_result(
                                &recognized_face_id,
                                &recognized_similarity
                            );


                        if (result_available) {

                            process_attendance(
                                recognized_face_id,
                                recognized_similarity,
                                rtc_ready,
                                employee_db_ready,
                                attendance_ready
                            );
                        }
                    }
                }

#else

                /* =============================================
                 * NORMAL RECOGNITION-ONLY MODE
                 * ============================================= */

                if (
                    face_recognition_ready &&
                    face_found
                ) {

                    bool recognized =
                        face_recognition_process(
                            fb
                        );


                    if (recognized) {

                        int recognized_face_id = -1;

                        float recognized_similarity = 0.0f;


                        bool result_available =
                            face_recognition_get_last_result(
                                &recognized_face_id,
                                &recognized_similarity
                            );


                        if (result_available) {

                            process_attendance(
                                recognized_face_id,
                                recognized_similarity,
                                rtc_ready,
                                employee_db_ready,
                                attendance_ready
                            );
                        }
                    }
                }

#endif
            }


            /* =================================================
             * RETURN CAMERA FRAME
             * ================================================= */

            camera_module_return_frame(
                fb
            );


            if (!display_ok) {

                ESP_LOGE(
                    TAG,
                    "TFT frame display failed"
                );
            }


            /* =================================================
             * EMPLOYEE MAPPING
             * ================================================= */

#if FACE_ENROLLMENT_MODE

            if (
                employee_mapping_pending &&
                pending_face_id >= 0
            ) {

                ESP_LOGI(
                    TAG,
                    ""
                );

                ESP_LOGI(
                    TAG,
                    "========================================"
                );

                ESP_LOGI(
                    TAG,
                    "    EMPLOYEE REGISTRATION REQUIRED"
                );

                ESP_LOGI(
                    TAG,
                    "========================================"
                );

                ESP_LOGI(
                    TAG,
                    "Face ID: %d",
                    pending_face_id
                );

                ESP_LOGI(
                    TAG,
                    "The face has already been stored."
                );

                ESP_LOGI(
                    TAG,
                    "Now enter employee details in Serial Monitor."
                );

                ESP_LOGI(
                    TAG,
                    "========================================"
                );


                bool mapping_success =
                    false;


                if (employee_db_ready) {

                    mapping_success =
                        enroll_employee_mapping(
                            pending_face_id
                        );

                } else {

                    ESP_LOGE(
                        TAG,
                        "Employee DB is not ready"
                    );

                    ESP_LOGE(
                        TAG,
                        "Cannot create employee mapping"
                    );
                }


                if (mapping_success) {

                    employee_mapping_completed = true;

                    employee_mapping_pending = false;


                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGI(
                        TAG,
                        "     SYSTEM READY FOR RECOGNITION"
                    );

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGI(
                        TAG,
                        "Face ID %d is now linked to employee",
                        pending_face_id
                    );

                    ESP_LOGI(
                        TAG,
                        "Recognition will start from next frame."
                    );

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );


                    employee_db_print_all();


                    pending_face_id = -1;


                } else {

                    ESP_LOGE(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGE(
                        TAG,
                        "EMPLOYEE MAPPING NOT COMPLETED"
                    );

                    ESP_LOGE(
                        TAG,
                        "Recognition remains paused."
                    );

                    ESP_LOGE(
                        TAG,
                        "========================================"
                    );
                }
            }

#endif


            /* =================================================
             * FRAME COUNTER
             * ================================================= */

            frame_count++;


            TickType_t now =
                xTaskGetTickCount();


            /* =================================================
             * FPS
             * ================================================= */

            if (
                now - fps_start >=
                pdMS_TO_TICKS(5000)
            ) {

                uint32_t elapsed_ms =
                    (now - fps_start) *
                    portTICK_PERIOD_MS;


                float fps =
                    (frame_count * 1000.0f) /
                    elapsed_ms;


                ESP_LOGI(
                    TAG,
                    "LIVE FPS: %.2f | Frames: %lu",
                    fps,
                    (unsigned long)frame_count
                );


                frame_count = 0;

                fps_start = now;
            }


            /* =================================================
             * RTC PERIODIC READ
             * ================================================= */

            if (
                rtc_ready &&
                now - rtc_last_read >=
                pdMS_TO_TICKS(
                    RTC_DISPLAY_INTERVAL_MS
                )
            ) {

                rtc_last_read = now;


                ds3231_datetime_t datetime;


                ret =
                    ds3231_get_datetime(
                        &datetime
                    );


                if (ret == ESP_OK) {

                    ESP_LOGI(
                        TAG,
                        "CURRENT RTC -> %04u-%02u-%02u %02u:%02u:%02u",
                        datetime.year,
                        datetime.month,
                        datetime.day,
                        datetime.hour,
                        datetime.minute,
                        datetime.second
                    );

                } else {

                    ESP_LOGE(
                        TAG,
                        "RTC read failed: %s",
                        esp_err_to_name(ret)
                    );
                }
            }
        }

    } else {

        /* ====================================================
         * NO CAMERA
         * ==================================================== */

        while (1) {

            ESP_LOGW(
                TAG,
                "System running without camera"
            );


            if (rtc_ready) {

                ds3231_datetime_t datetime;


                ret =
                    ds3231_get_datetime(
                        &datetime
                    );


                if (ret == ESP_OK) {

                    ESP_LOGI(
                        TAG,
                        "RTC -> %04u-%02u-%02u %02u:%02u:%02u",
                        datetime.year,
                        datetime.month,
                        datetime.day,
                        datetime.hour,
                        datetime.minute,
                        datetime.second
                    );
                }
            }


            vTaskDelay(
                pdMS_TO_TICKS(5000)
            );
        }
    }
}