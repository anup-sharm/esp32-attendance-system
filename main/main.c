#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "nvs_flash.h"

#include "wifi_manager.h"
#include "camera.h"
#include "tft.h"

#include "face_detection.h"
#include "face_recognition/face_recognition.h"

#include "storage.h"
#include "database_reset.h"

#include "ds3231.h"

#include "employee_db.h"
#include "attendance.h"

#include "web_server.h"

/*
 * AUDIO INTENTIONALLY DISABLED
 *
 * Sound / MAX98357A / PicoTTS is paused for now.
 *
 * Do NOT include audio.h
 */


/* ============================================================
 * TAG
 * ============================================================ */

#define TAG "ATTENDANCE"


/* ============================================================
 * RTC DISPLAY INTERVAL
 * ============================================================ */

#define RTC_DISPLAY_INTERVAL_MS     5000


/* ============================================================
 * FACE DETECTION
 * ============================================================ */

#define FACE_DETECTION_INTERVAL_FRAMES    5


/* ============================================================
 * ATTENDANCE DUPLICATE COOLDOWN
 * ============================================================ */

#define ATTENDANCE_COOLDOWN_MS      8000


/* ============================================================
 * REGISTRATION
 * ============================================================ */

#define ENROLLMENT_REQUIRED_DETECTIONS     3

#define ENROLLMENT_TIMEOUT_MS              30000


/* ============================================================
 * ATTENDANCE SYSTEM STATE
 * ============================================================ */

static int last_attendance_face_id = -1;

static int64_t last_attendance_process_time_us = 0;


/* ============================================================
 * ATTENDANCE COOLDOWN
 * ============================================================ */

