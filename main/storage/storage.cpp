#include "storage.h"

#include "esp_log.h"

#include "spiflash_fatfs.hpp"


static const char *TAG = "STORAGE";


/* ============================================================
 * STORAGE STATUS
 * ============================================================ */

static bool storage_mounted = false;


/* ============================================================
 * STORAGE INITIALIZATION
 * ============================================================ */

extern "C" esp_err_t storage_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "       SPI FLASH STORAGE INITIALIZATION");
    ESP_LOGI(TAG, "========================================");

    storage_mounted = false;


    ESP_LOGI(
        TAG,
        "Mount point: /spiflash"
    );

    ESP_LOGI(
        TAG,
        "Partition: storage"
    );

    ESP_LOGI(
        TAG,
        "Filesystem: FATFS"
    );


    /* --------------------------------------------------------
     * MOUNT STORAGE
     * -------------------------------------------------------- */

    esp_err_t ret =
        fatfs_flash_mount();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SPI flash FATFS mount FAILED: %s",
            esp_err_to_name(ret)
        );

        storage_mounted = false;

        return ret;
    }


    /* --------------------------------------------------------
     * STORAGE READY
     * -------------------------------------------------------- */

    storage_mounted = true;


    ESP_LOGI(
        TAG,
        "SPI flash FATFS mount PASSED"
    );

    ESP_LOGI(
        TAG,
        "Storage available at /spiflash"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    return ESP_OK;
}


/* ============================================================
 * STORAGE STATUS
 * ============================================================ */

extern "C" bool storage_is_mounted(void)
{
    return storage_mounted;
}