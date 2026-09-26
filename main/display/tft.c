#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_camera.h"

#include "tft.h"

static const char *TAG = "TFT";

/* ============================================================
 * TFT PINS
 * ============================================================ */

#define TFT_RST     21
#define TFT_DC      38
#define TFT_CS      39
#define TFT_MISO    40
#define TFT_MOSI    41
#define TFT_SCK     42

/* ============================================================
 * TFT SIZE
 * ============================================================ */

#define TFT_WIDTH   240
#define TFT_HEIGHT  320

/* ============================================================
 * CAMERA SIZE
 * ============================================================ */

#define CAMERA_WIDTH    320
#define CAMERA_HEIGHT   240

/* ============================================================
 * SPI
 * ============================================================ */

#define TFT_SPI_HOST            SPI2_HOST
#define TFT_SPI_HZ              20000000
#define TFT_ROW_BYTES           (TFT_WIDTH * 2)
#define TFT_MAX_TRANSFER        4096

static spi_device_handle_t tft_spi = NULL;

/*
 * DMA-capable internal RAM.
 *
 * One TFT row:
 *
 * 240 pixels × 2 bytes = 480 bytes
 */
static uint8_t *tx_buffer = NULL;


/* ============================================================
 * GPIO
 * ============================================================ */

static inline void tft_cs_low(void)
{
    gpio_set_level(TFT_CS, 0);
}

static inline void tft_cs_high(void)
{
    gpio_set_level(TFT_CS, 1);
}

static inline void tft_dc_command(void)
{
    gpio_set_level(TFT_DC, 0);
}

static inline void tft_dc_data(void)
{
    gpio_set_level(TFT_DC, 1);
}


/* ============================================================
 * SPI TRANSFER
 * ============================================================ */

static esp_err_t tft_spi_transfer(
    const uint8_t *buffer,
    size_t length)
{
    if (tft_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t trans;
    memset(&trans, 0, sizeof(trans));

    trans.length = length * 8;
    trans.tx_buffer = buffer;

    return spi_device_polling_transmit(
        tft_spi,
        &trans
    );
}


/* ============================================================
 * WRITE COMMAND
 * ============================================================ */

static esp_err_t tft_write_command(uint8_t command)
{
    spi_transaction_t trans;
    memset(&trans, 0, sizeof(trans));

    tx_buffer[0] = command;

    trans.length = 8;
    trans.tx_buffer = tx_buffer;

    tft_dc_command();
    tft_cs_low();

    esp_err_t err =
        spi_device_polling_transmit(
            tft_spi,
            &trans
        );

    tft_cs_high();

    return err;
}


/* ============================================================
 * WRITE DATA
 * ============================================================ */

static esp_err_t tft_write_data(
    const uint8_t *data,
    size_t length)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    tft_dc_data();
    tft_cs_low();

    esp_err_t err =
        tft_spi_transfer(
            data,
            length
        );

    tft_cs_high();

    return err;
}


/* ============================================================
 * COMMAND + DATA
 * ============================================================ */

static esp_err_t tft_write_command_data(
    uint8_t command,
    const uint8_t *data,
    size_t length)
{
    esp_err_t err;

    err = tft_write_command(command);

    if (err != ESP_OK) {
        return err;
    }

    if (length > 0) {
        err = tft_write_data(
            data,
            length
        );
    }

    return err;
}


/* ============================================================
 * ADDRESS WINDOW
 *
 * IMPORTANT:
 *
 * MADCTL = 0x48
 *
 * Therefore controller remains:
 *
 * 240 columns × 320 rows
 *
 * No MV rotation inside controller.
 * Rotation is done in software.
 * ============================================================ */

static esp_err_t tft_set_address_window(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1)
{
    uint8_t data[4];

    esp_err_t err;

    /* CASET */
    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)(x0 & 0xFF);
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)(x1 & 0xFF);

    err = tft_write_command_data(
        0x2A,
        data,
        4
    );

    if (err != ESP_OK) {
        return err;
    }

    /* PASET */
    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)(y0 & 0xFF);
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)(y1 & 0xFF);

    err = tft_write_command_data(
        0x2B,
        data,
        4
    );

    if (err != ESP_OK) {
        return err;
    }

    /* RAMWR */
    return tft_write_command(0x2C);
}


