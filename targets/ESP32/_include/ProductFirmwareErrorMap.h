//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef PRODUCT_FIRMWARE_ERROR_MAP_H
#define PRODUCT_FIRMWARE_ERROR_MAP_H

#include <nf_errors_exceptions.h>
#include <ProductFirmwareUpdate.h>

// ESP-IDF errors are often positive. Every nonzero SDK/product result must be
// translated before it reaches the CLR's signed HRESULT convention.
inline HRESULT MapOtaError(esp_err_t error)
{
    switch (error)
    {
        case ESP_OK:
            return S_OK;
        case ESP_ERR_NO_MEM:
        case PRODUCT_OTA_ERR_MEMORY:
            return CLR_E_OUT_OF_MEMORY;
        case ESP_ERR_INVALID_ARG:
        case PRODUCT_OTA_ERR_PATH:
        case PRODUCT_OTA_ERR_HEADER:
            return CLR_E_INVALID_PARAMETER;
        case ESP_ERR_TIMEOUT:
        case PRODUCT_OTA_ERR_TIMEOUT:
            return CLR_E_TIMEOUT;
        case PRODUCT_OTA_ERR_IO:
        case PRODUCT_OTA_ERR_FLASH:
            return CLR_E_FILE_IO;
        default:
            return CLR_E_INVALID_OPERATION;
    }
}

#endif // PRODUCT_FIRMWARE_ERROR_MAP_H
