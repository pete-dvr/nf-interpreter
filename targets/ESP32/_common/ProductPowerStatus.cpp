// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.

#include <ProductDeviceTelemetry.h>
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
StaticSemaphore_t s_powerMutexStorage;
SemaphoreHandle_t s_powerMutex = nullptr;
portMUX_TYPE s_powerInitializationMux = portMUX_INITIALIZER_UNLOCKED;
bool s_powerInitialized = false;
bool s_dropoutPending = false;

class PowerLock
{
  public:
    PowerLock()
    {
        portENTER_CRITICAL(&s_powerInitializationMux);
        if (s_powerMutex == nullptr)
        {
            s_powerMutex = xSemaphoreCreateMutexStatic(&s_powerMutexStorage);
        }
        m_mutex = s_powerMutex;
        portEXIT_CRITICAL(&s_powerInitializationMux);
        m_acquired = m_mutex != nullptr && xSemaphoreTake(m_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
    }
    ~PowerLock()
    {
        if (m_acquired)
        {
            xSemaphoreGive(m_mutex);
        }
    }
    bool IsAcquired() const { return m_acquired; }

  private:
    SemaphoreHandle_t m_mutex;
    bool m_acquired;
};

// Called under the mutex. Never erase the shared NVS partition on an error.
esp_err_t InitializePowerStatus()
{
    if (s_powerInitialized)
    {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open("product_power", NVS_READWRITE, &handle);
    if (result != ESP_OK)
    {
        return result;
    }
    uint8_t pending = 0;
    result = nvs_get_u8(handle, "dropout", &pending);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = ESP_OK;
    }
    if (result == ESP_OK && pending > 1)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK && esp_reset_reason() == ESP_RST_BROWNOUT)
    {
        pending = 1;
        result = nvs_set_u8(handle, "dropout", pending);
        if (result == ESP_OK)
        {
            result = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    if (result == ESP_OK)
    {
        s_dropoutPending = pending != 0;
        // Native state lasts until the next hardware boot. In particular, a
        // managed deployment must not relatch the old hardware reset reason.
        s_powerInitialized = true;
    }
    return result;
}
} // namespace

esp_err_t ProductTelemetry_InitializePowerStatus(void)
{
    PowerLock lock;
    return lock.IsAcquired() ? InitializePowerStatus() : ESP_ERR_TIMEOUT;
}

esp_err_t ProductTelemetry_GetPowerStatus(int32_t *status)
{
    if (status == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    PowerLock lock;
    if (!lock.IsAcquired())
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = InitializePowerStatus();
    if (result == ESP_OK)
    {
        *status = s_dropoutPending ? 2 : 1;
    }
    return result;
}

esp_err_t ProductTelemetry_AcknowledgePowerStatus(void)
{
    PowerLock lock;
    if (!lock.IsAcquired())
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = InitializePowerStatus();
    if (result != ESP_OK || !s_dropoutPending)
    {
        return result;
    }
    nvs_handle_t handle;
    result = nvs_open("product_power", NVS_READWRITE, &handle);
    if (result != ESP_OK)
    {
        return result;
    }
    result = nvs_set_u8(handle, "dropout", 0);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result == ESP_OK)
    {
        s_dropoutPending = false;
    }
    return result;
}
