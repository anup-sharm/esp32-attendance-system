#ifndef FACE_DETECTION_H
#define FACE_DETECTION_H

#include <stdbool.h>
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_DETECTION_MAX_RESULTS 5

typedef struct
{
    int box[4];
    float score;
} face_detection_result_t;


/*
 * Initialize face detector
 */
bool face_detection_init(void);


/*
 * Check whether detector is initialized
 */
bool face_detection_is_initialized(void);


/*
 * Run face detection on camera frame
 */
bool face_detection_process(const camera_fb_t *fb);


/*
 * Get number of detected faces
 */
int face_detection_get_count(void);


/*
 * Get all detected face results
 *
 * Returns number of copied results.
 */
int face_detection_get_results(
    face_detection_result_t *results,
    int max_results
);


/*
 * Get one detected face result
 */
bool face_detection_get_result(
    int index,
    face_detection_result_t *result
);


#ifdef __cplusplus
}
#endif

#endif