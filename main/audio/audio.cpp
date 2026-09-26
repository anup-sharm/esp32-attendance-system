#include "audio.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"

#include "picotts.h"

#define TAG "AUDIO"

/* =========================================================
 * MAX98357A PINS
 * ========================================================= */

#define AUDIO_BCLK_GPIO   45
#define AUDIO_WS_GPIO     46
#define AUDIO_DOUT_GPIO   47

#define AUDIO_SAMPLE_RATE 16000

#define AUDIO_WAV_PATH \
    "/spiflash/audio/good_morning.wav"

/* =========================================================
 * TTS CONFIGURATION
 *
 * Smaller chunks + larger queue gives the PicoTTS task
 * smoother back-pressure and prevents audio truncation.
 * ========================================================= */

#define TTS_PCM_CHUNK_SAMPLES   512
#define TTS_PCM_QUEUE_LENGTH    48

#define TTS_QUEUE_WAIT_MS       1000
#define TTS_OUTPUT_STACK_SIZE   6144
#define TTS_OUTPUT_PRIORITY     10
#define TTS_OUTPUT_CORE         0

#define TTS_FINAL_DRAIN_DELAY_MS 80

typedef struct
{
    uint16_t sample_count;
    int16_t samples[TTS_PCM_CHUNK_SAMPLES];

} tts_pcm_chunk_t;

/* =========================================================
 * GLOBAL STATE
 * ========================================================= */

static i2s_chan_handle_t tx_handle = NULL;

static bool audio_initialized = false;
static volatile bool audio_enabled = false;

static bool tts_initialized = false;
static volatile bool tts_busy = false;

static volatile bool tts_generation_done = false;

static QueueHandle_t tts_pcm_queue = NULL;

static TaskHandle_t audio_output_task_handle = NULL;

static bool audio_output_task_started = false;

/* =========================================================
 * STOP I2S
 * ========================================================= */

static esp_err_t audio_stop_output(void)
{
    if (tx_handle == NULL) {
        return ESP_OK;
    }

    if (audio_enabled) {

        esp_err_t err = i2s_channel_disable(tx_handle);

        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "i2s_channel_disable failed: %s",
                esp_err_to_name(err)
            );

            return err;
        }

        audio_enabled = false;
    }

    return ESP_OK;
}

/* =========================================================
 * START I2S
 * ========================================================= */

static esp_err_t audio_start_output(void)
{
    if (!audio_initialized || tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_enabled) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(tx_handle);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "i2s_channel_enable failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    audio_enabled = true;

    return ESP_OK;
}

/* =========================================================
 * AUDIO OUTPUT TASK
 *
 * PicoTTS callback NEVER writes directly to I2S.
 * Only this task writes PCM to MAX98357A.
 * ========================================================= */

static void audio_output_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Audio output task started"
    );

    tts_pcm_chunk_t chunk;

    while (true) {

        BaseType_t received =
            xQueueReceive(
                tts_pcm_queue,
                &chunk,
                pdMS_TO_TICKS(100)
            );

        if (received == pdTRUE) {

            if (!audio_initialized || tx_handle == NULL) {
                continue;
            }

            if (!audio_enabled) {

                esp_err_t err =
                    audio_start_output();

                if (err != ESP_OK) {

                    ESP_LOGE(
                        TAG,
                        "Unable to start I2S: %s",
                        esp_err_to_name(err)
                    );

                    continue;
                }
            }

            size_t bytes_to_write =
                ((size_t)chunk.sample_count) *
                sizeof(int16_t);

            size_t total_written = 0;

            while (total_written < bytes_to_write) {

                size_t bytes_written = 0;

                esp_err_t err =
                    i2s_channel_write(
                        tx_handle,
                        ((uint8_t *)chunk.samples) +
                            total_written,
                        bytes_to_write -
                            total_written,
                        &bytes_written,
                        portMAX_DELAY
                    );

                if (err != ESP_OK) {

                    ESP_LOGE(
                        TAG,
                        "I2S write failed: %s",
                        esp_err_to_name(err)
                    );

                    break;
                }

                if (bytes_written == 0) {

                    ESP_LOGE(
                        TAG,
                        "I2S returned zero bytes written"
                    );

                    break;
                }

                total_written += bytes_written;
            }
        }

        /*
         * PicoTTS has finished generating.
         *
         * The generation callback only sets the flag.
         * We stop I2S here, after the PCM queue is empty.
         */
        if (
            tts_generation_done &&
            uxQueueMessagesWaiting(tts_pcm_queue) == 0
        ) {

            if (tts_busy) {

                /*
                 * Allow final I2S DMA data to drain.
                 */
                vTaskDelay(
                    pdMS_TO_TICKS(
                        TTS_FINAL_DRAIN_DELAY_MS
                    )
                );

                audio_stop_output();

                tts_generation_done = false;
                tts_busy = false;

                ESP_LOGI(
                    TAG,
                    "TTS playback completely finished"
                );
            }
        }

        taskYIELD();
    }
}

