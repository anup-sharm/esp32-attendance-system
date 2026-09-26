#ifndef DATABASE_RESET_H
#define DATABASE_RESET_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATABASE_RESET_VERSION 2

esp_err_t database_reset_init(void);

#ifdef __cplusplus
}
#endif

#endif