//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include "Product_Firmware.h"
#include <ProductFirmwareUpdate.h>
#include <ProductFirmwareErrorMap.h>
#include <corlib_native.h>
#include <stdio.h>

static HRESULT OtaException(CLR_RT_StackFrame &stack, esp_err_t error)
{
    if (error == ESP_OK)
    {
        return S_OK;
    }
    char message[192];
    char detail[128] = {0};
    ProductOta_GetLastError(detail, sizeof(detail));
    snprintf(message, sizeof(message), "Product OTA error 0x%04x: %s", (unsigned int)error, detail);
    CLR_RT_HeapBlock &exception = stack.m_owningThread->m_currentException;
    HRESULT result = Library_corlib_native_System_Exception::CreateInstance(exception, MapOtaError(error), &stack);
    if (FAILED(result))
    {
        return result;
    }
    if (result == S_OK)
    {
        // The thread roots the exception while allocating its message. S_FALSE
        // means CreateInstance supplied the preallocated out-of-memory exception.
        (void)CLR_RT_HeapBlock_String::CreateInstance(
            exception.Dereference()[Library_corlib_native_System_Exception::FIELD___message], message);
    }
    return CLR_E_PROCESS_EXCEPTION;
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::InstallFromFile___STATIC__VOID__STRING__U4(
    CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();
    CLR_RT_HeapBlock interval;
    CLR_INT64 *deadline = NULL;
    uint32_t token = 0;
    bool done = false;
    bool event = false;
    esp_err_t result = ESP_OK;

    // Thread.Abort can unwind a waiting native frame without normal re-entry.
    // Its pop callback must release ownership and cancel any uncommitted work.
    if (stack.m_flags & CLR_RT_StackFrame::c_CalledOnPop)
    {
        NANOCLR_SET_AND_LEAVE(S_OK);
    }

    interval.SetInteger((CLR_INT64)50 * TIME_CONVERSION__TO_MILLISECONDS);
    NANOCLR_CHECK_HRESULT(stack.SetupTimeoutFromTicks(interval, deadline));
    if (stack.m_customState == 1)
    {
        const char *path = stack.Arg0().RecoverString();
        FAULT_ON_NULL_ARG(path);
        // StartInstall copies path and arguments before any managed yield.
        result = ProductOta_StartInstall(path, stack.Arg1().NumericByRef().u4, &token);
        if (result != ESP_OK)
        {
            NANOCLR_SET_AND_LEAVE(OtaException(stack, result));
        }
        stack.PushValueI4((CLR_INT32)token);
        stack.m_customState = 2;
        stack.m_flags |= CLR_RT_StackFrame::c_CallOnPop;
    }
    token = stack.m_evalStack[1].NumericByRef().u4;
    while (true)
    {
        result = ProductOta_PollInstall(token, &done);
        if (done || result != ESP_OK)
        {
            // Success commits and restarts in the worker, so it cannot return.
            NANOCLR_SET_AND_LEAVE(OtaException(
                stack, result == ESP_OK ? (esp_err_t)PRODUCT_OTA_ERR_STATE : result));
        }
        NANOCLR_CHECK_HRESULT(g_CLR_RT_ExecutionEngine.WaitEvents(stack.m_owningThread, *deadline, 0, event));
        NANOCLR_CHECK_HRESULT(g_CLR_RT_ExecutionEngine.InitTimeout(*deadline, interval.NumericByRef().s8));
    }

    NANOCLR_CLEANUP();
    if (hr != CLR_E_THREAD_WAITING)
    {
        stack.m_flags &= ~(CLR_RT_StackFrame::c_CallOnPop | CLR_RT_StackFrame::c_CalledOnPop);
        if (stack.m_customState == 2)
        {
            ProductOta_ReleaseInstall(stack.m_evalStack[1].NumericByRef().u4);
            stack.PopValue();
        }
        if (stack.m_customState != 0)
        {
            stack.PopValue();
        }
        stack.m_customState = 0;
    }
    NANOCLR_CLEANUP_END();
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::IsTrialBoot___STATIC__BOOLEAN(
    CLR_RT_StackFrame &stack)
{
    bool trial = false;
    esp_err_t result = ProductOta_IsTrialBoot(&trial);
    if (result != ESP_OK)
    {
        return OtaException(stack, result);
    }
    stack.SetResult_Boolean(trial);
    return S_OK;
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::ConfirmBoot___STATIC__VOID(CLR_RT_StackFrame &stack)
{
    return OtaException(stack, ProductOta_ConfirmBoot());
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::RejectBootAndRestart___STATIC__VOID(
    CLR_RT_StackFrame &stack)
{
    return OtaException(stack, ProductOta_RejectBootAndRestart());
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::GetStagingFreeBytes___STATIC__I8(
    CLR_RT_StackFrame &stack)
{
    uint64_t freeBytes = 0;
    esp_err_t result = ProductOta_GetStagingFreeBytes(&freeBytes);
    if (result != ESP_OK)
    {
        return OtaException(stack, result);
    }
    stack.SetResult_I8((CLR_INT64)freeBytes);
    return S_OK;
}

HRESULT Library_Product_Firmware_Product_Firmware_FirmwareUpdate::GetBootStatus___STATIC__STRING(
    CLR_RT_StackFrame &stack)
{
    char status[160];
    esp_err_t result = ProductOta_GetBootStatus(status, sizeof(status));
    if (result != ESP_OK)
    {
        return OtaException(stack, result);
    }
    return stack.SetResult_String(status);
}