/* =========================================================
 * START AUDIO OUTPUT TASK
 * ========================================================= */

static esp_err_t audio_start_output_task(void)
{
    if (audio_output_task_started) {
        return ESP_OK;
    }

    tts_pcm_queue =
        xQueueCreate(
            TTS_PCM_QUEUE_LENGTH,
            sizeof(tts_pcm_chunk_t)
        );

    if (tts_pcm_queue == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create TTS PCM queue"
        );

        return ESP_ERR_NO_MEM;
    }

    BaseType_t result =
        xTaskCreatePinnedToCore(
            audio_output_task,
            "audio_output",
            TTS_OUTPUT_STACK_SIZE,
            NULL,
            TTS_OUTPUT_PRIORITY,
            &audio_output_task_handle,
            TTS_OUTPUT_CORE
        );

    if (result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create audio output task"
        );

        vQueueDelete(tts_pcm_queue);

        tts_pcm_queue = NULL;

        return ESP_FAIL;
    }

    audio_output_task_started = true;

    return ESP_OK;
}

/* =========================================================
 * AUDIO INITIALIZATION
 * ========================================================= */

esp_err_t audio_init(void)
{
    if (audio_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "MAX98357A AUDIO INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0,
            I2S_ROLE_MASTER
        );

    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err =
        i2s_new_channel(
            &chan_cfg,
            &tx_handle,
            NULL
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "i2s_new_channel failed: %s",
            esp_err_to_name(err)
        );

        tx_handle = NULL;

        return err;
    }

    i2s_std_config_t std_cfg =
    {
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                AUDIO_SAMPLE_RATE
            ),

        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_MONO
            ),

        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,

            .bclk =
                (gpio_num_t)AUDIO_BCLK_GPIO,

            .ws =
                (gpio_num_t)AUDIO_WS_GPIO,

            .dout =
                (gpio_num_t)AUDIO_DOUT_GPIO,

            .din = I2S_GPIO_UNUSED,

            .invert_flags =
            {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    err =
        i2s_channel_init_std_mode(
            tx_handle,
            &std_cfg
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "i2s_channel_init_std_mode failed: %s",
            esp_err_to_name(err)
        );

        i2s_del_channel(tx_handle);

        tx_handle = NULL;

        return err;
    }

    audio_initialized = true;
    audio_enabled = false;

    err =
        audio_start_output_task();

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Audio output task failed: %s",
            esp_err_to_name(err)
        );

        i2s_del_channel(tx_handle);

        tx_handle = NULL;

        audio_initialized = false;

        return err;
    }

    ESP_LOGI(
        TAG,
        "MAX98357A initialized"
    );

    ESP_LOGI(
        TAG,
        "BCLK : GPIO%d",
        AUDIO_BCLK_GPIO
    );

    ESP_LOGI(
        TAG,
        "WS   : GPIO%d",
        AUDIO_WS_GPIO
    );

    ESP_LOGI(
        TAG,
        "DIN  : GPIO%d",
        AUDIO_DOUT_GPIO
    );

    ESP_LOGI(
        TAG,
        "Sample rate : %d Hz",
        AUDIO_SAMPLE_RATE
    );

    ESP_LOGI(
        TAG,
        "Format: 16-bit mono"
    );

    return ESP_OK;
}

/* =========================================================
 * PICO TTS SAMPLE CALLBACK
 *
 * IMPORTANT:
 * No I2S access from PicoTTS task.
 *
 * The callback copies PCM into the queue and waits until
 * the audio task consumes space.
 *
 * NO AUDIO DATA IS DROPPED.
 * ========================================================= */

