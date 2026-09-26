#include "camera.h"

#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "CAMERA";
static bool camera_initialized = false;

#define CAM_PIN_PWDN       -1
#define CAM_PIN_RESET      -1

#define CAM_PIN_XCLK       15
#define CAM_PIN_SIOD       4
#define CAM_PIN_SIOC       5

#define CAM_PIN_D7         16
#define CAM_PIN_D6         17
#define CAM_PIN_D5         18
#define CAM_PIN_D4         12
#define CAM_PIN_D3         10
#define CAM_PIN_D2         8
#define CAM_PIN_D1         9
#define CAM_PIN_D0         11

#define CAM_PIN_VSYNC      6
#define CAM_PIN_HREF       7
#define CAM_PIN_PCLK       13

esp_err_t camera_module_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "       GC2145 CAMERA INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    camera_initialized = false;

    camera_config_t config = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,

        .pin_xclk       = CAM_PIN_XCLK,

        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,

        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,

        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        /*
         * Keep existing XCLK.
         * Do not change hardware timing unnecessarily.
         */
        .xclk_freq_hz   = 20000000,

        /*
         * RGB565 is required for direct TFT display.
         */
        .pixel_format   = PIXFORMAT_RGB565,

        /*
         * Keep QVGA.
         *
         * 320x240 -> rotated to 240x320 TFT.
         */
        .frame_size     = FRAMESIZE_QVGA,

        /*
         * JPEG quality has no effect on RGB565 frames,
         * but keeping the existing value preserves the
         * original configuration.
         */
        .jpeg_quality   = 12,

        /*
         * Two frame buffers in PSRAM.
         */
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,

        /*
         * Always use the latest frame for live preview.
         */
        .grab_mode      = CAMERA_GRAB_LATEST
    };

    ESP_LOGI(TAG, "Camera sensor: GC2145");
    ESP_LOGI(TAG, "Camera resolution: 320x240");
    ESP_LOGI(TAG, "Pixel format: RGB565");
    ESP_LOGI(TAG, "Frame buffer count: 2");
    ESP_LOGI(TAG, "Frame buffer location: PSRAM");
    ESP_LOGI(TAG, "Camera XCLK: 20 MHz");
    ESP_LOGI(TAG, "Initializing camera driver...");

    esp_err_t ret = esp_camera_init(&config);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GC2145 camera initialization FAILED! Error = 0x%x (%s)",
            ret,
            esp_err_to_name(ret)
        );

        camera_initialized = false;
        return ret;
    }

    sensor_t *sensor = esp_camera_sensor_get();

    if (sensor == NULL) {
        ESP_LOGE(TAG, "Camera sensor handle is NULL");

        esp_camera_deinit();

        camera_initialized = false;

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera sensor PID : 0x%04X", sensor->id.PID);
    ESP_LOGI(TAG, "Camera sensor VER : 0x%04X", sensor->id.VER);
    ESP_LOGI(TAG, "Camera sensor MIDL: 0x%04X", sensor->id.MIDL);
    ESP_LOGI(TAG, "Camera sensor MIDH: 0x%04X", sensor->id.MIDH);

    /*
     * --------------------------------------------------------
     * IMAGE ORIENTATION
     * --------------------------------------------------------
     *
     * Keep the complete camera field of view.
     *
     * No crop.
     * No digital zoom.
     */
    if (sensor->set_hmirror != NULL) {
        sensor->set_hmirror(sensor, 0);
    }

    if (sensor->set_vflip != NULL) {
        sensor->set_vflip(sensor, 0);
    }

    /*
     * --------------------------------------------------------
     * BASIC IMAGE SETTINGS
     * --------------------------------------------------------
     *
     * Keep these at neutral values.
     *
     * We are not using aggressive software image processing,
     * because that can make the live preview look artificial
     * or introduce additional processing load.
     */
    if (sensor->set_brightness != NULL) {
        sensor->set_brightness(sensor, 0);
    }

    if (sensor->set_contrast != NULL) {
        sensor->set_contrast(sensor, 0);
    }

    if (sensor->set_saturation != NULL) {
        sensor->set_saturation(sensor, 0);
    }

    camera_initialized = true;

    ESP_LOGI(TAG, "Camera format: RGB565");
    ESP_LOGI(TAG, "Camera resolution: 320x240");
    ESP_LOGI(TAG, "Frame buffers: 2");
    ESP_LOGI(TAG, "Frame buffer location: PSRAM");
    ESP_LOGI(TAG, "Full camera field of view preserved");

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "     GC2145 CAMERA INITIALIZATION PASSED");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

camera_fb_t *camera_module_capture(void)
{
    if (!camera_initialized) {
        ESP_LOGE(
            TAG,
            "Camera capture requested before successful initialization"
        );

        return NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera frame capture FAILED");
        return NULL;
    }

    return fb;
}

void camera_module_return_frame(camera_fb_t *fb)
{
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}

void camera_module_print_frame_info(camera_fb_t *fb)
{
    if (fb == NULL) {
        ESP_LOGE(TAG, "Frame is NULL");
        return;
    }

    ESP_LOGI(
        TAG,
        "Frame: %ux%u | format=%d | length=%u bytes",
        fb->width,
        fb->height,
        fb->format,
        (unsigned int)fb->len
    );
}

bool camera_module_is_initialized(void)
{
    return camera_initialized;
}