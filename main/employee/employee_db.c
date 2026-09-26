#include "employee_db.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"


/* ============================================================
 * TAG
 * ============================================================ */

static const char *TAG = "EMPLOYEE_DB";


/* ============================================================
 * DATABASE PATH
 * ============================================================ */

#define EMPLOYEE_DB_PATH       "/spiflash/employees.csv"
#define EMPLOYEE_DB_TEMP_PATH  "/spiflash/employees.tmp"


/* ============================================================
 * DEFAULT EMPLOYEE
 * ============================================================ */

#define DEFAULT_FACE_ID        1
#define DEFAULT_EMPLOYEE_ID    "D01"
#define DEFAULT_EMPLOYEE_NAME  "Anup Kuumar Sharma"


/* ============================================================
 * DATABASE STATE
 * ============================================================ */

static bool employee_db_initialized = false;


/* ============================================================
 * INTERNAL HELPER
 * ============================================================ */

static void remove_newline(char *str)
{
    if (str == NULL) {
        return;
    }

    str[strcspn(str, "\r\n")] = '\0';
}


/* ============================================================
 * INTERNAL HELPER
 * ============================================================ */

static bool is_blank_string(const char *str)
{
    if (str == NULL) {
        return true;
    }

    while (*str != '\0') {

        if (!isspace((unsigned char)*str)) {
            return false;
        }

        str++;
    }

    return true;
}


/* ============================================================
 * CREATE DEFAULT DATABASE
 * ============================================================ */

static esp_err_t create_default_database(void)
{
    FILE *file = fopen(
        EMPLOYEE_DB_PATH,
        "w"
    );

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create employee database: %s",
            EMPLOYEE_DB_PATH
        );

        return ESP_FAIL;
    }


    fprintf(
        file,
        "face_id,employee_id,name\n"
    );


    fprintf(
        file,
        "%d,%s,%s\n",
        DEFAULT_FACE_ID,
        DEFAULT_EMPLOYEE_ID,
        DEFAULT_EMPLOYEE_NAME
    );


    fclose(file);


    ESP_LOGI(
        TAG,
        "Default employee database created"
    );


    ESP_LOGI(
        TAG,
        "Face ID     : %d",
        DEFAULT_FACE_ID
    );

    ESP_LOGI(
        TAG,
        "Employee ID : %s",
        DEFAULT_EMPLOYEE_ID
    );

    ESP_LOGI(
        TAG,
        "Name        : %s",
        DEFAULT_EMPLOYEE_NAME
    );


    return ESP_OK;
}


/* ============================================================
 * CHECK DATABASE FILE
 * ============================================================ */

static bool database_file_has_content(void)
{
    FILE *file = fopen(
        EMPLOYEE_DB_PATH,
        "r"
    );

    if (file == NULL) {
        return false;
    }


    int character;

    bool has_content = false;


    while ((character = fgetc(file)) != EOF) {

        if (!isspace((unsigned char)character)) {

            has_content = true;

            break;
        }
    }


    fclose(file);


    return has_content;
}


/* ============================================================
 * INITIALIZATION
 * ============================================================ */

esp_err_t employee_db_init(void)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       EMPLOYEE DATABASE INIT"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    employee_db_initialized = false;


    /*
     * Check if storage is mounted by attempting
     * to access the database location.
     */

    FILE *test_file = fopen(
        EMPLOYEE_DB_PATH,
        "r"
    );


    if (test_file != NULL) {

        fclose(test_file);

        ESP_LOGI(
            TAG,
            "Employee database file already exists"
        );

    } else {

        ESP_LOGW(
            TAG,
            "Employee database file does not exist"
        );


        ESP_LOGI(
            TAG,
            "Creating default employee database..."
        );


        esp_err_t ret =
            create_default_database();


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to create employee database"
            );

            return ret;
        }
    }


    /*
     * Make sure file is not empty.
     */

    if (!database_file_has_content()) {

        ESP_LOGW(
            TAG,
            "Employee database file is empty"
        );


        esp_err_t ret =
            create_default_database();


        if (ret != ESP_OK) {

            return ret;
        }
    }


    employee_db_initialized = true;


    ESP_LOGI(
        TAG,
        "Employee database initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "Database path: %s",
        EMPLOYEE_DB_PATH
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}


/* ============================================================
 * DATABASE STATUS
 * ============================================================ */

bool employee_db_is_initialized(void)
{
    return employee_db_initialized;
}


/* ============================================================
 * GET EMPLOYEE BY FACE ID
 * ============================================================ */

esp_err_t employee_db_get_by_face_id(
    int face_id,
    employee_record_t *employee
)
{
    if (!employee_db_initialized) {

        ESP_LOGE(
            TAG,
            "Employee database is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    if (employee == NULL) {

        ESP_LOGE(
            TAG,
            "Invalid employee output pointer"
        );

        return ESP_ERR_INVALID_ARG;
    }


    FILE *file = fopen(
        EMPLOYEE_DB_PATH,
        "r"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open employee database"
        );

        return ESP_FAIL;
    }


    char line[256];


    while (fgets(
        line,
        sizeof(line),
        file
    ) != NULL) {

        remove_newline(line);


        /*
         * Skip empty lines.
         */

        if (line[0] == '\0') {
            continue;
        }


        /*
         * Skip CSV header.
         */

        if (strncmp(
            line,
            "face_id",
            7
        ) == 0) {

            continue;
        }


        int parsed_face_id = -1;

        char parsed_employee_id[32] = {0};

        char parsed_name[96] = {0};


        int parsed = sscanf(
            line,
            "%d,%31[^,],%95[^\r\n]",
            &parsed_face_id,
            parsed_employee_id,
            parsed_name
        );


        if (parsed == 3) {

            if (parsed_face_id == face_id) {

                employee->face_id =
                    parsed_face_id;


                strncpy(
                    employee->employee_id,
                    parsed_employee_id,
                    sizeof(employee->employee_id) - 1
                );

                employee->employee_id[
                    sizeof(employee->employee_id) - 1
                ] = '\0';


                strncpy(
                    employee->name,
                    parsed_name,
                    sizeof(employee->name) - 1
                );

                employee->name[
                    sizeof(employee->name) - 1
                ] = '\0';


                fclose(file);


                ESP_LOGI(
                    TAG,
                    "Employee mapping found"
                );


                ESP_LOGI(
                    TAG,
                    "Face ID     : %d",
                    employee->face_id
                );

                ESP_LOGI(
                    TAG,
                    "Employee ID : %s",
                    employee->employee_id
                );

                ESP_LOGI(
                    TAG,
                    "Name        : %s",
                    employee->name
                );


                return ESP_OK;
            }
        }
    }


    fclose(file);


    ESP_LOGW(
        TAG,
        "No employee mapping found for Face ID: %d",
        face_id
    );


    return ESP_ERR_NOT_FOUND;
}


/* ============================================================
 * ADD / UPDATE EMPLOYEE MAPPING
 * ============================================================ */

