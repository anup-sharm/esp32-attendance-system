#include "face_recognition.h"

#include <list>
#include <vector>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"

#include "face_detection.h"

static const char *TAG = "FACE_RECOGNITION";

#define APPLICATION_RECOGNITION_THRESHOLD    0.90f
#define MAX_RECOGNITION_RESULTS              10

#define CAMERA_WIDTH                         320
#define CAMERA_HEIGHT                        240
#define RGB888_BUFFER_SIZE                   (CAMERA_WIDTH * CAMERA_HEIGHT * 3)

static HumanFaceRecognizer *face_recognizer = nullptr;
static HumanFaceDetect *recognition_face_detector = nullptr;

static bool recognition_initialized = false;

static bool last_result_valid = false;
static int last_face_id = -1;
static float last_similarity = 0.0f;

static int last_enrolled_face_id = -1;

static face_recognition_result_t
    last_results[MAX_RECOGNITION_RESULTS];

static int last_result_count = 0;

static uint8_t *g_rgb888_buffer = nullptr;

static const char *FACE_DATABASE_PATH =
    "/spiflash/face.db";


/* ============================================================
 * Clear previous recognition results
 * ============================================================ */

static void clear_last_results(void)
{
    last_result_valid = false;
    last_face_id = -1;
    last_similarity = 0.0f;
    last_result_count = 0;

    for (int i = 0; i < MAX_RECOGNITION_RESULTS; i++) {
        last_results[i].face_id = -1;
        last_results[i].similarity = 0.0f;
    }
}


/* ============================================================
 * RGB565 Big Endian -> RGB888
 *
 * Camera is producing:
 * PIXFORMAT_RGB565
 *
 * Current working camera data is RGB565BE.
 * ESP-DL detector receives RGB888.
 * ============================================================ */

static bool convert_rgb565be_to_rgb888(const camera_fb_t *fb)
{
    if (fb == nullptr) {
        ESP_LOGE(TAG, "convert: camera frame is NULL");
        return false;
    }

    if (fb->buf == nullptr) {
        ESP_LOGE(TAG, "convert: camera buffer is NULL");
        return false;
    }

    if (fb->width != CAMERA_WIDTH ||
        fb->height != CAMERA_HEIGHT) {

        ESP_LOGE(
            TAG,
            "convert: unsupported frame size %ux%u",
            (unsigned int)fb->width,
            (unsigned int)fb->height
        );

        return false;
    }

    if (fb->len < (CAMERA_WIDTH * CAMERA_HEIGHT * 2)) {
        ESP_LOGE(
            TAG,
            "convert: invalid frame length: %u",
            (unsigned int)fb->len
        );

        return false;
    }

    if (g_rgb888_buffer == nullptr) {
        ESP_LOGE(TAG, "RGB888 buffer is NULL");
        return false;
    }

    const uint8_t *src = fb->buf;
    uint8_t *dst = g_rgb888_buffer;

    const int pixel_count =
        CAMERA_WIDTH * CAMERA_HEIGHT;

    for (int i = 0; i < pixel_count; i++) {

        /*
         * RGB565 Big Endian:
         *
         * byte 0 = high byte
         * byte 1 = low byte
         */

        uint16_t pixel =
            ((uint16_t)src[i * 2] << 8) |
            ((uint16_t)src[i * 2 + 1]);

        uint8_t r5 =
            (pixel >> 11) & 0x1F;

        uint8_t g6 =
            (pixel >> 5) & 0x3F;

        uint8_t b5 =
            pixel & 0x1F;

        /*
         * Expand 5/6 bit values to 8 bit.
         */

        uint8_t r =
            (uint8_t)((r5 << 3) | (r5 >> 2));

        uint8_t g =
            (uint8_t)((g6 << 2) | (g6 >> 4));

        uint8_t b =
            (uint8_t)((b5 << 3) | (b5 >> 2));

        dst[i * 3 + 0] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
    }

    return true;
}


