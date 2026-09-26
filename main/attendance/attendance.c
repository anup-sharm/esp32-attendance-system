#include "attendance.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "storage.h"


static const char *TAG = "ATTENDANCE";


/* ============================================================
 * ATTENDANCE DATABASE PATH
 * ============================================================ */

#define ATTENDANCE_DB_PATH "/spiflash/attendance.csv"


/* ============================================================
 * DATABASE STATUS
 * ============================================================ */

static bool attendance_initialized = false;


/* ============================================================
 * CREATE DATE STRING
 * ============================================================ */

static void attendance_make_date(
    const ds3231_datetime_t *datetime,
    char *date_string,
    size_t date_size
)
{
    snprintf(
        date_string,
        date_size,
        "%04u-%02u-%02u",
        (unsigned int)datetime->year,
        (unsigned int)datetime->month,
        (unsigned int)datetime->day
    );
}


/* ============================================================
 * CREATE TIME STRING
 * ============================================================ */

static void attendance_make_time(
    const ds3231_datetime_t *datetime,
    char *time_string,
    size_t time_size
)
{
    snprintf(
        time_string,
        time_size,
        "%02u:%02u:%02u",
        (unsigned int)datetime->hour,
        (unsigned int)datetime->minute,
        (unsigned int)datetime->second
    );
}


/* ============================================================
 * CHECK ATTENDANCE FILE
 * ============================================================ */

static bool attendance_file_has_data(void)
{
    FILE *file =
        fopen(
            ATTENDANCE_DB_PATH,
            "r"
        );

    if (file == NULL) {
        return false;
    }

    int character = fgetc(file);

    fclose(file);

    if (character == EOF) {
        return false;
    }

    return true;
}


/* ============================================================
 * CREATE ATTENDANCE FILE
 * ============================================================ */

static esp_err_t attendance_create_file(void)
{
    ESP_LOGI(
        TAG,
        "Creating attendance database..."
    );

    FILE *file =
        fopen(
            ATTENDANCE_DB_PATH,
            "w"
        );

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create: %s",
            ATTENDANCE_DB_PATH
        );

        return ESP_FAIL;
    }

    fprintf(
        file,
        "employee_id,name,date,time,status\n"
    );

    fflush(file);

    fclose(file);

    ESP_LOGI(
        TAG,
        "Attendance database created"
    );

    return ESP_OK;
}


/* ============================================================
 * ATTENDANCE INITIALIZATION
 * ============================================================ */

esp_err_t attendance_init(void)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       ATTENDANCE INITIALIZATION"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    attendance_initialized = false;


    /* --------------------------------------------------------
     * CHECK STORAGE
     * -------------------------------------------------------- */

    if (!storage_is_mounted()) {

        ESP_LOGE(
            TAG,
            "SPI flash storage is not mounted"
        );

        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(
        TAG,
        "Attendance path: %s",
        ATTENDANCE_DB_PATH
    );


    /* --------------------------------------------------------
     * CHECK EXISTING FILE
     * -------------------------------------------------------- */

    if (!attendance_file_has_data()) {

        ESP_LOGW(
            TAG,
            "Attendance database does not exist or is empty"
        );

        esp_err_t ret =
            attendance_create_file();

        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to create attendance database"
            );

            return ret;
        }
    }
    else {

        ESP_LOGI(
            TAG,
            "Existing attendance database found"
        );
    }


    attendance_initialized = true;


    ESP_LOGI(
        TAG,
        "Attendance initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    return ESP_OK;
}


/* ============================================================
 * READ TODAY'S EMPLOYEE STATE
 *
 * has_in  = employee has IN today
 * has_out = employee has OUT today
 *
 * IMPORTANT:
 * CSV parser is corrected here.
 * ============================================================ */

static bool attendance_get_today_state(
    const employee_record_t *employee,
    const char *date_string,
    bool *has_in,
    bool *has_out
)
{
    if (
        employee == NULL ||
        date_string == NULL ||
        has_in == NULL ||
        has_out == NULL
    ) {
        return false;
    }


    *has_in = false;
    *has_out = false;


    FILE *file =
        fopen(
            ATTENDANCE_DB_PATH,
            "r"
        );

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open attendance database"
        );

        return false;
    }


    char line[256];


    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        /* ----------------------------------------------------
         * Skip header
         * ---------------------------------------------------- */

        if (
            strncmp(
                line,
                "employee_id",
                11
            ) == 0
        ) {
            continue;
        }


        /* ----------------------------------------------------
         * Parsed CSV fields
         * ---------------------------------------------------- */

        char parsed_employee_id[32] = {0};
        char parsed_name[96] = {0};
        char parsed_date[16] = {0};
        char parsed_time[16] = {0};
        char parsed_status[32] = {0};


        /*
         * CORRECT CSV FORMAT
         *
         * employee_id,name,date,time,status
         */
        int parsed =
            sscanf(
                line,
                "%31[^,],%95[^,],%15[^,],%15[^,],%31[^\r\n]",
                parsed_employee_id,
                parsed_name,
                parsed_date,
                parsed_time,
                parsed_status
            );


        if (parsed != 5) {

            ESP_LOGW(
                TAG,
                "Ignoring invalid attendance line: %s",
                line
            );

            continue;
        }


        /* ----------------------------------------------------
         * Employee check
         * ---------------------------------------------------- */

        if (
            strcmp(
                parsed_employee_id,
                employee->employee_id
            ) != 0
        ) {
            continue;
        }


        /* ----------------------------------------------------
         * Date check
         * ---------------------------------------------------- */

        if (
            strcmp(
                parsed_date,
                date_string
            ) != 0
        ) {
            continue;
        }


        /* ----------------------------------------------------
         * STATUS CHECK
         * ---------------------------------------------------- */

        if (
            strcmp(
                parsed_status,
                "IN"
            ) == 0
        ) {

            *has_in = true;
        }


        if (
            strcmp(
                parsed_status,
                "OUT"
            ) == 0
        ) {

            *has_out = true;
        }
    }


    fclose(file);

    return true;
}


