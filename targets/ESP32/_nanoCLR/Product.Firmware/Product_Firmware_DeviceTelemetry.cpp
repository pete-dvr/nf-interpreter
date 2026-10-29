//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include "Product_Firmware.h"
#include <ProductDeviceTelemetry.h>
#include <corlib_native.h>
#include <stdio.h>

static HRESULT TelemetryException(CLR_RT_StackFrame &stack, esp_err_t error, const char *operation)
{
    if (error == ESP_OK)
    {
        return S_OK;
    }

    // ESP-IDF error codes can be positive and must not escape as CLR success.
    const HRESULT exceptionCode = error == ESP_ERR_NO_MEM    ? CLR_E_OUT_OF_MEMORY
                                  : error == ESP_ERR_TIMEOUT ? CLR_E_TIMEOUT
                                                             : CLR_E_INVALID_OPERATION;
    CLR_RT_HeapBlock &exception = stack.m_owningThread->m_currentException;
    const HRESULT result = Library_corlib_native_System_Exception::CreateInstance(exception, exceptionCode, &stack);
    if (FAILED(result))
    {
        return result;
    }
    if (result == S_OK)
    {
        char message[112];
        snprintf(message, sizeof(message), "ESP32 %s failed: %s (0x%04x)",
                 operation, esp_err_to_name(error), (unsigned int)error);
        // Keep the exception rooted by its managed thread while allocating text.
        (void)CLR_RT_HeapBlock_String::CreateInstance(
            exception.Dereference()[Library_corlib_native_System_Exception::FIELD___message], message);
    }
    return CLR_E_PROCESS_EXCEPTION;
}

HRESULT Library_Product_Firmware_Product_Firmware_DeviceTelemetry::GetInternalTemperatureCelsius___STATIC__R4(
    CLR_RT_StackFrame &stack)
{
    float celsius = 0;
    const esp_err_t error = ProductTelemetry_GetInternalTemperatureCelsius(&celsius);
    if (error == ESP_OK)
    {
        stack.SetResult_R4(celsius);
    }
    return TelemetryException(stack, error, "internal temperature read");
}

HRESULT Library_Product_Firmware_Product_Firmware_DeviceTelemetry::GetPowerStatus___STATIC__I4(CLR_RT_StackFrame &stack)
{
    int32_t status = 0;
    const esp_err_t error = ProductTelemetry_GetPowerStatus(&status);
    if (error == ESP_OK)
    {
        stack.SetResult_I4(status);
    }
    return TelemetryException(stack, error, "power status read");
}

HRESULT Library_Product_Firmware_Product_Firmware_DeviceTelemetry::AcknowledgePowerStatus___STATIC__VOID(
    CLR_RT_StackFrame &stack)
{
    return TelemetryException(stack, ProductTelemetry_AcknowledgePowerStatus(), "power status acknowledgment");
}
