#include "tft.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "TFT";


/* =========================================================
 * ILI9341 PINS
 * ========================================================= */

#define TFT_RST     21
#define TFT_DC      38
#define TFT_CS      39
#define TFT_MISO    40
#define TFT_MOSI    41
#define TFT_SCK     42


#define TFT_SPI_HOST       SPI2_HOST
#define TFT_SPI_FREQ_HZ    27000000


/* =========================================================
 * TFT DISPLAY
 * ========================================================= */

#define TFT_WIDTH      240
#define TFT_HEIGHT     320


/* =========================================================
 * CAMERA FRAME
 * ========================================================= */

#define CAMERA_WIDTH    320
#define CAMERA_HEIGHT   240


/* =========================================================
 * SPI TRANSFER
 *
 * 8 TFT lines per transfer
 *
 * 240 pixels
 * x 2 bytes
 * x 8 lines
 *
 * = 3840 bytes
 * ========================================================= */

#define TFT_LINES_PER_TRANSFER  8

#define TFT_TRANSFER_SIZE \
    (TFT_WIDTH * 2 * TFT_LINES_PER_TRANSFER)


/* =========================================================
 * ILI9341 COMMANDS
 * ========================================================= */

#define ILI9341_SWRESET       0x01
#define ILI9341_SLPOUT        0x11

#define ILI9341_INVOFF        0x20
#define ILI9341_INVON         0x21

#define ILI9341_DISPON        0x29

#define ILI9341_CASET         0x2A
#define ILI9341_PASET         0x2B
#define ILI9341_RAMWR         0x2C

#define ILI9341_MADCTL        0x36
#define ILI9341_PIXFMT        0x3A

#define ILI9341_FRMCTR1       0xB1
#define ILI9341_DFUNCTR       0xB6

#define ILI9341_PWCTR1        0xC0
#define ILI9341_PWCTR2        0xC1
#define ILI9341_VMCTR1        0xC5
#define ILI9341_VMCTR2        0xC7

#define ILI9341_GMCTRP1       0xE0
#define ILI9341_GMCTRN1       0xE1


/* =========================================================
 * SPI DEVICE
 * ========================================================= */

static spi_device_handle_t tft_spi = NULL;


/* =========================================================
 * LINE BUFFER
 * ========================================================= */

static uint8_t line_buffer[TFT_TRANSFER_SIZE];


/* =========================================================
 * DC CONTROL
 * ========================================================= */

static inline void tft_dc_command(void)
{
    gpio_set_level(TFT_DC, 0);
}


static inline void tft_dc_data(void)
{
    gpio_set_level(TFT_DC, 1);
}


/* =========================================================
 * WRITE COMMAND
 * ========================================================= */

static esp_err_t tft_write_command(uint8_t command)
{
    spi_transaction_t transaction;

    memset(
        &transaction,
        0,
        sizeof(transaction)
    );

    transaction.length = 8;
    transaction.tx_buffer = &command;

    tft_dc_command();

    return spi_device_transmit(
        tft_spi,
        &transaction
    );
}


/* =========================================================
 * WRITE DATA
 * ========================================================= */

static esp_err_t tft_write_data(
    const uint8_t *data,
    size_t length
)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t transaction;

    memset(
        &transaction,
        0,
        sizeof(transaction)
    );

    transaction.length = length * 8;
    transaction.tx_buffer = data;

    tft_dc_data();

    return spi_device_transmit(
        tft_spi,
        &transaction
    );
}


/* =========================================================
 * WRITE COMMAND + DATA
 * ========================================================= */

static esp_err_t tft_write_command_data(
    uint8_t command,
    const uint8_t *data,
    size_t length
)
{
    esp_err_t ret;

    ret = tft_write_command(command);

    if (ret != ESP_OK) {
        return ret;
    }

    if (data != NULL && length > 0) {

        ret = tft_write_data(
            data,
            length
        );
    }

    return ret;
}


/* =========================================================
 * SET ADDRESS WINDOW
 * ========================================================= */

static esp_err_t tft_set_address_window(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1
)
{
    uint8_t data[4];

    /* -----------------------------------------------------
     * COLUMN ADDRESS SET
     * ----------------------------------------------------- */

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)(x0 & 0xFF);
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)(x1 & 0xFF);

    esp_err_t ret =
        tft_write_command_data(
            ILI9341_CASET,
            data,
            4
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * PAGE ADDRESS SET
     * ----------------------------------------------------- */

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)(y0 & 0xFF);
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)(y1 & 0xFF);

    ret =
        tft_write_command_data(
            ILI9341_PASET,
            data,
            4
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * MEMORY WRITE
     * ----------------------------------------------------- */

    return tft_write_command(
        ILI9341_RAMWR
    );
}


