#pragma once
#include <esp_err.h>
#include <stdint.h>
typedef unsigned int nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_REMOVE_FAILED 0x1108
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
