//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef PRODUCT_DEVICE_TELEMETRY_H
#define PRODUCT_DEVICE_TELEMETRY_H

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Reports the internal silicon temperature, not ambient temperature. The IDF
// driver automatically selects the analog range within -40 through 125 C.
// On failure, leaves *celsius unchanged and returns the original SDK error.
// Call from a task, never an interrupt. Safe across managed soft restarts.
esp_err_t ProductTelemetry_GetInternalTemperatureCelsius(float *celsius);

// Initialize once at hardware boot, after nvs_flash_init and before CLR startup.
// A brownout is persisted until acknowledged; normal boots preserve any pending
// event. Get/Acknowledge retry initialization if an earlier NVS operation failed.
esp_err_t ProductTelemetry_InitializePowerStatus(void);
// 1 = OK, 2 = an unreported brownout. Leaves output unchanged on storage failure.
esp_err_t ProductTelemetry_GetPowerStatus(int32_t *status);
// Call only after successful transmission. Storage errors retain the in-memory
// latch, but NVS may already have persisted the clear before returning an error.
esp_err_t ProductTelemetry_AcknowledgePowerStatus(void);

#ifdef __cplusplus
}
#endif
#endif