/* =========================================================
 * HARDWARE RESET
 * ========================================================= */

static void tft_hardware_reset(void)
{
    gpio_set_level(
        TFT_RST,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(10)
    );


    gpio_set_level(
        TFT_RST,
        0
    );

    vTaskDelay(
        pdMS_TO_TICKS(20)
    );


    gpio_set_level(
        TFT_RST,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );
}


/* =========================================================
 * ILI9341 CONTROLLER INITIALIZATION
 * ========================================================= */

static esp_err_t tft_controller_init(void)
{
    esp_err_t ret;


    /* -----------------------------------------------------
     * SOFTWARE RESET
     * ----------------------------------------------------- */

    ret = tft_write_command(
        ILI9341_SWRESET
    );

    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );


    /* -----------------------------------------------------
     * SLEEP OUT
     * ----------------------------------------------------- */

    ret = tft_write_command(
        ILI9341_SLPOUT
    );

    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(
        pdMS_TO_TICKS(120)
    );


    /* -----------------------------------------------------
     * PIXEL FORMAT
     *
     * 16-bit RGB565
     * ----------------------------------------------------- */

    uint8_t pixel_format = 0x55;

    ret =
        tft_write_command_data(
            ILI9341_PIXFMT,
            &pixel_format,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * MEMORY ACCESS CONTROL
     *
     * 0x48
     *
     * MX  = 1
     * MY  = 0
     * MV  = 0
     * BGR = 1
     *
     * Portrait orientation
     * ----------------------------------------------------- */

    uint8_t madctl = 0x48;

    ret =
        tft_write_command_data(
            ILI9341_MADCTL,
            &madctl,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * IMPORTANT
     *
     * Explicitly disable display inversion.
     *
     * This prevents a negative-looking image if the
     * controller/panel power-up state is inverted.
     * ----------------------------------------------------- */

    ret =
        tft_write_command(
            ILI9341_INVOFF
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * FRAME RATE
     * ----------------------------------------------------- */

    uint8_t frmctr1[] = {
        0x00,
        0x1B
    };

    ret =
        tft_write_command_data(
            ILI9341_FRMCTR1,
            frmctr1,
            sizeof(frmctr1)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * POWER CONTROL 1
     * ----------------------------------------------------- */

    uint8_t pwctr1 = 0x23;

    ret =
        tft_write_command_data(
            ILI9341_PWCTR1,
            &pwctr1,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * POWER CONTROL 2
     * ----------------------------------------------------- */

    uint8_t pwctr2 = 0x10;

    ret =
        tft_write_command_data(
            ILI9341_PWCTR2,
            &pwctr2,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * VCOM CONTROL 1
     * ----------------------------------------------------- */

    uint8_t vmctr1[] = {
        0x3E,
        0x28
    };

    ret =
        tft_write_command_data(
            ILI9341_VMCTR1,
            vmctr1,
            sizeof(vmctr1)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * VCOM CONTROL 2
     * ----------------------------------------------------- */

    uint8_t vmctr2 = 0x86;

    ret =
        tft_write_command_data(
            ILI9341_VMCTR2,
            &vmctr2,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * DISPLAY FUNCTION CONTROL
     * ----------------------------------------------------- */

    uint8_t dfunc[] = {
        0x08,
        0x82,
        0x27
    };

    ret =
        tft_write_command_data(
            ILI9341_DFUNCTR,
            dfunc,
            sizeof(dfunc)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * POSITIVE GAMMA
     * ----------------------------------------------------- */

    uint8_t gamma_positive[] = {
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

    ret =
        tft_write_command_data(
            ILI9341_GMCTRP1,
            gamma_positive,
            sizeof(gamma_positive)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * NEGATIVE GAMMA
     * ----------------------------------------------------- */

    uint8_t gamma_negative[] = {
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

    ret =
        tft_write_command_data(
            ILI9341_GMCTRN1,
            gamma_negative,
            sizeof(gamma_negative)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /* -----------------------------------------------------
     * DISPLAY ON
     * ----------------------------------------------------- */

    ret =
        tft_write_command(
            ILI9341_DISPON
        );

    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(
        pdMS_TO_TICKS(100)
    );


    return ESP_OK;
}


/* =========================================================
 * TFT INITIALIZATION
 * ========================================================= */

esp_err_t tft_init(void)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "        ILI9341 TFT INITIALIZATION"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    /* -----------------------------------------------------
     * GPIO
     * ----------------------------------------------------- */

    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << TFT_RST) |
            (1ULL << TFT_DC),

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };


    esp_err_t ret =
        gpio_config(
            &io_conf
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GPIO configuration failed"
        );

        return ret;
    }


    gpio_set_level(
        TFT_RST,
        1
    );

    gpio_set_level(
        TFT_DC,
        1
    );


    /* -----------------------------------------------------
     * SPI BUS
     * ----------------------------------------------------- */

    spi_bus_config_t bus_config = {

        .mosi_io_num =
            TFT_MOSI,

        .miso_io_num =
            TFT_MISO,

        .sclk_io_num =
            TFT_SCK,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .max_transfer_sz =
            TFT_TRANSFER_SIZE
    };


    ret =
        spi_bus_initialize(
            TFT_SPI_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        );


    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(
            TAG,
            "SPI bus initialization failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* -----------------------------------------------------
     * SPI DEVICE
     * ----------------------------------------------------- */

    spi_device_interface_config_t device_config = {

        .clock_speed_hz =
            TFT_SPI_FREQ_HZ,

        .mode =
            0,

        .spics_io_num =
            TFT_CS,

        .queue_size =
            1,

        .pre_cb =
            NULL,

        .post_cb =
            NULL
    };


    ret =
        spi_bus_add_device(
            TFT_SPI_HOST,
            &device_config,
            &tft_spi
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SPI device initialization failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "SPI frequency: %d Hz",
        TFT_SPI_FREQ_HZ
    );


    /* -----------------------------------------------------
     * HARDWARE RESET
     * ----------------------------------------------------- */

    tft_hardware_reset();


    /* -----------------------------------------------------
     * CONTROLLER INITIALIZATION
     * ----------------------------------------------------- */

    ret =
        tft_controller_init();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "ILI9341 controller initialization failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* -----------------------------------------------------
     * CLEAR DISPLAY
     * ----------------------------------------------------- */

    tft_clear(
        0x0000
    );


    ESP_LOGI(
        TAG,
        "Display resolution: 240x320"
    );

    ESP_LOGI(
        TAG,
        "ILI9341 initialization PASSED"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}


/* =========================================================
 * CLEAR SCREEN
 * ========================================================= */

void tft_clear(uint16_t color)
{
    if (tft_spi == NULL) {
        return;
    }


    esp_err_t ret =
        tft_set_address_window(
            0,
            0,
            TFT_WIDTH - 1,
            TFT_HEIGHT - 1
        );


    if (ret != ESP_OK) {
        return;
    }


    /* -----------------------------------------------------
     * Prepare 8 identical TFT lines
     * ----------------------------------------------------- */

    for (
        int y = 0;
        y < TFT_LINES_PER_TRANSFER;
        y++
    ) {

        for (
            int x = 0;
            x < TFT_WIDTH;
            x++
        ) {

            int index =
                (y * TFT_WIDTH + x) * 2;


            line_buffer[index] =
                (uint8_t)(color >> 8);

            line_buffer[index + 1] =
                (uint8_t)(color & 0xFF);
        }
    }


    tft_dc_data();


    /* -----------------------------------------------------
     * Send complete display
     * ----------------------------------------------------- */

    for (
        int y = 0;
        y < TFT_HEIGHT;
        y += TFT_LINES_PER_TRANSFER
    ) {

        spi_transaction_t transaction;

        memset(
            &transaction,
            0,
            sizeof(transaction)
        );


        transaction.length =
            TFT_TRANSFER_SIZE * 8;


        transaction.tx_buffer =
            line_buffer;


        ret =
            spi_device_transmit(
                tft_spi,
                &transaction
            );


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "TFT clear transfer failed"
            );

            return;
        }
    }
}


/* =========================================================
 * CAMERA FRAME → TFT
 *
 * CAMERA:
 *     320 x 240
 *
 * TFT:
 *     240 x 320
 *
 * Rotation:
 *     90 degrees
 *
 * IMPORTANT:
 *     Camera RGB565 framebuffer is treated as
 *     RAW BYTES.
 *
 * We DO NOT cast fb->buf to uint16_t.
 *
 * This preserves the exact byte order produced
 * by the camera driver.
 * ========================================================= */

bool tft_display_camera_frame(
    const camera_fb_t *fb
)
{
    if (
        tft_spi == NULL ||
        fb == NULL
    ) {
        return false;
    }


    /* -----------------------------------------------------
     * Verify format
     * ----------------------------------------------------- */

    if (
        fb->format != PIXFORMAT_RGB565
    ) {

        ESP_LOGE(
            TAG,
            "Unsupported camera format: %d",
            fb->format
        );

        return false;
    }


    /* -----------------------------------------------------
     * Verify resolution
     * ----------------------------------------------------- */

    if (
        fb->width != CAMERA_WIDTH ||
        fb->height != CAMERA_HEIGHT
    ) {

        ESP_LOGE(
            TAG,
            "Unexpected frame size: %ux%u",
            fb->width,
            fb->height
        );

        return false;
    }


    /*
     * IMPORTANT:
     *
     * Read camera buffer as BYTE ARRAY.
     *
     * Do not use:
     *
     *     uint16_t *src
     *
     * because ESP32-S3 is little-endian and RGB565
     * framebuffer byte ordering must be preserved.
     */

    const uint8_t *src =
        (const uint8_t *)fb->buf;


    /* -----------------------------------------------------
     * Full TFT window
     * ----------------------------------------------------- */

    esp_err_t ret =
        tft_set_address_window(
            0,
            0,
            TFT_WIDTH - 1,
            TFT_HEIGHT - 1
        );


    if (ret != ESP_OK) {
        return false;
    }


    tft_dc_data();


    /* =====================================================
     *
     * CAMERA → TFT ROTATION
     *
     * Camera:
     *
     *     X = 0 ... 319
     *     Y = 0 ... 239
     *
     *
     * TFT:
     *
     *     X = 0 ... 239
     *     Y = 0 ... 319
     *
     *
     * Mapping:
     *
     *     TFT X
     *       ↓
     *     Camera Y reversed
     *
     *
     *     TFT Y
     *       ↓
     *     Camera X
     *
     *
     * Therefore:
     *
     *     camera_x = display_y
     *
     *     camera_y = 239 - display_x
     *
     * ===================================================== */


    for (
        int block_y = 0;
        block_y < TFT_HEIGHT;
        block_y += TFT_LINES_PER_TRANSFER
    ) {

        int lines =
            TFT_HEIGHT - block_y;


        if (
            lines >
            TFT_LINES_PER_TRANSFER
        ) {
            lines =
                TFT_LINES_PER_TRANSFER;
        }


        int buffer_index = 0;


        /* -------------------------------------------------
         * Build 8 TFT lines
         * ------------------------------------------------- */

        for (
            int y = 0;
            y < lines;
            y++
        ) {

            int display_y =
                block_y + y;


            /*
             * TFT Y = Camera X
             *
             * Range:
             *
             * 0 ... 319
             */

            int camera_x =
                display_y;


            for (
                int x = 0;
                x < TFT_WIDTH;
                x++
            ) {

                /*
                 * TFT X:
                 *
                 * 0 ... 239
                 *
                 * Camera Y:
                 *
                 * 239 ... 0
                 */

                int camera_y =
                    (CAMERA_HEIGHT - 1) - x;


                /*
                 * RGB565 pixel position.
                 *
                 * IMPORTANT:
                 *
                 * We directly copy the TWO BYTES
                 * from the camera framebuffer.
                 *
                 * No uint16_t conversion.
                 * No manual byte swap.
                 */

                size_t pixel_index =
                    (
                        (
                            camera_y *
                            CAMERA_WIDTH
                        )
                        +
                        camera_x
                    ) * 2;


                line_buffer[buffer_index++] =
                    src[pixel_index];


                line_buffer[buffer_index++] =
                    src[pixel_index + 1];
            }
        }


        /* -------------------------------------------------
         * Send this block to ILI9341
         * ------------------------------------------------- */

        spi_transaction_t transaction;

        memset(
            &transaction,
            0,
            sizeof(transaction)
        );


        transaction.length =
            buffer_index * 8;


        transaction.tx_buffer =
            line_buffer;


        ret =
            spi_device_transmit(
                tft_spi,
                &transaction
            );


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Camera frame SPI transfer failed: %s",
                esp_err_to_name(ret)
            );

            return false;
        }
    }


    return true;
}