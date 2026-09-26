#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "nvs_flash.h"

#include "wifi_manager.h"
#include "camera.h"
#include "tft.h"


#define TAG "ATTENDANCE"


void app_main(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "   ESP32-S3 ATTENDANCE SYSTEM");
    ESP_LOGI(TAG, "   GC2145 -> ILI9341 LIVE CAMERA");
    ESP_LOGI(TAG, "======================================");


    /*
     * ========================================================
     * STEP 1: INITIALIZE NVS
     * ========================================================
     */

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_LOGW(TAG,
                 "NVS requires erase and reinitialization");

        ESP_ERROR_CHECK(nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG,
             "NVS initialized");


    /*
     * ========================================================
     * STEP 2: INITIALIZE NETWORK INTERFACE
     * ========================================================
     */

    ret = esp_netif_init();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(TAG,
                 "esp_netif_init failed: %s",
                 esp_err_to_name(ret));

        return;
    }

    ESP_LOGI(TAG,
             "Network interface initialized");


    /*
     * ========================================================
     * STEP 3: CREATE DEFAULT EVENT LOOP
     * ========================================================
     */

    ret = esp_event_loop_create_default();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(TAG,
                 "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(ret));

        return;
    }

    ESP_LOGI(TAG,
             "Default event loop initialized");


    /*
     * ========================================================
     * STEP 4: INITIALIZE WIFI MANAGER
     * ========================================================
     */

    ret = wifi_manager_init();

    if (ret != ESP_OK) {

        ESP_LOGE(TAG,
                 "Wi-Fi manager initialization failed: %s",
                 esp_err_to_name(ret));

        return;
    }


    /*
     * ========================================================
     * STEP 5: START WIFI
     * ========================================================
     */

    ret = wifi_manager_start();

    if (ret != ESP_OK) {

        ESP_LOGE(TAG,
                 "Wi-Fi manager start failed: %s",
                 esp_err_to_name(ret));

        /*
         * Do not stop the entire application.
         *
         * Attendance hardware should still be able
         * to operate offline.
         */
    }


    /*
     * ========================================================
     * STEP 6: INITIALIZE ILI9341 TFT
     * ========================================================
     */

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "       STARTING ILI9341 TFT");
    ESP_LOGI(TAG, "======================================");

    ret = tft_init();

    if (ret != ESP_OK) {

        ESP_LOGE(TAG,
                 "ILI9341 TFT initialization failed: %s",
                 esp_err_to_name(ret));

        ESP_LOGE(TAG,
                 "Camera system will continue without TFT.");

    } else {

        ESP_LOGI(TAG,
                 "ILI9341 TFT initialized successfully");

        ESP_LOGI(TAG,
                 "TFT resolution: 240x320");

        ESP_LOGI(TAG,
                 "TFT SPI speed: 27 MHz");
    }


    /*
     * ========================================================
     * STEP 7: INITIALIZE GC2145 CAMERA
     * ========================================================
     */

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "       STARTING GC2145 CAMERA");
    ESP_LOGI(TAG, "======================================");


    ret = camera_module_init();

    bool camera_ready = false;

    if (ret != ESP_OK) {

        ESP_LOGE(TAG,
                 "GC2145 camera initialization failed: %s",
                 esp_err_to_name(ret));

        ESP_LOGE(TAG,
                 "Camera capture test will NOT be started.");

    } else {

        camera_ready = true;

        ESP_LOGI(TAG,
                 "GC2145 camera initialized successfully");
    }


    /*
     * ========================================================
     * SYSTEM INITIALIZATION COMPLETE
     * ========================================================
     */

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "SYSTEM INITIALIZATION COMPLETE");
    ESP_LOGI(TAG, "======================================");


    /*
     * ========================================================
     * CAMERA + TFT LIVE PREVIEW
     * ========================================================
     *
     * Camera:
     *
     *     320 x 240 RGB565
     *
     * TFT:
     *
     *     240 x 320 portrait
     *
     * Pipeline:
     *
     *     GC2145
     *        ↓
     *     Camera Frame
     *        ↓
     *     PSRAM
     *        ↓
     *     90-degree rotation
     *        ↓
     *     240-pixel line buffer
     *        ↓
     *     SPI DMA
     *        ↓
     *     ILI9341
     *
     * ========================================================
     */

    if (camera_ready) {

        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "       LIVE CAMERA STARTING");
        ESP_LOGI(TAG, "======================================");

        ESP_LOGI(TAG,
                 "Camera: 320x240 RGB565");

        ESP_LOGI(TAG,
                 "Display: 240x320 portrait");

        ESP_LOGI(TAG,
                 "SPI: 27 MHz");

        ESP_LOGI(TAG,
                 "Line buffer: 240 pixels");

        ESP_LOGI(TAG,
                 "======================================");


        uint32_t frame_count = 0;

        TickType_t fps_start =
            xTaskGetTickCount();


        while (1) {

            /*
             * ------------------------------------------------
             * CAPTURE CAMERA FRAME
             * ------------------------------------------------
             */

            camera_fb_t *fb =
                camera_module_capture();


            if (fb == NULL) {

                ESP_LOGE(TAG,
                         "No camera frame received");

                vTaskDelay(
                    pdMS_TO_TICKS(10)
                );

                continue;
            }


            /*
             * ------------------------------------------------
             * DISPLAY FRAME ON TFT
             * ------------------------------------------------
             */

            bool display_ok =
                tft_display_camera_frame(fb);


            /*
             * ------------------------------------------------
             * RETURN CAMERA FRAME
             * ------------------------------------------------
             */

            camera_module_return_frame(fb);


            /*
             * ------------------------------------------------
             * DISPLAY STATUS
             * ------------------------------------------------
             */

            if (!display_ok) {

                ESP_LOGE(TAG,
                         "TFT frame display failed");
            }


            frame_count++;


            /*
             * ------------------------------------------------
             * FPS REPORT
             * ------------------------------------------------
             *
             * Print FPS every 5 seconds.
             *
             * This does NOT affect camera operation.
             */

            TickType_t now =
                xTaskGetTickCount();


            if (
                now - fps_start >=
                pdMS_TO_TICKS(5000)
            ) {

                uint32_t elapsed_ms =
                    (
                        now - fps_start
                    ) *
                    portTICK_PERIOD_MS;


                float fps =
                    (
                        frame_count *
                        1000.0f
                    ) /
                    elapsed_ms;


                ESP_LOGI(
                    TAG,
                    "LIVE FPS: %.2f | Frames: %lu",
                    fps,
                    (unsigned long)frame_count
                );


                frame_count = 0;

                fps_start = now;
            }
        }


    } else {

        /*
         * ====================================================
         * CAMERA FAILED
         * ====================================================
         */

        while (1) {

            ESP_LOGW(
                TAG,
                "System running without camera"
            );

            vTaskDelay(
                pdMS_TO_TICKS(5000)
            );
        }
    }
}