static void pico_sample_callback(
    int16_t *buffer,
    unsigned count
)
{
    if (buffer == NULL || count == 0) {
        return;
    }

    if (
        !audio_initialized ||
        tts_pcm_queue == NULL
    ) {
        return;
    }

    unsigned offset = 0;

    while (offset < count) {

        unsigned remaining =
            count - offset;

        unsigned chunk_samples =
            remaining;

        if (
            chunk_samples >
            TTS_PCM_CHUNK_SAMPLES
        ) {
            chunk_samples =
                TTS_PCM_CHUNK_SAMPLES;
        }

        tts_pcm_chunk_t chunk;

        chunk.sample_count =
            (uint16_t)chunk_samples;

        memcpy(
            chunk.samples,
            &buffer[offset],
            ((size_t)chunk_samples) *
            sizeof(int16_t)
        );

        /*
         * IMPORTANT:
         *
         * Never silently drop audio.
         *
         * PicoTTS runs on CPU1 while the I2S output
         * task runs on CPU0, so blocking here gives
         * the output task time to drain PCM.
         */
        BaseType_t queued =
            xQueueSend(
                tts_pcm_queue,
                &chunk,
                pdMS_TO_TICKS(
                    TTS_QUEUE_WAIT_MS
                )
            );

        if (queued != pdTRUE) {

            ESP_LOGE(
                TAG,
                "TTS PCM queue timeout - audio data not accepted"
            );

            /*
             * Reset only on a real timeout.
             * This prevents stale partial audio from
             * being played on the next request.
             */
            xQueueReset(tts_pcm_queue);

            tts_generation_done = false;
            tts_busy = false;

            audio_stop_output();

            return;
        }

        offset += chunk_samples;
    }
}

/* =========================================================
 * TTS FINISHED CALLBACK
 * ========================================================= */

static void pico_tts_done_callback(void)
{
    ESP_LOGI(
        TAG,
        "TTS generation finished"
    );

    /*
     * DO NOT stop I2S here.
     *
     * PicoTTS generation and PCM playback are separate.
     * The output task will stop I2S after the queue is empty.
     */
    tts_generation_done = true;
}

/* =========================================================
 * TTS ERROR CALLBACK
 * ========================================================= */

static void pico_tts_error_callback(void)
{
    ESP_LOGE(
        TAG,
        "PicoTTS ERROR"
    );

    if (tts_pcm_queue != NULL) {

        xQueueReset(
            tts_pcm_queue
        );
    }

    tts_generation_done = false;
    tts_busy = false;

    audio_stop_output();
}

/* =========================================================
 * INITIALIZE PICO TTS
 * ========================================================= */

