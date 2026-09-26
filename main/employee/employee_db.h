#ifndef EMPLOYEE_DB_H
#define EMPLOYEE_DB_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
 * EMPLOYEE RECORD
 * ============================================================ */

typedef struct
{
    int face_id;

    char employee_id[32];

    char name[96];

} employee_record_t;


/* ============================================================
 * INITIALIZE EMPLOYEE DATABASE
 * ============================================================ */

esp_err_t employee_db_init(void);


/* ============================================================
 * CHECK INITIALIZATION STATUS
 * ============================================================ */

bool employee_db_is_initialized(void);


/* ============================================================
 * GET EMPLOYEE BY FACE ID
 *
 * Face ID
 *     ↓
 * Employee ID
 *     ↓
 * Employee Name
 * ============================================================ */

esp_err_t employee_db_get_by_face_id(
    int face_id,
    employee_record_t *employee
);


/* ============================================================
 * ADD / UPDATE EMPLOYEE MAPPING
 *
 * If Face ID does not exist:
 *     New mapping is added.
 *
 * If Face ID already exists:
 *     Existing mapping is updated.
 *
 * Example:
 *
 * Face ID     : 3
 * Employee ID : D02
 * Name        : Rahul Kumar
 *
 * Result in employees.csv:
 *
 * 3,D02,Rahul Kumar
 * ============================================================ */

esp_err_t employee_db_add_mapping(
    int face_id,
    const char *employee_id,
    const char *name
);


/* ============================================================
 * PRINT ALL EMPLOYEE RECORDS
 * ============================================================ */

esp_err_t employee_db_print_all(void);


#ifdef __cplusplus
}
#endif

#endif /* EMPLOYEE_DB_H */