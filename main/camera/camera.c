#include "camera.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_camera.h"

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

/*
 * GC2145
 *
 * IMPORTANT:
 *
 * Camera output:
 *     320 x 240
 *     RGB565
 *
 * Expected frame size:
 *
 *     320 * 240 * 2
 *     = 153600 bytes
 *
 * We intentionally use ONE frame buffer first.
 *
 * This reduces PSRAM/DMA pressure and avoids the
 * continuous EV-EOF-OVF condition seen with the
 * previous double-buffer configuration.
 */

#define CAMERA_FRAME_WIDTH       320
#define CAMERA_FRAME_HEIGHT      240
#define CAMERA_EXPECTED_SIZE    \
    (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2)


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
         * ----------------------------------------------------
         * CAMERA CLOCK
         * ----------------------------------------------------
         *
         * Previous:
         *
         *     20 MHz
         *
         * We start with 10 MHz to reduce the camera/DMA
         * timing pressure which was producing:
         *
         *     EV-EOF-OVF
         *
         * If the sensor remains stable, this is a safer
         * configuration for the current RGB565 pipeline.
         */
        .xclk_freq_hz   = 10000000,

        /*
         * RGB565 is required by:
         *
         * 1. Face detector
         * 2. Face recognition
         * 3. Direct TFT preview
         */
        .pixel_format   = PIXFORMAT_RGB565,

        /*
         * QVGA
         *
         * 320 x 240
         */
        .frame_size     = FRAMESIZE_QVGA,

        /*
         * JPEG quality is irrelevant for RGB565,
         * but keep a valid value.
         */
        .jpeg_quality   = 12,

        /*
         * ----------------------------------------------------
         * FRAME BUFFER
         * ----------------------------------------------------
         *
         * IMPORTANT CHANGE:
         *
         * Previous:
         *
         *     fb_count = 2
         *     GRAB_LATEST
         *
         * New:
         *
         *     fb_count = 1
         *     GRAB_WHEN_EMPTY
         *
         * This reduces PSRAM and DMA pressure.
         */
        .fb_count       = 1,
        .fb_location    = CAMERA_FB_IN_PSRAM,

        /*
         * Do not continuously overwrite buffers.
         *
         * Capture one complete frame, process it,
         * return it, then capture the next frame.
         */
        .grab_mode      = CAMERA_GRAB_WHEN_EMPTY
    };


    ESP_LOGI(TAG, "Camera sensor       : GC2145");
    ESP_LOGI(TAG, "Resolution           : 320x240");
    ESP_LOGI(TAG, "Pixel format         : RGB565");
    ESP_LOGI(TAG, "Expected frame size  : %d bytes",
             CAMERA_EXPECTED_SIZE);

    ESP_LOGI(TAG, "Frame buffer count   : 1");
    ESP_LOGI(TAG, "Frame buffer         : PSRAM");
    ESP_LOGI(TAG, "Grab mode            : WHEN_EMPTY");
    ESP_LOGI(TAG, "Camera XCLK          : 10 MHz");

    ESP_LOGI(TAG, "Initializing camera driver...");


    esp_err_t ret = esp_camera_init(&config);

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GC2145 camera initialization FAILED!"
        );

        ESP_LOGE(
            TAG,
            "Error = 0x%x (%s)",
            ret,
            esp_err_to_name(ret)
        );

        camera_initialized = false;

        return ret;
    }


    sensor_t *sensor = esp_camera_sensor_get();

    if (sensor == NULL) {

        ESP_LOGE(
            TAG,
            "Camera sensor handle is NULL"
        );

        esp_camera_deinit();

        camera_initialized = false;

        return ESP_FAIL;
    }


    ESP_LOGI(
        TAG,
        "Camera sensor PID : 0x%04X",
        sensor->id.PID
    );

    ESP_LOGI(
        TAG,
        "Camera sensor VER : 0x%04X",
        sensor->id.VER
    );

    ESP_LOGI(
        TAG,
        "Camera sensor MIDL: 0x%04X",
        sensor->id.MIDL
    );

    ESP_LOGI(
        TAG,
        "Camera sensor MIDH: 0x%04X",
        sensor->id.MIDH
    );


    /*
     * --------------------------------------------------------
     * SENSOR IMAGE ORIENTATION
     * --------------------------------------------------------
     *
     * Keep the full camera field of view.
     *
     * TFT rotation is handled separately in tft.c.
     */
    if (sensor->set_hmirror != NULL) {
        sensor->set_hmirror(sensor, 0);
    }

    if (sensor->set_vflip != NULL) {
        sensor->set_vflip(sensor, 0);
    }


    /*
     * --------------------------------------------------------
     * IMAGE SETTINGS
     * --------------------------------------------------------
     *
     * Keep neutral values.
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


    /*
     * --------------------------------------------------------
     * VERIFY SENSOR CONFIGURATION
     * --------------------------------------------------------
     */
    ESP_LOGI(
        TAG,
        "Final sensor configuration:"
    );

    ESP_LOGI(
        TAG,
        "  resolution = 320x240"
    );

    ESP_LOGI(
        TAG,
        "  pixel format = RGB565"
    );

    ESP_LOGI(
        TAG,
        "  expected frame = %d bytes",
        CAMERA_EXPECTED_SIZE
    );

    ESP_LOGI(
        TAG,
        "  frame buffers = 1"
    );

    ESP_LOGI(
        TAG,
        "  frame buffer location = PSRAM"
    );


    camera_initialized = true;


    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "     GC2145 CAMERA INITIALIZATION PASSED");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}


