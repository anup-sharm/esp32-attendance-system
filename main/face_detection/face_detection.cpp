#include "face_detection.h"

#include <list>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

static const char *TAG = "FACE_DETECT";

/* ============================================================
 * CAMERA CONFIGURATION
 * ============================================================ */

#define CAMERA_WIDTH                    320
#define CAMERA_HEIGHT                   240
#define CAMERA_RGB565_FRAME_SIZE        (CAMERA_WIDTH * CAMERA_HEIGHT * 2)
#define CAMERA_RGB888_FRAME_SIZE        (CAMERA_WIDTH * CAMERA_HEIGHT * 3)

/* ============================================================
 * DETECTOR CONFIGURATION
 * ============================================================ */

#define FACE_DETECTION_SCORE_THRESHOLD  0.50f

/* ============================================================
 * GLOBAL OBJECTS
 * ============================================================ */

static HumanFaceDetect *g_face_detector = nullptr;

static bool g_initialized = false;

/*
 * RGB888 working buffer.
 *
 * Camera gives RGB565.
 * ESP-DL detector receives RGB888 here.
 */
static uint8_t *g_rgb888_buffer = nullptr;

/* ============================================================
 * DETECTION RESULTS
 * ============================================================ */

static face_detection_result_t
    g_results[FACE_DETECTION_MAX_RESULTS];

static int g_result_count = 0;

/* ============================================================
 * CLEAR RESULTS
 * ============================================================ */

static void clear_results(void)
{
    g_result_count = 0;

    for (int i = 0; i < FACE_DETECTION_MAX_RESULTS; i++) {

        g_results[i].box[0] = 0;
        g_results[i].box[1] = 0;
        g_results[i].box[2] = 0;
        g_results[i].box[3] = 0;

        g_results[i].score = 0.0f;
    }
}

/* ============================================================
 * RGB565 BE -> RGB888
 *
 * GC2145 frame is RGB565.
 *
 * Camera byte order currently observed:
 *
 *   BYTE0 = HIGH BYTE
 *   BYTE1 = LOW BYTE
 *
 * Therefore:
 *
 *   RGB565 = BYTE0 << 8 | BYTE1
 * ============================================================ */

static bool convert_rgb565be_to_rgb888(
    const camera_fb_t *fb
)
{
    if (fb == nullptr) {

        ESP_LOGE(
            TAG,
            "RGB888 conversion failed: frame is NULL"
        );

        return false;
    }

    if (fb->buf == nullptr) {

        ESP_LOGE(
            TAG,
            "RGB888 conversion failed: buffer is NULL"
        );

        return false;
    }

    if (fb->width != CAMERA_WIDTH ||
        fb->height != CAMERA_HEIGHT) {

        ESP_LOGE(
            TAG,
            "Unsupported frame size: %ux%u",
            (unsigned int)fb->width,
            (unsigned int)fb->height
        );

        return false;
    }

    if (fb->len < CAMERA_RGB565_FRAME_SIZE) {

        ESP_LOGE(
            TAG,
            "Invalid frame length: %u, expected at least %u",
            (unsigned int)fb->len,
            (unsigned int)CAMERA_RGB565_FRAME_SIZE
        );

        return false;
    }

    if (g_rgb888_buffer == nullptr) {

        ESP_LOGE(
            TAG,
            "RGB888 buffer is NULL"
        );

        return false;
    }

    const uint8_t *src = fb->buf;
    uint8_t *dst = g_rgb888_buffer;

    const int pixel_count =
        CAMERA_WIDTH * CAMERA_HEIGHT;

    for (int i = 0; i < pixel_count; i++) {

        uint16_t pixel =
            ((uint16_t)src[(i * 2) + 0] << 8) |
            ((uint16_t)src[(i * 2) + 1]);

        /* ----------------------------------------------------
         * Extract RGB565
         * ---------------------------------------------------- */

        uint8_t r5 =
            (pixel >> 11) & 0x1F;

        uint8_t g6 =
            (pixel >> 5) & 0x3F;

        uint8_t b5 =
            pixel & 0x1F;

        /* ----------------------------------------------------
         * Expand to 8-bit
         * ---------------------------------------------------- */

        uint8_t r =
            (uint8_t)(
                (r5 << 3) |
                (r5 >> 2)
            );

        uint8_t g =
            (uint8_t)(
                (g6 << 2) |
                (g6 >> 4)
            );

        uint8_t b =
            (uint8_t)(
                (b5 << 3) |
                (b5 >> 2)
            );

        /* ----------------------------------------------------
         * RGB888 output
         *
         * R G B
         * ---------------------------------------------------- */

        dst[(i * 3) + 0] = r;
        dst[(i * 3) + 1] = g;
        dst[(i * 3) + 2] = b;
    }

    return true;
}

