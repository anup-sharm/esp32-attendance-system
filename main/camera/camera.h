#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif


esp_err_t camera_module_init(void);


/*
 * Returns only a complete valid:
 *
 * 320x240
 * RGB565
 * 153600-byte
 *
 * frame.
 *
 * Invalid/partial frames return NULL.
 */
camera_fb_t *camera_module_capture(void);


/*
 * Return frame to camera driver.
 */
void camera_module_return_frame(camera_fb_t *fb);


/*
 * Print frame information.
 */
void camera_module_print_frame_info(camera_fb_t *fb);


/*
 * Camera initialization status.
 */
bool camera_module_is_initialized(void);


#ifdef __cplusplus
}
#endif

#endif