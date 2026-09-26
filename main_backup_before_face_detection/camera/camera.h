#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_module_init(void);

camera_fb_t *camera_module_capture(void);

void camera_module_return_frame(camera_fb_t *fb);

void camera_module_print_frame_info(camera_fb_t *fb);

bool camera_module_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif