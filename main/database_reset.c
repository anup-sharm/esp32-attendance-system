#include "database_reset.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "DATABASE_RESET";

#define STORAGE_PATH        "/spiflash"
#define VERSION_FILE        "/spiflash/.attendance_db_version"

#define FACE_DB_FILE        "/spiflash/face.db"
#define EMPLOYEE_DB_FILE    "/spiflash/employees.csv"
#define ATTENDANCE_DB_FILE  "/spiflash/attendance.csv"

static bool file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static esp_err_t delete_file_if_exists(const char *path)
{
    if (!file_exists(path)) {
        return ESP_OK;
    }

    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "Failed to delete: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted: %s", path);

    return ESP_OK;
}

static int read_database_version(void)
{
    FILE *f = fopen(VERSION_FILE, "r");

    if (!f) {
        return 0;
    }

    int version = 0;

    if (fscanf(f, "%d", &version) != 1) {
        version = 0;
    }

    fclose(f);

    return version;
}

static esp_err_t write_database_version(void)
{
    FILE *f = fopen(VERSION_FILE, "w");

    if (!f) {
        ESP_LOGE(TAG, "Cannot create database version file");
        return ESP_FAIL;
    }

    fprintf(f, "%d\n", DATABASE_RESET_VERSION);

    fclose(f);

    return ESP_OK;
}

static esp_err_t create_empty_employee_db(void)
{
    FILE *f = fopen(EMPLOYEE_DB_FILE, "w");

    if (!f) {
        ESP_LOGE(TAG, "Cannot create employees.csv");
        return ESP_FAIL;
    }

    /*
     * Empty database.
     *
     * Header only.
     * No employee.
     * No Face ID mapping.
     */
    fprintf(f, "face_id,employee_id,name\n");

    fclose(f);

    return ESP_OK;
}

static esp_err_t create_empty_attendance_db(void)
{
    FILE *f = fopen(ATTENDANCE_DB_FILE, "w");

    if (!f) {
        ESP_LOGE(TAG, "Cannot create attendance.csv");
        return ESP_FAIL;
    }

    /*
     * Empty attendance database.
     *
     * Header only.
     * No IN.
     * No OUT.
     * No PRESENT.
     */
    fprintf(
        f,
        "employee_id,name,date,time,status\n"
    );

    fclose(f);

    return ESP_OK;
}

esp_err_t database_reset_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DATABASE INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    /*
     * Storage must already be mounted before
     * this function is called.
     */

    if (!file_exists(STORAGE_PATH)) {
        ESP_LOGE(
            TAG,
            "Storage path does not exist: %s",
            STORAGE_PATH
        );

        return ESP_ERR_NOT_FOUND;
    }

    int current_version = read_database_version();

    ESP_LOGI(
        TAG,
        "Stored database version: %d",
        current_version
    );

    ESP_LOGI(
        TAG,
        "Required database version: %d",
        DATABASE_RESET_VERSION
    );

    /*
     * IMPORTANT:
     *
     * Version 2 is a one-time migration.
     *
     * Existing:
     *
     *   face.db
     *   employees.csv
     *   attendance.csv
     *
     * are deleted only when the stored version is
     * older than version 2.
     *
     * Therefore the database will NOT be erased
     * on every reboot.
     */

    if (current_version < DATABASE_RESET_VERSION) {

        ESP_LOGW(TAG, "OLD DATABASE DETECTED");
        ESP_LOGW(TAG, "PERFORMING ONE-TIME DATABASE RESET");

        /*
         * ----------------------------------------
         * FACE DATABASE
         * ----------------------------------------
         *
         * This is critical.
         *
         * HumanFaceRecognizer must initialize
         * after this deletion.
         *
         * Otherwise old Face IDs will return.
         */
        if (delete_file_if_exists(FACE_DB_FILE) != ESP_OK) {
            return ESP_FAIL;
        }

        /*
         * ----------------------------------------
         * EMPLOYEE DATABASE
         * ----------------------------------------
         */
        if (delete_file_if_exists(EMPLOYEE_DB_FILE) != ESP_OK) {
            return ESP_FAIL;
        }

        /*
         * ----------------------------------------
         * ATTENDANCE DATABASE
         * ----------------------------------------
         */
        if (delete_file_if_exists(ATTENDANCE_DB_FILE) != ESP_OK) {
            return ESP_FAIL;
        }

        /*
         * Create clean empty CSV files.
         */

        if (create_empty_employee_db() != ESP_OK) {
            return ESP_FAIL;
        }

        if (create_empty_attendance_db() != ESP_OK) {
            return ESP_FAIL;
        }

        /*
         * face.db is intentionally NOT created here.
         *
         * HumanFaceRecognizer will create it when
         * enrollment happens.
         */

        if (write_database_version() != ESP_OK) {
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "DATABASE RESET COMPLETE");
        ESP_LOGI(TAG, "========================================");

        ESP_LOGI(TAG, "face.db       : EMPTY / NEW");
        ESP_LOGI(TAG, "employees.csv : EMPTY");
        ESP_LOGI(TAG, "attendance.csv: EMPTY");
        ESP_LOGI(TAG, "Face IDs      : 0");
        ESP_LOGI(TAG, "Employees     : 0");
        ESP_LOGI(TAG, "Attendance    : 0");

        ESP_LOGI(TAG, "========================================");

        return ESP_OK;
    }

    /*
     * Database already migrated.
     *
     * Do NOT erase anything.
     */

    ESP_LOGI(TAG, "Database already initialized.");
    ESP_LOGI(TAG, "Existing registrations will be preserved.");

    return ESP_OK;
}