/* ============================================================
 * CHECK IN TODAY
 * ============================================================ */

bool attendance_has_in_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
)
{
    if (
        employee == NULL ||
        datetime == NULL
    ) {
        return false;
    }


    char date_string[16];

    attendance_make_date(
        datetime,
        date_string,
        sizeof(date_string)
    );


    bool has_in = false;
    bool has_out = false;


    if (
        !attendance_get_today_state(
            employee,
            date_string,
            &has_in,
            &has_out
        )
    ) {
        return false;
    }


    return has_in;
}


/* ============================================================
 * CHECK OUT TODAY
 * ============================================================ */

bool attendance_has_out_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
)
{
    if (
        employee == NULL ||
        datetime == NULL
    ) {
        return false;
    }


    char date_string[16];

    attendance_make_date(
        datetime,
        date_string,
        sizeof(date_string)
    );


    bool has_in = false;
    bool has_out = false;


    if (
        !attendance_get_today_state(
            employee,
            date_string,
            &has_in,
            &has_out
        )
    ) {
        return false;
    }


    return has_out;
}


/* ============================================================
 * CHECK COMPLETE TODAY
 * ============================================================ */

bool attendance_is_complete_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
)
{
    if (
        employee == NULL ||
        datetime == NULL
    ) {
        return false;
    }


    char date_string[16];

    attendance_make_date(
        datetime,
        date_string,
        sizeof(date_string)
    );


    bool has_in = false;
    bool has_out = false;


    if (
        !attendance_get_today_state(
            employee,
            date_string,
            &has_in,
            &has_out
        )
    ) {
        return false;
    }


    return (
        has_in &&
        has_out
    );
}


/* ============================================================
 * APPEND ATTENDANCE RECORD
 * ============================================================ */

static bool attendance_append_record(
    const employee_record_t *employee,
    const char *date_string,
    const char *time_string,
    const char *status
)
{
    FILE *file =
        fopen(
            ATTENDANCE_DB_PATH,
            "a"
        );

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open attendance database for writing"
        );

        return false;
    }


    int written =
        fprintf(
            file,
            "%s,%s,%s,%s,%s\n",
            employee->employee_id,
            employee->name,
            date_string,
            time_string,
            status
        );


    fflush(file);

    fclose(file);


    if (written < 0) {

        ESP_LOGE(
            TAG,
            "Failed to write attendance record"
        );

        return false;
    }


    return true;
}


/* ============================================================
 * MARK ATTENDANCE
 *
 * FIRST SCAN:
 *     IN
 *
 * SECOND SCAN:
 *     OUT
 *
 * THIRD+ SCAN:
 *     IGNORE
 *
 * NEXT DATE:
 *     NEW IN
 * ============================================================ */

