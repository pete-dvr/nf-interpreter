//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <targetHAL.h>
#include <nanoCLR_Application.h>
#include <target_os.h>
#include <WireProtocol_ReceiverThread.h>
#include <LaunchCLR.h>
#include <string.h>
#if CONFIG_NF_FEATURE_PRODUCT_OTA
#include <ProductFirmwareUpdate.h>
#include <ProductDeviceTelemetry.h>
#include <targetHAL_ConfigStorage.h>
#endif

extern void CLRStartupThread(void const *argument);
TaskHandle_t ReceiverTask;

void receiver_task(void *pvParameter)
{
    (void)pvParameter;

    ReceiverThread(0);

    vTaskDelete(NULL);
}

// Main task start point
void main_task(void *pvParameter)
{
    (void)pvParameter;

    // CLR settings to launch CLR thread
    CLR_SETTINGS clrSettings;
    (void)memset(&clrSettings, 0, sizeof(CLR_SETTINGS));

    clrSettings.MaxContextSwitches = 50;
    clrSettings.WaitForDebugger = false;
    clrSettings.EnterDebuggerLoopAfterExit = true;

    CLRStartupThread(&clrSettings);

    vTaskDelete(NULL);
}

// Dummy defauly log method to stop output from ESP32 IDF
int dummyLog(const char *format, va_list arg)
{
    (void)format;
    (void)arg;
    return 1;
}

// App_main
// Called from Esp32 IDF start up code before scheduler starts
void app_main()
{
#if CONFIG_NF_FEATURE_PRODUCT_OTA
    // The native deadline starts before configuration, networking or CLR startup.
    esp_err_t otaResult = ProductOta_Initialize();
    if (otaResult != ESP_OK)
    {
        ProductOta_FailBoot(otaResult);
    }
    // nanoHAL_Initialize validates the pair before every CLR deployment scan,
    // including the initial boot and subsequent CLR-only reboots.
    otaResult = nvs_flash_init();
    if (otaResult != ESP_OK)
    {
        // Never erase field NVS to recover initialization errors.
        ProductOta_FailBoot(otaResult);
    }
    // Capture the hardware reset cause before managed code or network startup.
    // Telemetry failures must not prevent boot; managed calls retry storage.
    esp_err_t powerStatusResult = ProductTelemetry_InitializePowerStatus();
    if (powerStatusResult != ESP_OK)
    {
        ESP_LOGE("product_power", "Cannot persist reset cause: %s", esp_err_to_name(powerStatusResult));
    }
    ConfigStorage_Initialise();
#endif
    // Switch off logging so as not to interfere with WireProtocol over Uart0
    esp_log_level_set("*", ESP_LOG_NONE);

    // Stop any logging being directed to VS connection, was an issue with Nimble, outputting on Uart0
    // TODO : redirect these to debugger controlled from nanoframework.Hardware.Esp32
    esp_log_set_vprintf(dummyLog);

#if !CONFIG_NF_FEATURE_PRODUCT_OTA
    ESP_ERROR_CHECK(nvs_flash_init());
#endif

    // start receiver task
    xTaskCreatePinnedToCore(&receiver_task, "ReceiverThread", 3072, NULL, 5, &ReceiverTask, 0);

    // start the CLR main task
    xTaskCreatePinnedToCore(&main_task, "main_task", 15000, NULL, 5, NULL, 0);
}
