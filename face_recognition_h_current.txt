#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
 * RECOGNITION RESULT
 * ============================================================ */

typedef struct
{
    int face_id;
    float similarity;

} face_recognition_result_t;


/* ============================================================
 * FACE RECOGNITION INITIALIZATION
 * ============================================================ */

esp_err_t face_recognition_init(void);


/* ============================================================
 * FACE RECOGNITION PROCESS
 * ============================================================ */

bool face_recognition_process(
    const camera_fb_t *fb
);


/* ============================================================
 * FACE ENROLLMENT
 * ============================================================ */

bool face_recognition_enroll(
    const camera_fb_t *fb
);


/* ============================================================
 * RECOGNITION STATUS
 * ============================================================ */

bool face_recognition_is_initialized(void);


/* ============================================================
 * ENROLLED FACE COUNT
 * ============================================================ */

int face_recognition_get_num_faces(void);


/* ============================================================
 * LAST ENROLLED FACE ID
 *
 * Returns the Face ID assigned to the most recently enrolled
 * face.
 *
 * Returns:
 *
 * true  -> valid Face ID available
 * false -> no valid enrollment available
 * ============================================================ */

bool face_recognition_get_last_enrolled_face_id(
    int *face_id
);


/* ============================================================
 * LAST RECOGNITION RESULT
 * ============================================================ */

bool face_recognition_get_last_result(
    int *face_id,
    float *similarity
);


/* ============================================================
 * MULTIPLE RECOGNITION RESULTS
 * ============================================================
 *
 * Returns the number of recognized faces from the most recent
 * recognition operation.
 *
 * The results are stored internally by the recognition module.
 *
 * max_results:
 *     Maximum number of results to copy.
 *
 * results:
 *     Destination array.
 *
 * Returns:
 *     Number of results copied.
 * ============================================================ */

int face_recognition_get_results(
    face_recognition_result_t *results,
    int max_results
);


/* ============================================================
 * RECOGNITION THRESHOLD
 * ============================================================
 *
 * Returns the minimum similarity required by the application
 * before a recognition result is accepted.
 * ============================================================ */

float face_recognition_get_application_threshold(void);


#ifdef __cplusplus
}
#endif

#endif