attendance_result_t attendance_mark(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
)
{
    /* --------------------------------------------------------
     * INITIALIZATION
     * -------------------------------------------------------- */

    if (!attendance_initialized) {

        ESP_LOGE(
            TAG,
            "Attendance system is not initialized"
        );

        return ATTENDANCE_RESULT_ERROR;
    }


    /* --------------------------------------------------------
     * EMPLOYEE
     * -------------------------------------------------------- */

    if (employee == NULL) {

        ESP_LOGE(
            TAG,
            "Employee pointer is NULL"
        );

        return ATTENDANCE_RESULT_ERROR;
    }


    /* --------------------------------------------------------
     * DATETIME
     * -------------------------------------------------------- */

    if (datetime == NULL) {

        ESP_LOGE(
            TAG,
            "Date/time pointer is NULL"
        );

        return ATTENDANCE_RESULT_ERROR;
    }


    /* --------------------------------------------------------
     * EMPLOYEE ID
     * -------------------------------------------------------- */

    if (employee->employee_id[0] == '\0') {

        ESP_LOGE(
            TAG,
            "Employee ID is empty"
        );

        return ATTENDANCE_RESULT_ERROR;
    }


    /* --------------------------------------------------------
     * DATE
     * -------------------------------------------------------- */

    char date_string[16];

    attendance_make_date(
        datetime,
        date_string,
        sizeof(date_string)
    );


    /* --------------------------------------------------------
     * TIME
     * -------------------------------------------------------- */

    char time_string[16];

    attendance_make_time(
        datetime,
        time_string,
        sizeof(time_string)
    );


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "ATTENDANCE REQUEST"
    );

    ESP_LOGI(
        TAG,
        "Employee ID: %s",
        employee->employee_id
    );

    ESP_LOGI(
        TAG,
        "Name       : %s",
        employee->name
    );

    ESP_LOGI(
        TAG,
        "Date       : %s",
        date_string
    );

    ESP_LOGI(
        TAG,
        "Time       : %s",
        time_string
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    /* --------------------------------------------------------
     * GET TODAY'S STATE
     * -------------------------------------------------------- */

    bool has_in = false;
    bool has_out = false;


    if (
        !attendance_get_today_state(
            employee,
            date_string,
            &has_in,
            &has_out
        )
    ) {

        ESP_LOGE(
            TAG,
            "Could not read today's attendance state"
        );

        return ATTENDANCE_RESULT_ERROR;
    }


    /* ========================================================
     * STATE 1
     *
     * NOTHING TODAY
     *
     * -> CREATE IN
     * ======================================================== */

    if (!has_in) {

        if (
            !attendance_append_record(
                employee,
                date_string,
                time_string,
                "IN"
            )
        ) {
            return ATTENDANCE_RESULT_ERROR;
        }


        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "ATTENDANCE IN RECORDED"
        );

        ESP_LOGI(
            TAG,
            "Employee ID: %s",
            employee->employee_id
        );

        ESP_LOGI(
            TAG,
            "Name: %s",
            employee->name
        );

        ESP_LOGI(
            TAG,
            "Date: %s",
            date_string
        );

        ESP_LOGI(
            TAG,
            "Time: %s",
            time_string
        );

        ESP_LOGI(
            TAG,
            "Status: IN"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );


        return ATTENDANCE_RESULT_IN;
    }


    /* ========================================================
     * STATE 2
     *
     * IN EXISTS
     * OUT DOES NOT EXIST
     *
     * -> CREATE OUT
     * ======================================================== */

    if (
        has_in &&
        !has_out
    ) {

        if (
            !attendance_append_record(
                employee,
                date_string,
                time_string,
                "OUT"
            )
        ) {
            return ATTENDANCE_RESULT_ERROR;
        }


        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "ATTENDANCE OUT RECORDED"
        );

        ESP_LOGI(
            TAG,
            "Employee ID: %s",
            employee->employee_id
        );

        ESP_LOGI(
            TAG,
            "Name: %s",
            employee->name
        );

        ESP_LOGI(
            TAG,
            "Date: %s",
            date_string
        );

        ESP_LOGI(
            TAG,
            "Time: %s",
            time_string
        );

        ESP_LOGI(
            TAG,
            "Status: OUT"
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );


        return ATTENDANCE_RESULT_OUT;
    }


    /* ========================================================
     * STATE 3
     *
     * IN + OUT ALREADY EXIST
     *
     * -> IGNORE
     * ======================================================== */

    if (
        has_in &&
        has_out
    ) {

        ESP_LOGW(
            TAG,
            "========================================"
        );

        ESP_LOGW(
            TAG,
            "ATTENDANCE ALREADY COMPLETE"
        );

        ESP_LOGW(
            TAG,
            "Employee ID: %s",
            employee->employee_id
        );

        ESP_LOGW(
            TAG,
            "Name: %s",
            employee->name
        );

        ESP_LOGW(
            TAG,
            "Date: %s",
            date_string
        );

        ESP_LOGW(
            TAG,
            "IN + OUT already exist"
        );

        ESP_LOGW(
            TAG,
            "SCAN IGNORED"
        );

        ESP_LOGW(
            TAG,
            "========================================"
        );


        return ATTENDANCE_RESULT_ALREADY_COMPLETE;
    }


    return ATTENDANCE_RESULT_ERROR;
}


/* ============================================================
 * ATTENDANCE STATUS
 * ============================================================ */

bool attendance_is_initialized(void)
{
    return attendance_initialized;
}


/* ============================================================
 * PRINT ATTENDANCE DATABASE
 * ============================================================ */

esp_err_t attendance_print_all(void)
{
    if (!attendance_initialized) {

        ESP_LOGE(
            TAG,
            "Attendance system is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    FILE *file =
        fopen(
            ATTENDANCE_DB_PATH,
            "r"
        );

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open attendance database"
        );

        return ESP_FAIL;
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "        ATTENDANCE DATABASE"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    char line[256];


    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        line[
            strcspn(
                line,
                "\r\n"
            )
        ] = '\0';


        if (line[0] == '\0') {
            continue;
        }


        ESP_LOGI(
            TAG,
            "%s",
            line
        );
    }


    fclose(file);


    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}