/*
 * ============================================================
 * CAMERA CAPTURE
 * ============================================================
 */
camera_fb_t *camera_module_capture(void)
{
    if (!camera_initialized) {

        ESP_LOGE(
            TAG,
            "Camera capture requested before initialization"
        );

        return NULL;
    }


    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {

        ESP_LOGE(
            TAG,
            "Camera frame capture FAILED"
        );

        return NULL;
    }


    /*
     * --------------------------------------------------------
     * FRAME VALIDATION
     * --------------------------------------------------------
     *
     * RGB565 320x240 must always be:
     *
     * 153600 bytes
     *
     * Never send a partial frame to:
     *
     * - face detector
     * - face recognizer
     * - TFT
     */
    if (fb->width != CAMERA_FRAME_WIDTH ||
        fb->height != CAMERA_FRAME_HEIGHT ||
        fb->format != PIXFORMAT_RGB565 ||
        fb->len != CAMERA_EXPECTED_SIZE) {

        ESP_LOGW(
            TAG,
            "INVALID CAMERA FRAME"
        );

        ESP_LOGW(
            TAG,
            "Expected: %dx%d RGB565 %d bytes",
            CAMERA_FRAME_WIDTH,
            CAMERA_FRAME_HEIGHT,
            CAMERA_EXPECTED_SIZE
        );

        ESP_LOGW(
            TAG,
            "Received: %ux%u format=%d length=%u",
            fb->width,
            fb->height,
            fb->format,
            (unsigned int)fb->len
        );

        /*
         * Return bad frame immediately.
         */
        esp_camera_fb_return(fb);

        return NULL;
    }


    return fb;
}


/*
 * ============================================================
 * RETURN FRAME
 * ============================================================
 */
void camera_module_return_frame(camera_fb_t *fb)
{
    if (fb != NULL) {

        esp_camera_fb_return(fb);
    }
}


/*
 * ============================================================
 * FRAME INFORMATION
 * ============================================================
 */
void camera_module_print_frame_info(camera_fb_t *fb)
{
    if (fb == NULL) {

        ESP_LOGE(
            TAG,
            "Frame is NULL"
        );

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


/*
 * ============================================================
 * CAMERA STATUS
 * ============================================================
 */
bool camera_module_is_initialized(void)
{
    return camera_initialized;
}