/* ============================================================
 * HARDWARE RESET
 * ============================================================ */

static void tft_hardware_reset(void)
{
    gpio_set_level(TFT_RST, 1);

    vTaskDelay(
        pdMS_TO_TICKS(20)
    );

    gpio_set_level(TFT_RST, 0);

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );

    gpio_set_level(TFT_RST, 1);

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );
}


/* ============================================================
 * ILI9341 INIT
 * ============================================================ */

static esp_err_t tft_controller_init(void)
{
    esp_err_t err;

    /* Software reset */
    err = tft_write_command(0x01);

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );

    /* --------------------------------------------------------
     * Pixel format: RGB565
     * -------------------------------------------------------- */

    {
        uint8_t data = 0x55;

        err = tft_write_command_data(
            0x3A,
            &data,
            1
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* --------------------------------------------------------
     * MADCTL
     *
     * 0x48:
     *
     * MX  = 1
     * MV  = 0
     * BGR = 1
     *
     * IMPORTANT:
     *
     * NO MV.
     *
     * Therefore TFT remains native:
     *
     * 240 × 320
     *
     * Software performs camera rotation.
     * -------------------------------------------------------- */

    {
        uint8_t data = 0x48;

        err = tft_write_command_data(
            0x36,
            &data,
            1
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Frame rate */
    {
        uint8_t data[] = {
            0x00,
            0x1B
        };

        err = tft_write_command_data(
            0xB1,
            data,
            sizeof(data)
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Display function */
    {
        uint8_t data[] = {
            0x0A,
            0x82,
            0x27
        };

        err = tft_write_command_data(
            0xB6,
            data,
            sizeof(data)
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Power control 1 */
    {
        uint8_t data = 0x23;

        err = tft_write_command_data(
            0xC0,
            &data,
            1
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Power control 2 */
    {
        uint8_t data = 0x10;

        err = tft_write_command_data(
            0xC1,
            &data,
            1
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* VCOM */
    {
        uint8_t data[] = {
            0x3E,
            0x28
        };

        err = tft_write_command_data(
            0xC5,
            data,
            sizeof(data)
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    {
        uint8_t data = 0x86;

        err = tft_write_command_data(
            0xC7,
            &data,
            1
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Positive gamma */
    {
        uint8_t data[] = {
            0x0F,
            0x31,
            0x2B,
            0x0C,
            0x0E,
            0x08,
            0x4E,
            0xF1,
            0x37,
            0x07,
            0x10,
            0x03,
            0x0E,
            0x09,
            0x00
        };

        err = tft_write_command_data(
            0xE0,
            data,
            sizeof(data)
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Negative gamma */
    {
        uint8_t data[] = {
            0x00,
            0x0E,
            0x14,
            0x03,
            0x11,
            0x07,
            0x31,
            0xC1,
            0x48,
            0x08,
            0x0F,
            0x0C,
            0x31,
            0x36,
            0x0F
        };

        err = tft_write_command_data(
            0xE1,
            data,
            sizeof(data)
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    /* Sleep OUT */
    err = tft_write_command(0x11);

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );

    /* Display ON */
    err = tft_write_command(0x29);

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(20)
    );

    ESP_LOGI(
        TAG,
        "ILI9341 initialization PASSED"
    );

    return ESP_OK;
}


/* ============================================================
 * TFT INIT
 * ============================================================ */

esp_err_t tft_init(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "ILI9341 TFT INITIALIZATION");
    ESP_LOGI(TAG, "=================================");

    ESP_LOGI(
        TAG,
        "SPI frequency: %d Hz",
        TFT_SPI_HZ
    );

    ESP_LOGI(
        TAG,
        "SPI DMA: ENABLED"
    );

    ESP_LOGI(
        TAG,
        "SPI mode: 0"
    );

    ESP_LOGI(
        TAG,
        "CS: MANUAL GPIO %d",
        TFT_CS
    );

    ESP_LOGI(
        TAG,
        "Display resolution: %dx%d",
        TFT_WIDTH,
        TFT_HEIGHT
    );

    ESP_LOGI(
        TAG,
        "Orientation: PORTRAIT"
    );

    ESP_LOGI(
        TAG,
        "MADCTL: 0x48"
    );

    /* --------------------------------------------------------
     * GPIO
     * -------------------------------------------------------- */

    gpio_config_t gpio_cfg;

    memset(
        &gpio_cfg,
        0,
        sizeof(gpio_cfg)
    );

    gpio_cfg.mode = GPIO_MODE_OUTPUT;

    gpio_cfg.pin_bit_mask =
        (1ULL << TFT_RST) |
        (1ULL << TFT_DC) |
        (1ULL << TFT_CS);

    gpio_cfg.pull_down_en =
        GPIO_PULLDOWN_DISABLE;

    gpio_cfg.pull_up_en =
        GPIO_PULLUP_DISABLE;

    gpio_cfg.intr_type =
        GPIO_INTR_DISABLE;

    err = gpio_config(
        &gpio_cfg
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    gpio_set_level(TFT_CS, 1);
    gpio_set_level(TFT_DC, 1);
    gpio_set_level(TFT_RST, 1);

    /* --------------------------------------------------------
     * SPI BUS
     * -------------------------------------------------------- */

    spi_bus_config_t bus_cfg;

    memset(
        &bus_cfg,
        0,
        sizeof(bus_cfg)
    );

    bus_cfg.mosi_io_num = TFT_MOSI;
    bus_cfg.miso_io_num = TFT_MISO;
    bus_cfg.sclk_io_num = TFT_SCK;

    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;

    bus_cfg.max_transfer_sz =
        TFT_MAX_TRANSFER;

    err = spi_bus_initialize(
        TFT_SPI_HOST,
        &bus_cfg,
        SPI_DMA_CH_AUTO
    );

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(
            TAG,
            "SPI bus initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /* --------------------------------------------------------
     * SPI DEVICE
     * -------------------------------------------------------- */

    spi_device_interface_config_t dev_cfg;

    memset(
        &dev_cfg,
        0,
        sizeof(dev_cfg)
    );

    dev_cfg.clock_speed_hz =
        TFT_SPI_HZ;

    dev_cfg.mode = 0;

    /*
     * Manual CS.
     */
    dev_cfg.spics_io_num = -1;

    dev_cfg.queue_size = 1;

    err = spi_bus_add_device(
        TFT_SPI_HOST,
        &dev_cfg,
        &tft_spi
    );

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(
            TAG,
            "SPI device initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /* --------------------------------------------------------
     * DMA BUFFER
     * -------------------------------------------------------- */

    tx_buffer = heap_caps_malloc(
        TFT_MAX_TRANSFER,
        MALLOC_CAP_DMA |
        MALLOC_CAP_INTERNAL |
        MALLOC_CAP_8BIT
    );

    if (tx_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "DMA buffer allocation failed"
        );

        return ESP_ERR_NO_MEM;
    }

    memset(
        tx_buffer,
        0,
        TFT_MAX_TRANSFER
    );

    /* --------------------------------------------------------
     * RESET
     * -------------------------------------------------------- */

    tft_hardware_reset();

    /* --------------------------------------------------------
     * CONTROLLER
     * -------------------------------------------------------- */

    err = tft_controller_init();

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "ILI9341 initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "TFT initialized successfully"
    );

    return ESP_OK;
}


/* ============================================================
 * CLEAR SCREEN
 * ============================================================ */

void tft_clear(uint16_t color)
{
    /*
     * Build one complete 240-pixel row.
     */
    for (int x = 0; x < TFT_WIDTH; x++) {

        tx_buffer[x * 2] =
            (uint8_t)(color >> 8);

        tx_buffer[x * 2 + 1] =
            (uint8_t)(color & 0xFF);
    }

    esp_err_t err =
        tft_set_address_window(
            0,
            0,
            TFT_WIDTH - 1,
            TFT_HEIGHT - 1
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Clear address window failed: %s",
            esp_err_to_name(err)
        );

        return;
    }

    tft_dc_data();
    tft_cs_low();

    for (int y = 0; y < TFT_HEIGHT; y++) {

        err =
            tft_spi_transfer(
                tx_buffer,
                TFT_ROW_BYTES
            );

        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "TFT clear row %d failed: %s",
                y,
                esp_err_to_name(err)
            );

            break;
        }
    }

    tft_cs_high();
}


/* ============================================================
 * CAMERA → TFT
 *
 * CAMERA:
 *      320 × 240
 *
 * TFT:
 *      240 × 320
 *
 * SOFTWARE 90° ROTATION
 *
 * No MV in MADCTL.
 * No controller-side coordinate rotation.
 *
 * ------------------------------------------------------------
 *
 * display_x = camera_y
 * display_y = camera_width - 1 - camera_x
 *
 * Therefore:
 *
 * camera_x = 319 - display_y
 * camera_y = display_x
 *
 * This produces a fixed 240×320 portrait frame.
 * ============================================================ */

bool tft_display_camera_frame(
    const camera_fb_t *fb)
{
    if (fb == NULL) {

        ESP_LOGE(
            TAG,
            "Camera frame NULL"
        );

        return false;
    }

    if (fb->buf == NULL) {

        ESP_LOGE(
            TAG,
            "Camera buffer NULL"
        );

        return false;
    }

    if (fb->width != CAMERA_WIDTH ||
        fb->height != CAMERA_HEIGHT) {

        ESP_LOGE(
            TAG,
            "Unexpected camera size: %ux%u",
            fb->width,
            fb->height
        );

        return false;
    }

    if (fb->format != PIXFORMAT_RGB565) {

        ESP_LOGE(
            TAG,
            "Unexpected pixel format: %d",
            fb->format
        );

        return false;
    }

    if (fb->len <
        CAMERA_WIDTH *
        CAMERA_HEIGHT *
        2) {

        ESP_LOGE(
            TAG,
            "Invalid frame length: %u",
            (unsigned)fb->len
        );

        return false;
    }

    /*
     * IMPORTANT:
     *
     * TFT controller remains native 240×320.
     *
     * Set exactly one full-screen window.
     */
    esp_err_t err =
        tft_set_address_window(
            0,
            0,
            239,
            319
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Address window failed: %s",
            esp_err_to_name(err)
        );

        return false;
    }

    tft_dc_data();
    tft_cs_low();

    /*
     * Generate one complete TFT row at a time.
     *
     * TFT row:
     *     240 pixels
     *
     * Camera:
     *     320 × 240
     *
     * Software rotation:
     *
     * camera_x = 319 - display_y
     * camera_y = display_x
     */
    for (int display_y = 0;
         display_y < TFT_HEIGHT;
         display_y++) {

        int camera_x =
            (CAMERA_WIDTH - 1) -
            display_y;

        for (int display_x = 0;
             display_x < TFT_WIDTH;
             display_x++) {

            int camera_y =
                display_x;

            size_t index =
                ((size_t)camera_y *
                 CAMERA_WIDTH +
                 camera_x) * 2;

            /*
             * RGB565 byte order preserved.
             */
            tx_buffer[display_x * 2] =
                fb->buf[index];

            tx_buffer[display_x * 2 + 1] =
                fb->buf[index + 1];
        }

        /*
         * 480-byte DMA transfer.
         */
        err =
            tft_spi_transfer(
                tx_buffer,
                TFT_ROW_BYTES
            );

        if (err != ESP_OK) {

            tft_cs_high();

            ESP_LOGE(
                TAG,
                "Frame transfer failed at row %d: %s",
                display_y,
                esp_err_to_name(err)
            );

            return false;
        }
    }

    tft_cs_high();

    return true;
}