esp_err_t employee_db_add_mapping(
    int face_id,
    const char *employee_id,
    const char *name
)
{
    if (!employee_db_initialized) {

        ESP_LOGE(
            TAG,
            "Cannot add mapping: database not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    if (face_id < 0) {

        ESP_LOGE(
            TAG,
            "Invalid Face ID: %d",
            face_id
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (employee_id == NULL ||
        name == NULL) {

        ESP_LOGE(
            TAG,
            "Employee ID or name is NULL"
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (is_blank_string(employee_id) ||
        is_blank_string(name)) {

        ESP_LOGE(
            TAG,
            "Employee ID or name cannot be empty"
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (strchr(employee_id, ',') != NULL ||
        strchr(name, ',') != NULL) {

        ESP_LOGE(
            TAG,
            "Comma is not allowed in Employee ID or Name"
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (strlen(employee_id) >= 32) {

        ESP_LOGE(
            TAG,
            "Employee ID is too long"
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (strlen(name) >= 96) {

        ESP_LOGE(
            TAG,
            "Employee name is too long"
        );

        return ESP_ERR_INVALID_ARG;
    }


    FILE *source = fopen(
        EMPLOYEE_DB_PATH,
        "r"
    );


    if (source == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open existing employee database"
        );

        return ESP_FAIL;
    }


    FILE *temp = fopen(
        EMPLOYEE_DB_TEMP_PATH,
        "w"
    );


    if (temp == NULL) {

        fclose(source);


        ESP_LOGE(
            TAG,
            "Failed to create temporary employee database"
        );

        return ESP_FAIL;
    }


    /*
     * Always write a clean CSV header.
     */

    fprintf(
        temp,
        "face_id,employee_id,name\n"
    );


    char line[256];

    bool mapping_found = false;


    while (fgets(
        line,
        sizeof(line),
        source
    ) != NULL) {

        remove_newline(line);


        if (line[0] == '\0') {
            continue;
        }


        /*
         * Skip old header.
         */

        if (strncmp(
            line,
            "face_id",
            7
        ) == 0) {

            continue;
        }


        int existing_face_id = -1;

        char existing_employee_id[32] = {0};

        char existing_name[96] = {0};


        int parsed = sscanf(
            line,
            "%d,%31[^,],%95[^\r\n]",
            &existing_face_id,
            existing_employee_id,
            existing_name
        );


        if (parsed != 3) {

            ESP_LOGW(
                TAG,
                "Skipping invalid CSV line: %s",
                line
            );

            continue;
        }


        /*
         * If this Face ID already exists,
         * replace its mapping.
         */

        if (existing_face_id == face_id) {

            mapping_found = true;

            continue;
        }


        /*
         * Preserve existing mapping.
         */

        fprintf(
            temp,
            "%d,%s,%s\n",
            existing_face_id,
            existing_employee_id,
            existing_name
        );
    }


    /*
     * Add the new / updated mapping.
     */

    fprintf(
        temp,
        "%d,%s,%s\n",
        face_id,
        employee_id,
        name
    );


    fflush(temp);

    fclose(temp);

    fclose(source);


    /*
     * Remove old database.
     */

    if (remove(EMPLOYEE_DB_PATH) != 0) {

        ESP_LOGE(
            TAG,
            "Failed to remove old employee database"
        );

        remove(EMPLOYEE_DB_TEMP_PATH);

        return ESP_FAIL;
    }


    /*
     * Rename temporary database.
     */

    if (rename(
        EMPLOYEE_DB_TEMP_PATH,
        EMPLOYEE_DB_PATH
    ) != 0) {

        ESP_LOGE(
            TAG,
            "Failed to rename temporary employee database"
        );

        return ESP_FAIL;
    }


    if (mapping_found) {

        ESP_LOGI(
            TAG,
            "Existing employee mapping updated"
        );

    } else {

        ESP_LOGI(
            TAG,
            "New employee mapping added"
        );
    }


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "       EMPLOYEE MAPPING SAVED"
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
        name
    );

    ESP_LOGI(
        TAG,
        "Database    : %s",
        EMPLOYEE_DB_PATH
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}


/* ============================================================
 * PRINT ALL EMPLOYEES
 * ============================================================ */

esp_err_t employee_db_print_all(void)
{
    if (!employee_db_initialized) {

        ESP_LOGW(
            TAG,
            "Employee database is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }


    FILE *file = fopen(
        EMPLOYEE_DB_PATH,
        "r"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open employee database"
        );

        return ESP_FAIL;
    }


    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "      EMPLOYEE DATABASE"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );


    char line[256];


    while (fgets(
        line,
        sizeof(line),
        file
    ) != NULL) {

        remove_newline(line);


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
        "======================================"
    );


    return ESP_OK;
}