static bool attendance_cooldown_allowed(int face_id)
{
    int64_t now_us = esp_timer_get_time();

    if (last_attendance_face_id == face_id)
    {
        int64_t elapsed_us =
            now_us - last_attendance_process_time_us;

        if (elapsed_us <
            ((int64_t)ATTENDANCE_COOLDOWN_MS * 1000))
        {
            return false;
        }
    }

    last_attendance_face_id =
        face_id;

    last_attendance_process_time_us =
        now_us;

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
     * DUPLICATE FRAME PROTECTION
     * ======================================================== */

    if (!attendance_cooldown_allowed(
            recognized_face_id))
    {
        ESP_LOGI(
            TAG,
            "Attendance cooldown active for Face ID %d",
            recognized_face_id
        );

        return;
    }


    /* ========================================================
     * EMPLOYEE DATABASE CHECK
     * ======================================================== */

    if (!employee_db_ready)
    {
        ESP_LOGW(
            TAG,
            "Employee DB is not ready"
        );

        ESP_LOGW(
            TAG,
            "Attendance cannot be mapped to employee"
        );

        return;
    }


    employee_record_t employee;

    memset(
        &employee,
        0,
        sizeof(employee)
    );


    esp_err_t employee_ret =
        employee_db_get_by_face_id(
            recognized_face_id,
            &employee
        );


    if (employee_ret != ESP_OK)
    {
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

    if (!rtc_ready)
    {
        ESP_LOGW(
            TAG,
            "Attendance skipped: RTC not ready"
        );

        return;
    }


    /* ========================================================
     * ATTENDANCE DATABASE CHECK
     * ======================================================== */

    if (!attendance_ready)
    {
        ESP_LOGW(
            TAG,
            "Attendance skipped: attendance DB not ready"
        );

        return;
    }


    /* ========================================================
     * READ RTC
     * ======================================================== */

    ds3231_datetime_t datetime;

    memset(
        &datetime,
        0,
        sizeof(datetime)
    );


    esp_err_t rtc_ret =
        ds3231_get_datetime(
            &datetime
        );


    if (rtc_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot read RTC for attendance: %s",
            esp_err_to_name(rtc_ret)
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
     * CHECK TODAY'S CURRENT STATE
     * ======================================================== */

    bool has_in =
        attendance_has_in_today(
            &employee,
            &datetime
        );

    bool has_out =
        attendance_has_out_today(
            &employee,
            &datetime
        );


    if (has_in && has_out)
    {
        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "TODAY ATTENDANCE COMPLETE"
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
            "State       : IN + OUT"
        );

        ESP_LOGI(
            TAG,
            "Further scans ignored for today"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        return;
    }


    /* ========================================================
     * MARK ATTENDANCE
     *
     * No IN       -> IN
     * IN only     -> OUT
     * IN + OUT    -> ignore
     * ======================================================== */

    attendance_result_t attendance_result =
        attendance_mark(
            &employee,
            &datetime
        );


    /* ========================================================
     * FIRST SCAN = IN
     * ======================================================== */

    if (attendance_result ==
        ATTENDANCE_RESULT_IN)
    {
        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "       ATTENDANCE IN SUCCESS"
        );

        ESP_LOGI(
            TAG,
            "========================================"
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

        ESP_LOGI(
            TAG,
            "Status      : IN"
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

        return;
    }


    /* ========================================================
     * SECOND SCAN = OUT
     * ======================================================== */

    if (attendance_result ==
        ATTENDANCE_RESULT_OUT)
    {
        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "       ATTENDANCE OUT SUCCESS"
        );

        ESP_LOGI(
            TAG,
            "========================================"
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

        ESP_LOGI(
            TAG,
            "Status      : OUT"
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

        return;
    }


    /* ========================================================
     * ALREADY COMPLETE
     * ======================================================== */

    if (attendance_result ==
        ATTENDANCE_RESULT_ALREADY_COMPLETE)
    {
        ESP_LOGI(
            TAG,
            "Attendance already completed for today"
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

        return;
    }


    /* ========================================================
     * COMPATIBILITY WITH OLD ATTENDANCE RESULT
     * ======================================================== */

    if (attendance_result ==
        ATTENDANCE_RESULT_ALREADY_MARKED)
    {
        ESP_LOGI(
            TAG,
            "Attendance already completed for today"
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

        return;
    }


    /* ========================================================
     * FAILURE
     * ======================================================== */

    ESP_LOGE(
        TAG,
        "Attendance marking failed"
    );

    ESP_LOGE(
        TAG,
        "Result code: %d",
        (int)attendance_result
    );


    ESP_LOGI(
        TAG,
        "========================================"
    );
}


/* ============================================================
 * START PHONE REGISTRATION REQUEST
 * ============================================================ */

static bool start_phone_registration(
    char *pending_employee_id,
    size_t pending_employee_id_size,
    char *pending_employee_name,
    size_t pending_employee_name_size
)
{
    if (!web_server_registration_requested())
    {
        return false;
    }


    memset(
        pending_employee_id,
        0,
        pending_employee_id_size
    );

    memset(
        pending_employee_name,
        0,
        pending_employee_name_size
    );


    esp_err_t ret =
        web_server_get_registration_request(
            pending_employee_id,
            pending_employee_id_size,
            pending_employee_name,
            pending_employee_name_size
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to read phone registration request: %s",
            esp_err_to_name(ret)
        );

        web_server_set_registration_status(
            "REQUEST ERROR",
            -1
        );

        web_server_clear_registration_request();

        return false;
    }


    web_server_clear_registration_request();


    if (strlen(pending_employee_id) == 0)
    {
        ESP_LOGW(
            TAG,
            "Registration rejected: Employee ID is empty"
        );

        web_server_set_registration_status(
            "EMPLOYEE ID REQUIRED",
            -1
        );

        return false;
    }


    if (strlen(pending_employee_name) == 0)
    {
        ESP_LOGW(
            TAG,
            "Registration rejected: Employee Name is empty"
        );

        web_server_set_registration_status(
            "EMPLOYEE NAME REQUIRED",
            -1
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       PHONE REGISTRATION REQUEST"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "Employee ID : %s",
        pending_employee_id
    );

    ESP_LOGI(
        TAG,
        "Name        : %s",
        pending_employee_name
    );

    ESP_LOGI(
        TAG,
        "Registration mode started"
    );

    ESP_LOGI(
        TAG,
        "Waiting for exactly one stable face..."
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    web_server_set_registration_status(
        "DETECTING FACE",
        -1
    );


    return true;
}


/* ============================================================
 * APPLICATION
 * ============================================================ */

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
        "   AUDIO DISABLED"
    );

    ESP_LOGI(
        TAG,
        "   PHONE REGISTRATION ENABLED"
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
    )
    {
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
    )
    {
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
    )
    {
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


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi manager initialization failed: %s",
            esp_err_to_name(ret)
        );
    }
    else
    {
        ret =
            wifi_manager_start();


        if (ret != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Wi-Fi manager start failed: %s",
                esp_err_to_name(ret)
            );
        }
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


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "DS3231 initialization FAILED: %s",
            esp_err_to_name(ret)
        );
    }
    else
    {
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


        if (ret == ESP_OK)
        {
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


    bool tft_ready = false;


    ret =
        tft_init();


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "ILI9341 TFT initialization failed: %s",
            esp_err_to_name(ret)
        );
    }
    else
    {
        tft_ready = true;


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
            "TFT SPI speed: 20 MHz"
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


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "GC2145 camera initialization failed: %s",
            esp_err_to_name(ret)
        );
    }
    else
    {
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


        /*
         * IMPORTANT:
         *
         * face_detection_init() returns bool.
         *
         * It does NOT return esp_err_t.
         *
         * Therefore it MUST NOT be checked against ESP_OK.
         *
         * Old incorrect code:
         *
         * ret = face_detection_init();
         *
         * if (ret != ESP_OK)
         *
         * Since true == 1 and ESP_OK == 0,
         * successful initialization was incorrectly reported
         * as a failure.
         */

        bool face_detection_init_ok =
            face_detection_init();


        if (!face_detection_init_ok)
        {
            ESP_LOGE(
                TAG,
                "Human face detection initialization failed"
            );

            face_detection_ready = false;
        }
        else
        {
            face_detection_ready = true;


            ESP_LOGI(
                TAG,
                "Human face detection initialized successfully"
            );


            /* =================================================
             * SPI FLASH STORAGE
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


            if (ret != ESP_OK)
            {
                ESP_LOGE(
                    TAG,
                    "SPI flash storage initialization failed: %s",
                    esp_err_to_name(ret)
                );
            }
            else
            {
                ESP_LOGI(
                    TAG,
                    "SPI flash storage initialized successfully"
                );


                /* =================================================
                 * DATABASE RESET / MIGRATION
                 * ================================================= */

                ESP_LOGI(
                    TAG,
                    "======================================"
                );

                ESP_LOGI(
                    TAG,
                    "       DATABASE INITIALIZATION"
                );

                ESP_LOGI(
                    TAG,
                    "======================================"
                );


                ret =
                    database_reset_init();


                if (ret != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Database reset/initialization FAILED: %s",
                        esp_err_to_name(ret)
                    );

                    ESP_LOGE(
                        TAG,
                        "Employee/attendance/face database startup halted"
                    );
                }
                else
                {
                    ESP_LOGI(
                        TAG,
                        "Database reset/initialization completed"
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


                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(
                            TAG,
                            "Employee database initialization failed: %s",
                            esp_err_to_name(ret)
                        );
                    }
                    else
                    {
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


                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(
                            TAG,
                            "Attendance database initialization failed: %s",
                            esp_err_to_name(ret)
                        );
                    }
                    else
                    {
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


                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(
                            TAG,
                            "Face recognition initialization failed: %s",
                            esp_err_to_name(ret)
                        );
                    }
                    else
                    {
                        face_recognition_ready = true;


                        ESP_LOGI(
                            TAG,
                            "Face recognition initialized successfully"
                        );

                        ESP_LOGI(
                            TAG,
                            "Recognition threshold: %.2f",
                            face_recognition_get_application_threshold()
                        );

                        ESP_LOGI(
                            TAG,
                            "Enrolled faces: %d",
                            face_recognition_get_num_faces()
                        );


                        int enrolled_faces =
                            face_recognition_get_num_faces();


                        ESP_LOGI(
                            TAG,
                            "======================================"
                        );

                        ESP_LOGI(
                            TAG,
                            "       DATABASE STATE"
                        );

                        ESP_LOGI(
                            TAG,
                            "======================================"
                        );

                        ESP_LOGI(
                            TAG,
                            "Face IDs       : %d",
                            enrolled_faces
                        );

                        ESP_LOGI(
                            TAG,
                            "Employee DB    : %s",
                            employee_db_ready
                                ? "READY"
                                : "NOT READY"
                        );

                        ESP_LOGI(
                            TAG,
                            "Attendance DB  : %s",
                            attendance_ready
                                ? "READY"
                                : "NOT READY"
                        );

                        ESP_LOGI(
                            TAG,
                            "======================================"
                        );


                        if (enrolled_faces == 0)
                        {
                            ESP_LOGI(
                                TAG,
                                "NO FACE REGISTERED"
                            );

                            ESP_LOGI(
                                TAG,
                                "System is waiting for admin registration"
                            );
                        }
                        else
                        {
                            ESP_LOGI(
                                TAG,
                                "Registered faces found: %d",
                                enrolled_faces
                            );
                        }
                    }
                }
            }
        }
    }


    /* ========================================================
     * AUDIO
     * ======================================================== */

    bool audio_ready = false;


    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       AUDIO / MAX98357A"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "Audio is intentionally DISABLED"
    );

    ESP_LOGI(
        TAG,
        "MAX98357A / PicoTTS will be enabled later"
    );


    /* ========================================================
     * WEB SERVER
     * ======================================================== */

    bool web_server_ready = false;


    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       STARTING ADMIN WEB SERVER"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    ret =
        web_server_start();


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Admin web server start failed: %s",
            esp_err_to_name(ret)
        );
    }
    else
    {
        web_server_ready = true;

        ESP_LOGI(
            TAG,
            "Admin web server started"
        );

        ESP_LOGI(
            TAG,
            "Phone registration is ENABLED"
        );
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
        "TFT              : %s",
        tft_ready ? "ENABLED" : "DISABLED"
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

    ESP_LOGI(
        TAG,
        "Web Server       : %s",
        web_server_ready ? "ENABLED" : "DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Audio            : %s",
        audio_ready ? "ENABLED" : "DISABLED"
    );


    if (face_recognition_ready)
    {
        ESP_LOGI(
            TAG,
            "Enrolled faces   : %d",
            face_recognition_get_num_faces()
        );
    }


    /* ========================================================
     * REGISTRATION / ATTENDANCE STATUS
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "       REGISTRATION / ATTENDANCE"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    if (face_recognition_ready &&
        employee_db_ready)
    {
        int enrolled =
            face_recognition_get_num_faces();


        if (enrolled == 0)
        {
            ESP_LOGI(
                TAG,
                "STATUS: WAITING FOR ADMIN REGISTRATION"
            );

            ESP_LOGI(
                TAG,
                "Phone/Admin registration required"
            );

            ESP_LOGI(
                TAG,
                "Attendance will start after first"
            );

            ESP_LOGI(
                TAG,
                "successful employee registration"
            );
        }
        else
        {
            ESP_LOGI(
                TAG,
                "STATUS: ATTENDANCE READY"
            );

            ESP_LOGI(
                TAG,
                "Registered faces: %d",
                enrolled
            );
        }
    }
    else
    {
        ESP_LOGW(
            TAG,
            "STATUS: SYSTEM NOT READY"
        );
    }


    ESP_LOGI(
        TAG,
        "Serial employee registration: DISABLED"
    );

    ESP_LOGI(
        TAG,
        "Registration method: PHONE / ADMIN"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    /* ========================================================
     * CAMERA LOOP
     * ======================================================== */

    if (camera_ready)
    {
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
            "SPI: 20 MHz"
        );


        if (face_detection_ready)
        {
            ESP_LOGI(
                TAG,
                "Face Detection: ENABLED"
            );

            ESP_LOGI(
                TAG,
                "Detection interval: every %dth frame",
                FACE_DETECTION_INTERVAL_FRAMES
            );
        }
        else
        {
            ESP_LOGI(
                TAG,
                "Face Detection: DISABLED"
            );
        }


        if (face_recognition_ready)
        {
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
                "Recognition threshold: %.2f",
                face_recognition_get_application_threshold()
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
        }
        else
        {
            ESP_LOGI(
                TAG,
                "Face Recognition: DISABLED"
            );
        }


        if (employee_db_ready)
        {
            ESP_LOGI(
                TAG,
                "Employee DB: /spiflash/employees.csv"
            );
        }
        else
        {
            ESP_LOGI(
                TAG,
                "Employee DB: DISABLED"
            );
        }


        if (attendance_ready)
        {
            ESP_LOGI(
                TAG,
                "Attendance DB: /spiflash/attendance.csv"
            );
        }
        else
        {
            ESP_LOGI(
                TAG,
                "Attendance DB: DISABLED"
            );
        }


        if (rtc_ready)
        {
            ESP_LOGI(
                TAG,
                "RTC: DS3231 @ 0x68"
            );

            ESP_LOGI(
                TAG,
                "RTC: GPIO3 SDA / GPIO2 SCL"
            );
        }
        else
        {
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
         * PHONE REGISTRATION STATE
         * ==================================================== */

        bool enrollment_mode = false;

        char pending_employee_id[32] = {0};

        char pending_employee_name[96] = {0};

        int enrollment_stable_detections = 0;

        TickType_t enrollment_start_tick = 0;


        /* ====================================================
         * CAMERA LOOP
         * ==================================================== */

        while (1)
        {
            /* =================================================
             * CHECK PHONE REGISTRATION REQUEST
             * ================================================= */

            if (!enrollment_mode &&
                face_recognition_ready &&
                employee_db_ready &&
                web_server_ready)
            {
                if (start_phone_registration(
                        pending_employee_id,
                        sizeof(pending_employee_id),
                        pending_employee_name,
                        sizeof(pending_employee_name)))
                {
                    enrollment_mode = true;

                    enrollment_stable_detections = 0;

                    enrollment_start_tick =
                        xTaskGetTickCount();


                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGI(
                        TAG,
                        "       ENROLLMENT MODE ACTIVE"
                    );

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );

                    ESP_LOGI(
                        TAG,
                        "Employee ID : %s",
                        pending_employee_id
                    );

                    ESP_LOGI(
                        TAG,
                        "Name        : %s",
                        pending_employee_name
                    );

                    ESP_LOGI(
                        TAG,
                        "Look at camera and hold still"
                    );

                    ESP_LOGI(
                        TAG,
                        "========================================"
                    );
                }
            }


            /* =================================================
             * CAMERA CAPTURE
             * ================================================= */

            camera_fb_t *fb =
                camera_module_capture();


            if (fb == NULL)
            {
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
                detection_frame_counter >=
                    FACE_DETECTION_INTERVAL_FRAMES
            )
            {
                detection_frame_counter = 0;


                bool face_found =
                    face_detection_process(
                        fb
                    );


                /* =================================================
                 * ENROLLMENT MODE
                 * ================================================= */

                if (enrollment_mode)
                {
                    /* =============================================
                     * ENROLLMENT TIMEOUT
                     * ============================================= */

                    TickType_t now_tick =
                        xTaskGetTickCount();


                    uint32_t enrollment_elapsed_ms =
                        (uint32_t)(
                            (
                                now_tick -
                                enrollment_start_tick
                            ) * portTICK_PERIOD_MS
                        );


                    if (
                        enrollment_elapsed_ms >=
                        ENROLLMENT_TIMEOUT_MS
                    )
                    {
                        ESP_LOGW(
                            TAG,
                            "========================================"
                        );

                        ESP_LOGW(
                            TAG,
                            "       ENROLLMENT TIMEOUT"
                        );

                        ESP_LOGW(
                            TAG,
                            "========================================"
                        );

                        ESP_LOGW(
                            TAG,
                            "No stable face captured"
                        );


                        web_server_set_registration_status(
                            "TIMEOUT - TRY AGAIN",
                            -1
                        );


                        enrollment_mode = false;

                        enrollment_stable_detections = 0;

                        memset(
                            pending_employee_id,
                            0,
                            sizeof(pending_employee_id)
                        );

                        memset(
                            pending_employee_name,
                            0,
                            sizeof(pending_employee_name)
                        );
                    }
                    else
                    {
                        /* =========================================
                         * NO FACE
                         * ========================================= */

                        if (!face_found)
                        {
                            enrollment_stable_detections = 0;


                            web_server_set_registration_status(
                                "LOOK AT CAMERA",
                                -1
                            );


                            ESP_LOGI(
                                TAG,
                                "Enrollment: no face detected"
                            );
                        }
                        else
                        {
                            /* =====================================
                             * FACE DETECTED
                             * ===================================== */

                            enrollment_stable_detections++;


                            ESP_LOGI(
                                TAG,
                                "Enrollment face detection %d/%d",
                                enrollment_stable_detections,
                                ENROLLMENT_REQUIRED_DETECTIONS
                            );


                            if (
                                enrollment_stable_detections == 1
                            )
                            {
                                web_server_set_registration_status(
                                    "FACE DETECTED - HOLD STILL",
                                    -1
                                );
                            }


                            /* =====================================
                             * STABLE FACE
                             * ===================================== */

                            if (
                                enrollment_stable_detections >=
                                ENROLLMENT_REQUIRED_DETECTIONS
                            )
                            {
                                ESP_LOGI(
                                    TAG,
                                    "========================================"
                                );

                                ESP_LOGI(
                                    TAG,
                                    "       STARTING FACE ENROLLMENT"
                                );

                                ESP_LOGI(
                                    TAG,
                                    "========================================"
                                );

                                ESP_LOGI(
                                    TAG,
                                    "Employee ID : %s",
                                    pending_employee_id
                                );

                                ESP_LOGI(
                                    TAG,
                                    "Name        : %s",
                                    pending_employee_name
                                );


                                web_server_set_registration_status(
                                    "CAPTURING FACE",
                                    -1
                                );


                                /* =================================
                                 * ACTUAL FACE ENROLLMENT
                                 * ================================= */

                                bool enroll_ok =
                                    face_recognition_enroll(
                                        fb
                                    );


                                if (!enroll_ok)
                                {
                                    ESP_LOGW(
                                        TAG,
                                        "Face enrollment failed"
                                    );

                                    web_server_set_registration_status(
                                        "ENROLLMENT FAILED - TRY AGAIN",
                                        -1
                                    );


                                    enrollment_stable_detections = 0;
                                }
                                else
                                {
                                    /* =============================
                                     * GET GENERATED FACE ID
                                     * ============================= */

                                    int new_face_id = -1;


                                    bool face_id_ok =
                                        face_recognition_get_last_enrolled_face_id(
                                            &new_face_id
                                        );


                                    if (!face_id_ok ||
                                        new_face_id < 0)
                                    {
                                        ESP_LOGE(
                                            TAG,
                                            "Face enrolled but Face ID could not be read"
                                        );

                                        web_server_set_registration_status(
                                            "FACE ID ERROR",
                                            -1
                                        );


                                        enrollment_stable_detections = 0;
                                    }
                                    else
                                    {
                                        /* =========================
                                         * SAVE EMPLOYEE MAPPING
                                         * ========================= */

                                        ESP_LOGI(
                                            TAG,
                                            "Face enrollment successful"
                                        );

                                        ESP_LOGI(
                                            TAG,
                                            "Generated Face ID: %d",
                                            new_face_id
                                        );


                                        web_server_set_registration_status(
                                            "SAVING EMPLOYEE",
                                            new_face_id
                                        );


                                        esp_err_t employee_add_ret =
                                            employee_db_add_mapping(
                                                new_face_id,
                                                pending_employee_id,
                                                pending_employee_name
                                            );


                                        if (
                                            employee_add_ret != ESP_OK
                                        )
                                        {
                                            ESP_LOGE(
                                                TAG,
                                                "========================================"
                                            );

                                            ESP_LOGE(
                                                TAG,
                                                "EMPLOYEE MAPPING SAVE FAILED"
                                            );

                                            ESP_LOGE(
                                                TAG,
                                                "Face ID      : %d",
                                                new_face_id
                                            );

                                            ESP_LOGE(
                                                TAG,
                                                "Employee ID  : %s",
                                                pending_employee_id
                                            );

                                            ESP_LOGE(
                                                TAG,
                                                "Error        : %s",
                                                esp_err_to_name(
                                                    employee_add_ret
                                                )
                                            );

                                            ESP_LOGE(
                                                TAG,
                                                "========================================"
                                            );


                                            web_server_set_registration_status(
                                                "EMPLOYEE SAVE FAILED",
                                                new_face_id
                                            );


                                            enrollment_mode = false;

                                            enrollment_stable_detections = 0;

                                            memset(
                                                pending_employee_id,
                                                0,
                                                sizeof(pending_employee_id)
                                            );

                                            memset(
                                                pending_employee_name,
                                                0,
                                                sizeof(pending_employee_name)
                                            );
                                        }
                                        else
                                        {
                                            /* =========================
                                             * REGISTRATION SUCCESS
                                             * ========================= */

                                            ESP_LOGI(
                                                TAG,
                                                "========================================"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "       REGISTRATION SUCCESS"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "========================================"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Face ID      : %d",
                                                new_face_id
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Employee ID  : %s",
                                                pending_employee_id
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Name         : %s",
                                                pending_employee_name
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Face DB      : SAVED"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Employee DB  : SAVED"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "Attendance   : NOW ENABLED"
                                            );

                                            ESP_LOGI(
                                                TAG,
                                                "========================================"
                                            );


                                            web_server_set_registration_status(
                                                "SUCCESS",
                                                new_face_id
                                            );


                                            enrollment_mode = false;

                                            enrollment_stable_detections = 0;


                                            memset(
                                                pending_employee_id,
                                                0,
                                                sizeof(pending_employee_id)
                                            );

                                            memset(
                                                pending_employee_name,
                                                0,
                                                sizeof(pending_employee_name)
                                            );
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    /* =================================================
                     * NORMAL ATTENDANCE MODE
                     * ================================================= */

                    if (
                        face_recognition_ready &&
                        face_found
                    )
                    {
                        if (
                            face_recognition_get_num_faces() <= 0
                        )
                        {
                            /* Waiting for phone registration. */
                        }
                        else
                        {
                            bool recognized =
                                face_recognition_process(
                                    fb
                                );


                            if (recognized)
                            {
                                int recognized_face_id =
                                    -1;

                                float recognized_similarity =
                                    0.0f;


                                bool result_available =
                                    face_recognition_get_last_result(
                                        &recognized_face_id,
                                        &recognized_similarity
                                    );


                                if (result_available)
                                {
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
                    }
                }
            }


            /* =================================================
             * RETURN CAMERA FRAME
             * ================================================= */

            camera_module_return_frame(
                fb
            );


            if (!display_ok)
            {
                ESP_LOGE(
                    TAG,
                    "TFT frame display failed"
                );
            }


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
            )
            {
                uint32_t elapsed_ms =
                    (now - fps_start) *
                    portTICK_PERIOD_MS;


                float fps =
                    (frame_count * 1000.0f) /
                    elapsed_ms;


                ESP_LOGI(
                    TAG,
                    "LIVE FPS: %.2f | Frames: %lu | Mode: %s",
                    fps,
                    (unsigned long)frame_count,
                    enrollment_mode
                        ? "ENROLLMENT"
                        : "ATTENDANCE"
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
            )
            {
                rtc_last_read = now;


                ds3231_datetime_t datetime;


                ret =
                    ds3231_get_datetime(
                        &datetime
                    );


                if (ret == ESP_OK)
                {
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
                }
                else
                {
                    ESP_LOGE(
                        TAG,
                        "RTC read failed: %s",
                        esp_err_to_name(ret)
                    );
                }
            }
        }
    }
    else
    {
        /* ====================================================
         * NO CAMERA
         * ==================================================== */

        while (1)
        {
            ESP_LOGW(
                TAG,
                "System running without camera"
            );


            if (rtc_ready)
            {
                ds3231_datetime_t datetime;


                ret =
                    ds3231_get_datetime(
                        &datetime
                    );


                if (ret == ESP_OK)
                {
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