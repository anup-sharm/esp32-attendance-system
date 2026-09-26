#ifndef ATTENDANCE_H
#define ATTENDANCE_H

#include <stdbool.h>

#include "esp_err.h"

#include "employee_db.h"
#include "ds3231.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * ATTENDANCE RESULT
 * ============================================================ */

typedef enum
{
    ATTENDANCE_RESULT_ERROR = -1,

    /* No attendance yet today -> IN recorded */
    ATTENDANCE_RESULT_IN = 1,

    /* IN exists, OUT not yet recorded -> OUT recorded */
    ATTENDANCE_RESULT_OUT = 2,

    /* IN + OUT already exist -> ignore */
    ATTENDANCE_RESULT_ALREADY_COMPLETE = 3,

    /*
     * Compatibility aliases for existing main.c.
     */
    ATTENDANCE_RESULT_MARKED = ATTENDANCE_RESULT_IN,

    ATTENDANCE_RESULT_ALREADY_MARKED =
        ATTENDANCE_RESULT_ALREADY_COMPLETE

} attendance_result_t;


/* ============================================================
 * ATTENDANCE DATABASE INITIALIZATION
 * ============================================================ */

esp_err_t attendance_init(void);


/* ============================================================
 * MARK ATTENDANCE
 *
 * State machine:
 *
 * First valid recognition:
 *     IN
 *
 * Second valid recognition:
 *     OUT
 *
 * Third and later:
 *     IGNORE
 *
 * New calendar day:
 *     IN again
 * ============================================================ */

attendance_result_t attendance_mark(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
);


/* ============================================================
 * ATTENDANCE STATUS
 * ============================================================ */

bool attendance_is_initialized(void);


/* ============================================================
 * CHECK TODAY'S ATTENDANCE
 * ============================================================ */

bool attendance_has_in_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
);

bool attendance_has_out_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
);

bool attendance_is_complete_today(
    const employee_record_t *employee,
    const ds3231_datetime_t *datetime
);


/* ============================================================
 * PRINT ATTENDANCE DATABASE
 * ============================================================ */

esp_err_t attendance_print_all(void);


#ifdef __cplusplus
}
#endif

#endif