esp_err_t audio_tts_init(void)
{
    if (tts_initialized) {
        return ESP_OK;
    }

    if (!audio_initialized) {

        esp_err_t err =
            audio_init();

        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "PicoTTS INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    /*
     * PicoTTS is resource intensive.
     *
     * Run it on CPU1 so CPU0 remains available for
     * camera/display/audio output.
     */
    bool ok =
        picotts_init(
            5,
            pico_sample_callback,
            1
        );

    if (!ok) {

        ESP_LOGE(
            TAG,
            "picotts_init() FAILED"
        );

        return ESP_FAIL;
    }

    picotts_set_idle_notify(
        pico_tts_done_callback
    );

    picotts_set_error_notify(
        pico_tts_error_callback
    );

    tts_initialized = true;

    ESP_LOGI(
        TAG,
        "PicoTTS initialized successfully"
    );

    ESP_LOGI(
        TAG,
        "Dynamic English speech READY"
    );

    return ESP_OK;
}

/* =========================================================
 * SPEAK TEXT
 * ========================================================= */

esp_err_t audio_speak(const char *text)
{
    if (
        text == NULL ||
        text[0] == '\0'
    ) {

        ESP_LOGE(
            TAG,
            "audio_speak(): empty text"
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_initialized) {

        esp_err_t err =
            audio_init();

        if (err != ESP_OK) {
            return err;
        }
    }

    if (!tts_initialized) {

        esp_err_t err =
            audio_tts_init();

        if (err != ESP_OK) {
            return err;
        }
    }

    /*
     * Never allow another sentence to start while
     * previous PCM is still playing.
     */
    if (tts_busy) {

        ESP_LOGW(
            TAG,
            "TTS already speaking - request ignored"
        );

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Remove any stale PCM from an earlier failed
     * generation.
     */
    if (tts_pcm_queue != NULL) {

        xQueueReset(
            tts_pcm_queue
        );
    }

    tts_generation_done = false;
    tts_busy = true;

    ESP_LOGI(
        TAG,
        "TTS: %s",
        text
    );

    /*
     * IMPORTANT:
     *
     * PicoTTS expects the terminating '\0' as part of
     * the submitted message.
     */
    size_t text_length =
        strlen(text) + 1;

    picotts_add(
        text,
        text_length
    );

    return ESP_OK;
}

/* =========================================================
 * BUSY STATUS
 * ========================================================= */

bool audio_is_busy(void)
{
    return tts_busy;
}

/* =========================================================
 * WAV DATA CHUNK FINDER
 * ========================================================= */

static bool find_wav_data(
    FILE *file,
    uint32_t *data_size
)
{
    char chunk_id[4];

    while (true) {

        uint32_t chunk_size = 0;

        if (
            fread(
                chunk_id,
                1,
                4,
                file
            ) != 4
        ) {
            return false;
        }

        if (
            fread(
                &chunk_size,
                1,
                4,
                file
            ) != 4
        ) {
            return false;
        }

        if (
            memcmp(
                chunk_id,
                "data",
                4
            ) == 0
        ) {

            *data_size = chunk_size;

            return true;
        }

        if (
            fseek(
                file,
                (long)chunk_size,
                SEEK_CUR
            ) != 0
        ) {
            return false;
        }

        if (chunk_size & 1) {

            if (
                fseek(
                    file,
                    1,
                    SEEK_CUR
                ) != 0
            ) {
                return false;
            }
        }
    }
}

/* =========================================================
 * WAV PLAYBACK
 * ========================================================= */

static esp_err_t audio_play_wav(
    const char *path
)
{
    if (
        !audio_initialized ||
        tx_handle == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Do not start WAV playback while TTS is active.
     */
    if (tts_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file =
        fopen(path, "rb");

    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot open WAV: %s",
            path
        );

        return ESP_ERR_NOT_FOUND;
    }

    typedef struct
    {
        char riff[4];

        uint32_t file_size;

        char wave[4];

        char fmt[4];

        uint32_t fmt_size;

        uint16_t audio_format;

        uint16_t channels;

        uint32_t sample_rate;

        uint32_t byte_rate;

        uint16_t block_align;

        uint16_t bits_per_sample;

    } wav_header_t;

    wav_header_t header;

    memset(
        &header,
        0,
        sizeof(header)
    );

    if (
        fread(
            &header,
            1,
            sizeof(header),
            file
        ) != sizeof(header)
    ) {

        fclose(file);

        return ESP_ERR_INVALID_SIZE;
    }

    if (
        memcmp(
            header.riff,
            "RIFF",
            4
        ) != 0 ||

        memcmp(
            header.wave,
            "WAVE",
            4
        ) != 0
    ) {

        fclose(file);

        return ESP_ERR_INVALID_ARG;
    }

    if (header.audio_format != 1) {

        fclose(file);

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (header.channels != 1) {

        fclose(file);

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (
        header.sample_rate !=
        AUDIO_SAMPLE_RATE
    ) {

        fclose(file);

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (header.bits_per_sample != 16) {

        fclose(file);

        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t data_size = 0;

    if (
        !find_wav_data(
            file,
            &data_size
        )
    ) {

        fclose(file);

        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err =
        audio_start_output();

    if (err != ESP_OK) {

        fclose(file);

        return err;
    }

    uint8_t buffer[4096];

    uint32_t remaining =
        data_size;

    while (remaining > 0) {

        size_t bytes_to_read =
            remaining;

        if (
            bytes_to_read >
            sizeof(buffer)
        ) {
            bytes_to_read =
                sizeof(buffer);
        }

        size_t bytes_read =
            fread(
                buffer,
                1,
                bytes_to_read,
                file
            );

        if (bytes_read == 0) {

            fclose(file);

            audio_stop_output();

            return ESP_ERR_INVALID_SIZE;
        }

        size_t offset = 0;

        while (offset < bytes_read) {

            size_t bytes_written = 0;

            err =
                i2s_channel_write(
                    tx_handle,
                    buffer + offset,
                    bytes_read - offset,
                    &bytes_written,
                    portMAX_DELAY
                );

            if (err != ESP_OK) {

                fclose(file);

                audio_stop_output();

                return err;
            }

            if (bytes_written == 0) {

                fclose(file);

                audio_stop_output();

                return ESP_FAIL;
            }

            offset += bytes_written;
        }

        remaining -= bytes_read;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            TTS_FINAL_DRAIN_DELAY_MS
        )
    );

    fclose(file);

    audio_stop_output();

    return ESP_OK;
}

/* =========================================================
 * PUBLIC WAV TEST
 * ========================================================= */

esp_err_t audio_play_test(void)
{
    return audio_play_wav(
        AUDIO_WAV_PATH
    );
}