/* ============================================================
 * INITIALIZE FACE DETECTOR
 * ============================================================ */

extern "C" bool face_detection_init(void)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "        FACE DETECTION INIT"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    /*
     * Reset state.
     */
    g_initialized = false;

    clear_results();

    /*
     * --------------------------------------------------------
     * Existing detector cleanup
     * --------------------------------------------------------
     */

    if (g_face_detector != nullptr) {

        delete g_face_detector;

        g_face_detector = nullptr;
    }

    /*
     * --------------------------------------------------------
     * Existing RGB buffer cleanup
     * --------------------------------------------------------
     */

    if (g_rgb888_buffer != nullptr) {

        free(g_rgb888_buffer);

        g_rgb888_buffer = nullptr;
    }

    /*
     * --------------------------------------------------------
     * Create HumanFaceDetect object
     *
     * Default ESP-DL detector uses the configured
     * HumanFaceDetect model.
     * --------------------------------------------------------
     */

    g_face_detector =
        new HumanFaceDetect();

    if (g_face_detector == nullptr) {

        ESP_LOGE(
            TAG,
            "HumanFaceDetect allocation FAILED"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "HumanFaceDetect initialized"
    );

    ESP_LOGI(
        TAG,
        "Model: MSRMNP_S8_V1"
    );

    ESP_LOGI(
        TAG,
        "Camera input: RGB565 -> RGB888"
    );

    ESP_LOGI(
        TAG,
        "Resolution: %dx%d",
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    );

    /*
     * --------------------------------------------------------
     * Allocate RGB888 buffer.
     *
     * 320 x 240 x 3 = 230400 bytes
     *
     * Prefer PSRAM.
     * --------------------------------------------------------
     */

    g_rgb888_buffer =
        (uint8_t *)heap_caps_malloc(
            CAMERA_RGB888_FRAME_SIZE,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (g_rgb888_buffer == nullptr) {

        ESP_LOGW(
            TAG,
            "PSRAM RGB888 allocation failed"
        );

        ESP_LOGW(
            TAG,
            "Trying internal RAM..."
        );

        g_rgb888_buffer =
            (uint8_t *)heap_caps_malloc(
                CAMERA_RGB888_FRAME_SIZE,
                MALLOC_CAP_8BIT
            );
    }

    if (g_rgb888_buffer == nullptr) {

        ESP_LOGE(
            TAG,
            "RGB888 buffer allocation FAILED"
        );

        delete g_face_detector;

        g_face_detector = nullptr;

        return false;
    }

    memset(
        g_rgb888_buffer,
        0,
        CAMERA_RGB888_FRAME_SIZE
    );

    ESP_LOGI(
        TAG,
        "RGB888 buffer allocated: %d bytes",
        CAMERA_RGB888_FRAME_SIZE
    );

    /*
     * --------------------------------------------------------
     * Final initialization state.
     * --------------------------------------------------------
     */

    g_initialized = true;

    ESP_LOGI(
        TAG,
        "Face detection initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    return true;
}

/* ============================================================
 * CHECK INITIALIZATION
 * ============================================================ */

extern "C" bool face_detection_is_initialized(void)
{
    return g_initialized &&
           g_face_detector != nullptr &&
           g_rgb888_buffer != nullptr;
}

/* ============================================================
 * PROCESS CAMERA FRAME
 * ============================================================ */

extern "C" bool face_detection_process(
    const camera_fb_t *fb
)
{
    clear_results();

    /*
     * --------------------------------------------------------
     * Initialization check
     * --------------------------------------------------------
     */

    if (!face_detection_is_initialized()) {

        ESP_LOGE(
            TAG,
            "Face detector is not initialized"
        );

        return false;
    }

    /*
     * --------------------------------------------------------
     * Frame check
     * --------------------------------------------------------
     */

    if (fb == nullptr) {

        ESP_LOGE(
            TAG,
            "Camera frame is NULL"
        );

        return false;
    }

    /*
     * --------------------------------------------------------
     * Convert RGB565 -> RGB888
     * --------------------------------------------------------
     */

    int64_t conversion_start =
        esp_timer_get_time();

    if (!convert_rgb565be_to_rgb888(fb)) {

        ESP_LOGE(
            TAG,
            "RGB565 -> RGB888 conversion FAILED"
        );

        return false;
    }

    int64_t conversion_end =
        esp_timer_get_time();

    float conversion_time_ms =
        (float)(
            conversion_end -
            conversion_start
        ) / 1000.0f;

    /*
     * --------------------------------------------------------
     * Build ESP-DL image
     * --------------------------------------------------------
     */

    dl::image::img_t image = {

        .data =
            (void *)g_rgb888_buffer,

        .width =
            (uint16_t)fb->width,

        .height =
            (uint16_t)fb->height,

        .pix_type =
            dl::image::DL_IMAGE_PIX_TYPE_RGB888
    };

    /*
     * --------------------------------------------------------
     * Run HumanFaceDetect
     * --------------------------------------------------------
     */

    int64_t detector_start =
        esp_timer_get_time();

    auto &detect_results =
        g_face_detector->run(image);

    int64_t detector_end =
        esp_timer_get_time();

    float detector_time_ms =
        (float)(
            detector_end -
            detector_start
        ) / 1000.0f;

    ESP_LOGI(
        TAG,
        "RGB565 -> RGB888: %.2f ms",
        conversion_time_ms
    );

    ESP_LOGI(
        TAG,
        "Face detector execution: %.2f ms",
        detector_time_ms
    );

    ESP_LOGI(
        TAG,
        "Detector returned %u result(s)",
        (unsigned int)detect_results.size()
    );

    /*
     * --------------------------------------------------------
     * Copy detector results to our C API structure.
     * --------------------------------------------------------
     */

    int result_index = 0;

    for (const auto &result :
         detect_results) {

        /*
         * Respect maximum result count.
         */

        if (
            result_index >=
            FACE_DETECTION_MAX_RESULTS
        ) {
            break;
        }

        /*
         * Copy bounding box.
         *
         * ESP-DL result_t:
         *
         * box[0] = x1
         * box[1] = y1
         * box[2] = x2
         * box[3] = y2
         */

        g_results[result_index].box[0] =
            result.box[0];

        g_results[result_index].box[1] =
            result.box[1];

        g_results[result_index].box[2] =
            result.box[2];

        g_results[result_index].box[3] =
            result.box[3];

        g_results[result_index].score =
            result.score;

        /*
         * Log result.
         */

        ESP_LOGI(
            TAG,
            "Face[%d] box=[%d,%d,%d,%d] score=%.4f",
            result_index,

            g_results[result_index].box[0],
            g_results[result_index].box[1],
            g_results[result_index].box[2],
            g_results[result_index].box[3],

            g_results[result_index].score
        );

        result_index++;
    }

    g_result_count =
        result_index;

    /*
     * --------------------------------------------------------
     * No face
     * --------------------------------------------------------
     */

    if (g_result_count == 0) {

        return false;
    }

    /*
     * --------------------------------------------------------
     * Face detected.
     * --------------------------------------------------------
     */

    return true;
}

/* ============================================================
 * GET NUMBER OF FACES
 * ============================================================ */

extern "C" int face_detection_get_count(void)
{
    return g_result_count;
}

/* ============================================================
 * GET ALL RESULTS
 * ============================================================ */

extern "C" int face_detection_get_results(
    face_detection_result_t *results,
    int max_results
)
{
    if (results == nullptr) {
        return 0;
    }

    if (max_results <= 0) {
        return 0;
    }

    int copy_count =
        g_result_count;

    if (copy_count > max_results) {
        copy_count = max_results;
    }

    for (int i = 0;
         i < copy_count;
         i++) {

        results[i] =
            g_results[i];
    }

    return copy_count;
}

/* ============================================================
 * GET ONE RESULT
 * ============================================================ */

extern "C" bool face_detection_get_result(
    int index,
    face_detection_result_t *result
)
{
    if (result == nullptr) {
        return false;
    }

    if (index < 0) {
        return false;
    }

    if (index >= g_result_count) {
        return false;
    }

    *result =
        g_results[index];

    return true;
}