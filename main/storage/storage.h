#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mount the SPI flash storage partition as FATFS.
 *
 * Mount point:
 *
 *     /spiflash
 *
 * Partition:
 *
 *     storage
 */
esp_err_t storage_init(void);

/**
 * Check whether SPI flash storage is mounted.
 */
bool storage_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif