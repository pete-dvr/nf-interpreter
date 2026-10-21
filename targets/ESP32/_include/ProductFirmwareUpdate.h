//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef PRODUCT_FIRMWARE_UPDATE_H
#define PRODUCT_FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <esp_partition.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Generated from the final application.bin, before building its native companion.
// This is native read-only data, not a serialized package or an esp_app_desc_t.
typedef struct ProductReleaseDescriptor
{
    uint8_t targetId[16];
    uint32_t layoutId;
    uint8_t releaseId[16];
    uint32_t managedLength;
    uint8_t managedSha256[32];
} ProductReleaseDescriptor;

extern const ProductReleaseDescriptor g_ProductReleaseDescriptor;
// SEC1 uncompressed P-256 public point: 0x04 || X[32] || Y[32].
extern const uint8_t g_ProductOtaPublicKey[];
extern const size_t g_ProductOtaPublicKeyLength;

// Product codes are stable and distinct from SDK errors. The native log includes
// this code, the failing phase, and the original SDK/crypto/errno value.
enum ProductOtaError
{
    PRODUCT_OTA_ERR_NOT_READY = 0x7201,
    PRODUCT_OTA_ERR_BUSY = 0x7202,
    PRODUCT_OTA_ERR_LAYOUT = 0x7203,
    PRODUCT_OTA_ERR_STATE = 0x7204,
    PRODUCT_OTA_ERR_PATH = 0x7205,
    PRODUCT_OTA_ERR_IO = 0x7206,
    PRODUCT_OTA_ERR_HEADER = 0x7207,
    PRODUCT_OTA_ERR_SIGNATURE = 0x7208,
    PRODUCT_OTA_ERR_CRC = 0x7209,
    PRODUCT_OTA_ERR_HASH = 0x720A,
    PRODUCT_OTA_ERR_FLASH = 0x720B,
    PRODUCT_OTA_ERR_CANCELLED = 0x720C,
    PRODUCT_OTA_ERR_MEMORY = 0x720D,
    PRODUCT_OTA_ERR_DESCRIPTOR = 0x720E,
    PRODUCT_OTA_ERR_TAIL = 0x720F,
    PRODUCT_OTA_ERR_SPACE = 0x7210,
    PRODUCT_OTA_ERR_WATCHDOG = 0x7211,
    PRODUCT_OTA_ERR_TIMEOUT = 0x7212
};

const esp_partition_t *ProductOta_GetDeploymentPartition(const esp_partition_t *runtime);
// Call once per hardware boot, before mounting configuration or starting the CLR.
esp_err_t ProductOta_Initialize(void);
esp_err_t ProductOta_ValidateRunningPair(void);
// Startup failure only: invoke before allowing the CLR to execute.
void ProductOta_FailBoot(esp_err_t error) __attribute__((noreturn));
// Must precede filesystem unmount and managed deployment unmapping.
void ProductOta_PrepareSoftReboot(void);

// The worker owns a copy of all arguments. On success the chip restarts.
esp_err_t ProductOta_StartInstall(const char *path, uint32_t expectedCrc32, uint32_t *token);
// ESP_OK with done=false means keep yielding; only a terminal failure returns
// another error. Never interpret esp_err_t itself as a CLR HRESULT.
esp_err_t ProductOta_PollInstall(uint32_t token, bool *done);
// Relinquish caller ownership. An unfinished operation is cancelled and remains
// exclusive until its worker acknowledges cancellation and releases resources.
void ProductOta_ReleaseInstall(uint32_t token);
// A thread-safe text snapshot for managed exceptions even when SDK logs are off.
void ProductOta_GetLastError(char *buffer, size_t size);
esp_err_t ProductOta_GetStagingFreeBytes(uint64_t *freeBytes);
esp_err_t ProductOta_GetBootStatus(char *buffer, size_t size);
esp_err_t ProductOta_IsTrialBoot(bool *isTrial);
esp_err_t ProductOta_ConfirmBoot(void);
esp_err_t ProductOta_RejectBootAndRestart(void);

#ifdef __cplusplus
}
#endif
#endif
