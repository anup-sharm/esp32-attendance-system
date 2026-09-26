#ifndef TFT_H
#define TFT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

esp_err_t tft_init(void);

void tft_clear(uint16_t color);

bool tft_display_camera_frame(
    const camera_fb_t *fb
);

#ifdef __cplusplus
}
#endif

#endif