/* ============================================================
 * Run ESP-DL detector directly
 *
 * IMPORTANT:
 * We do NOT reconstruct dl::detect::result_t from the custom
 * face_detection_result_t because the custom structure does
 * not contain landmarks/keypoints.
 *
 * HumanFaceDetect::run() gives the real ESP-DL result including
 * the information required by HumanFaceRecognizer.
 * ============================================================ */

static bool run_recognition_detector(
    const camera_fb_t *fb,
    std::list<dl::detect::result_t> &detect_results
)
{
    detect_results.clear();

    if (fb == nullptr) {
        ESP_LOGE(TAG, "Detector: camera frame is NULL");
        return false;
    }

    if (recognition_face_detector == nullptr) {
        ESP_LOGE(TAG, "Detector object is NULL");
        return false;
    }

    if (!convert_rgb565be_to_rgb888(fb)) {
        ESP_LOGE(TAG, "RGB565 -> RGB888 conversion FAILED");
        return false;
    }

    dl::image::img_t image = {
        .data = (void *)g_rgb888_buffer,
        .width = (uint16_t)fb->width,
        .height = (uint16_t)fb->height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888
    };

    int64_t detector_start =
        esp_timer_get_time();

    auto &results =
        recognition_face_detector->run(image);

    int64_t detector_end =
        esp_timer_get_time();

    float detector_time_ms =
        (float)(detector_end - detector_start) / 1000.0f;

    ESP_LOGI(
        TAG,
        "Recognition detector execution time: %.2f ms",
        detector_time_ms
    );

    for (const auto &result : results) {
        detect_results.push_back(result);
    }

    ESP_LOGI(
        TAG,
        "Recognition detector returned %u face(s)",
        (unsigned int)detect_results.size()
    );

    int index = 0;

    for (const auto &result : detect_results) {

        ESP_LOGI(
            TAG,
            "Face[%d] box: [%d,%d,%d,%d] score=%.4f",
            index,
            result.box[0],
            result.box[1],
            result.box[2],
            result.box[3],
            result.score
        );

        index++;
    }

    return !detect_results.empty();
}


/* ============================================================
 * Face Recognition Initialization
 * ============================================================ */

extern "C" esp_err_t face_recognition_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "      FACE RECOGNITION INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    recognition_initialized = false;

    clear_last_results();

    last_enrolled_face_id = -1;

    /*
     * First verify that the main face detection component
     * has been initialized.
     */

    if (!face_detection_is_initialized()) {

        ESP_LOGE(
            TAG,
            "Face detector is not initialized"
        );

        ESP_LOGE(
            TAG,
            "Initialize face detection before face recognition"
        );

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Allocate RGB888 working buffer.
     *
     * 320 x 240 x 3 = 230400 bytes.
     */

    if (g_rgb888_buffer == nullptr) {

        g_rgb888_buffer =
            (uint8_t *)heap_caps_malloc(
                RGB888_BUFFER_SIZE,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

        if (g_rgb888_buffer == nullptr) {

            ESP_LOGW(
                TAG,
                "PSRAM RGB888 allocation failed, trying internal RAM"
            );

            g_rgb888_buffer =
                (uint8_t *)heap_caps_malloc(
                    RGB888_BUFFER_SIZE,
                    MALLOC_CAP_8BIT
                );
        }
    }

    if (g_rgb888_buffer == nullptr) {

        ESP_LOGE(
            TAG,
            "RGB888 buffer allocation FAILED (%d bytes)",
            RGB888_BUFFER_SIZE
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "RGB888 working buffer allocated: %d bytes",
        RGB888_BUFFER_SIZE
    );

    /*
     * Create an independent ESP-DL detector for recognition.
     *
     * This gives us the real dl::detect::result_t objects
     * required by HumanFaceRecognizer.
     */

    recognition_face_detector =
        new HumanFaceDetect();

    if (recognition_face_detector == nullptr) {

        ESP_LOGE(
            TAG,
            "Recognition HumanFaceDetect creation FAILED"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Recognition detector initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "Recognition model: MFN_S8_V1"
    );

    ESP_LOGI(
        TAG,
        "Database path: %s",
        FACE_DATABASE_PATH
    );

    ESP_LOGI(
        TAG,
        "Application similarity threshold: %.2f",
        APPLICATION_RECOGNITION_THRESHOLD
    );

    /*
     * Create face recognizer.
     */

    face_recognizer =
        new HumanFaceRecognizer(
            FACE_DATABASE_PATH,
            HumanFaceFeat::MFN_S8_V1,
            false
        );

    if (face_recognizer == nullptr) {

        ESP_LOGE(
            TAG,
            "HumanFaceRecognizer creation FAILED"
        );

        recognition_initialized = false;

        return ESP_FAIL;
    }

    recognition_initialized = true;

    ESP_LOGI(
        TAG,
        "HumanFaceRecognizer initialization PASSED"
    );

    int enrolled_faces =
        face_recognizer->get_num_feats();

    ESP_LOGI(
        TAG,
        "Enrolled face count: %d",
        enrolled_faces
    );

    if (enrolled_faces == 0) {

        ESP_LOGW(
            TAG,
            "WARNING: No face is enrolled in database"
        );

        ESP_LOGW(
            TAG,
            "Recognition cannot identify employees until faces are enrolled"
        );
    }

    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}


/* ============================================================
 * FACE ENROLLMENT
 * ============================================================ */

extern "C" bool face_recognition_enroll(
    const camera_fb_t *fb
)
{
    if (!recognition_initialized) {

        ESP_LOGE(
            TAG,
            "Enrollment failed: recognizer is not initialized"
        );

        return false;
    }

    if (face_recognizer == nullptr) {

        ESP_LOGE(
            TAG,
            "Enrollment failed: recognizer object is NULL"
        );

        return false;
    }

    if (fb == nullptr) {

        ESP_LOGE(
            TAG,
            "Enrollment failed: camera frame is NULL"
        );

        return false;
    }

    /*
     * Run the real ESP-DL detector.
     */

    std::list<dl::detect::result_t> detect_results;

    if (!run_recognition_detector(
            fb,
            detect_results)) {

        ESP_LOGW(
            TAG,
            "ENROLLMENT: No face detected"
        );

        return false;
    }

    /*
     * Exactly ONE face is required.
     */

    if (detect_results.size() > 1) {

        ESP_LOGW(
            TAG,
            "ENROLLMENT: Multiple faces detected (%u)",
            (unsigned int)detect_results.size()
        );

        ESP_LOGW(
            TAG,
            "Please keep only ONE face in front of the camera"
        );

        return false;
    }

    /*
     * Convert frame to RGB888.
     */

    if (!convert_rgb565be_to_rgb888(fb)) {

        ESP_LOGE(
            TAG,
            "ENROLLMENT: RGB888 conversion failed"
        );

        return false;
    }

    dl::image::img_t image = {
        .data = (void *)g_rgb888_buffer,
        .width = (uint16_t)fb->width,
        .height = (uint16_t)fb->height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888
    };

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "         FACE ENROLLMENT START");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(
        TAG,
        "Image: %ux%u",
        (unsigned int)fb->width,
        (unsigned int)fb->height
    );

    ESP_LOGI(
        TAG,
        "Detected face count: %u",
        (unsigned int)detect_results.size()
    );

    int64_t enrollment_start =
        esp_timer_get_time();

    esp_err_t enroll_result =
        face_recognizer->enroll(
            image,
            detect_results
        );

    int64_t enrollment_end =
        esp_timer_get_time();

    float enrollment_time_ms =
        (float)(
            enrollment_end -
            enrollment_start
        ) / 1000.0f;

    ESP_LOGI(
        TAG,
        "Face enrollment execution time: %.2f ms",
        enrollment_time_ms
    );

    if (enroll_result != ESP_OK) {

        ESP_LOGE(
            TAG,
            "FACE ENROLLMENT FAILED"
        );

        ESP_LOGE(
            TAG,
            "enroll() returned: %s",
            esp_err_to_name(enroll_result)
        );

        return false;
    }

    int enrolled_faces =
        face_recognizer->get_num_feats();

    if (enrolled_faces <= 0) {

        ESP_LOGE(
            TAG,
            "Enrollment reported invalid database count: %d",
            enrolled_faces
        );

        last_enrolled_face_id = -1;

        return false;
    }

    /*
     * Get actual Face ID generated by ESP-DL database.
     */

    last_enrolled_face_id =
        face_recognizer->get_last_feat_id();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "      FACE ENROLLMENT SUCCESS");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(
        TAG,
        "Total enrolled faces: %d",
        enrolled_faces
    );

    ESP_LOGI(
        TAG,
        "New Face ID: %d",
        last_enrolled_face_id
    );

    ESP_LOGI(
        TAG,
        "Database: %s",
        FACE_DATABASE_PATH
    );

    ESP_LOGI(TAG, "========================================");

    return true;
}


/* ============================================================
 * FACE RECOGNITION PROCESS
 * ============================================================ */

extern "C" bool face_recognition_process(
    const camera_fb_t *fb
)
{
    clear_last_results();

    if (!recognition_initialized) {

        ESP_LOGE(
            TAG,
            "Face recognizer is not initialized"
        );

        return false;
    }

    if (face_recognizer == nullptr) {

        ESP_LOGE(
            TAG,
            "Face recognizer object is NULL"
        );

        return false;
    }

    if (fb == nullptr) {

        ESP_LOGE(
            TAG,
            "Camera frame is NULL"
        );

        return false;
    }

    /*
     * Run actual ESP-DL detector.
     */

    std::list<dl::detect::result_t> detect_results;

    if (!run_recognition_detector(
            fb,
            detect_results)) {

        ESP_LOGI(
            TAG,
            "Recognition skipped: no face detected"
        );

        return false;
    }

    /*
     * Convert camera frame to RGB888.
     */

    if (!convert_rgb565be_to_rgb888(fb)) {

        ESP_LOGE(
            TAG,
            "Recognition RGB888 conversion FAILED"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Recognition input: %u detected face(s)",
        (unsigned int)detect_results.size()
    );

    dl::image::img_t image = {
        .data = (void *)g_rgb888_buffer,
        .width = (uint16_t)fb->width,
        .height = (uint16_t)fb->height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888
    };

    ESP_LOGI(
        TAG,
        "Calling HumanFaceRecognizer::recognize()..."
    );

    int64_t recognition_start =
        esp_timer_get_time();

    std::vector<dl::recognition::result_t>
        recognition_results =
            face_recognizer->recognize(
                image,
                detect_results
            );

    int64_t recognition_end =
        esp_timer_get_time();

    float recognition_time_ms =
        (float)(
            recognition_end -
            recognition_start
        ) / 1000.0f;

    ESP_LOGI(
        TAG,
        "Face recognition execution time: %.2f ms",
        recognition_time_ms
    );

    ESP_LOGI(
        TAG,
        "Recognition returned %u result(s)",
        (unsigned int)recognition_results.size()
    );

    if (recognition_results.empty()) {

        ESP_LOGI(
            TAG,
            "NO MATCHING ENROLLED FACE"
        );

        return false;
    }

    int accepted_count = 0;

    for (const auto &result :
         recognition_results) {

        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "RECOGNITION CANDIDATE"
        );

        ESP_LOGI(
            TAG,
            "Face ID    : %d",
            result.id
        );

        ESP_LOGI(
            TAG,
            "Similarity : %.4f",
            result.similarity
        );

        if (
            result.similarity <
            APPLICATION_RECOGNITION_THRESHOLD
        ) {

            ESP_LOGW(
                TAG,
                "MATCH REJECTED"
            );

            ESP_LOGW(
                TAG,
                "Similarity %.4f is below application threshold %.2f",
                result.similarity,
                APPLICATION_RECOGNITION_THRESHOLD
            );

            ESP_LOGW(
                TAG,
                "This face will NOT be used for attendance"
            );

            ESP_LOGI(
                TAG,
                "========================================"
            );

            continue;
        }

        if (
            accepted_count <
            MAX_RECOGNITION_RESULTS
        ) {

            last_results[accepted_count].face_id =
                result.id;

            last_results[accepted_count].similarity =
                result.similarity;

            accepted_count++;
        }

        ESP_LOGI(
            TAG,
            "MATCH ACCEPTED"
        );

        ESP_LOGI(
            TAG,
            "Face ID    : %d",
            result.id
        );

        ESP_LOGI(
            TAG,
            "Similarity : %.4f",
            result.similarity
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );
    }

    last_result_count =
        accepted_count;

    if (accepted_count == 0) {

        ESP_LOGW(
            TAG,
            "NO FACE PASSED APPLICATION RECOGNITION THRESHOLD"
        );

        return false;
    }

    /*
     * First accepted result is the primary result.
     */

    last_face_id =
        last_results[0].face_id;

    last_similarity =
        last_results[0].similarity;

    last_result_valid = true;

    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "      RECOGNITION SUCCESS"
    );

    ESP_LOGI(
        TAG,
        "Accepted faces: %d",
        accepted_count
    );

    for (int i = 0;
         i < accepted_count;
         i++) {

        ESP_LOGI(
            TAG,
            "Result[%d] -> Face ID: %d | Similarity: %.4f",
            i,
            last_results[i].face_id,
            last_results[i].similarity
        );
    }

    ESP_LOGI(
        TAG,
        "========================================"
    );

    return true;
}


/* ============================================================
 * Initialization status
 * ============================================================ */

extern "C" bool face_recognition_is_initialized(void)
{
    return recognition_initialized;
}


/* ============================================================
 * Number of enrolled faces
 * ============================================================ */

extern "C" int face_recognition_get_num_faces(void)
{
    if (!recognition_initialized) {
        return 0;
    }

    if (face_recognizer == nullptr) {
        return 0;
    }

    return face_recognizer->get_num_feats();
}


/* ============================================================
 * Last enrolled Face ID
 * ============================================================ */

extern "C" bool face_recognition_get_last_enrolled_face_id(
    int *face_id
)
{
    if (face_id == nullptr) {

        ESP_LOGE(
            TAG,
            "get_last_enrolled_face_id(): invalid output pointer"
        );

        return false;
    }

    if (last_enrolled_face_id < 0) {
        return false;
    }

    *face_id =
        last_enrolled_face_id;

    return true;
}


/* ============================================================
 * Last primary recognition result
 * ============================================================ */

extern "C" bool face_recognition_get_last_result(
    int *face_id,
    float *similarity
)
{
    if (
        face_id == nullptr ||
        similarity == nullptr
    ) {

        ESP_LOGE(
            TAG,
            "get_last_result(): invalid output pointer"
        );

        return false;
    }

    if (!last_result_valid) {
        return false;
    }

    *face_id =
        last_face_id;

    *similarity =
        last_similarity;

    return true;
}


/* ============================================================
 * Get all accepted recognition results
 * ============================================================ */

extern "C" int face_recognition_get_results(
    face_recognition_result_t *results,
    int max_results
)
{
    if (
        results == nullptr ||
        max_results <= 0
    ) {
        return 0;
    }

    int copy_count =
        last_result_count;

    if (copy_count > max_results) {
        copy_count = max_results;
    }

    for (int i = 0;
         i < copy_count;
         i++) {

        results[i] =
            last_results[i];
    }

    return copy_count;
}


/* ============================================================
 * Application recognition threshold
 * ============================================================ */

extern "C" float face_recognition_get_application_threshold(void)
{
    return APPLICATION_RECOGNITION_THRESHOLD;
}