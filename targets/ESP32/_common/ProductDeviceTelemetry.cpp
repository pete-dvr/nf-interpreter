//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <ProductDeviceTelemetry.h>
#include <driver/temperature_sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
StaticSemaphore_t s_sensorMutexStorage;
SemaphoreHandle_t s_sensorMutex = nullptr;
portMUX_TYPE s_initializationMux = portMUX_INITIALIZER_UNLOCKED;
temperature_sensor_handle_t s_sensor = nullptr;
bool s_sensorEnabled = false;

SemaphoreHandle_t GetSensorMutex()
{
    portENTER_CRITICAL(&s_initializationMux);
    if (s_sensorMutex == nullptr)
    {
        s_sensorMutex = xSemaphoreCreateMutexStatic(&s_sensorMutexStorage);
    }
    SemaphoreHandle_t mutex = s_sensorMutex;
    portEXIT_CRITICAL(&s_initializationMux);
    return mutex;
}

class SensorLock
{
  public:
    explicit SensorLock(SemaphoreHandle_t mutex) : m_mutex(mutex)
    {
        m_acquired = mutex != nullptr && xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE;
    }

    ~SensorLock()
    {
        if (m_acquired)
        {
            xSemaphoreGive(m_mutex);
        }
    }

    bool IsAcquired() const
    {
        return m_acquired;
    }

  private:
    SemaphoreHandle_t m_mutex;
    bool m_acquired;
};

esp_err_t EnsureSensorEnabled()
{
    if (s_sensor == nullptr)
    {
        // Select the most accurate initial range. IDF 5.5's
        // temp_sensor_get_raw_value() changes ranges automatically, including
        // the high-temperature range up to 125 C. Asking install() for the full
        // -40..125 span fails because no one analog range covers that span.
        const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t result = temperature_sensor_install(&config, &s_sensor);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (!s_sensorEnabled)
    {
        esp_err_t result = temperature_sensor_enable(s_sensor);
        if (result != ESP_OK)
        {
            // Keep ownership if cleanup fails, so the next call can recover
            // without losing the handle or installing a second sensor.
            if (temperature_sensor_uninstall(s_sensor) == ESP_OK)
            {
                s_sensor = nullptr;
            }
            return result;
        }
        s_sensorEnabled = true;
    }

    return ESP_OK;
}
} // namespace

esp_err_t ProductTelemetry_GetInternalTemperatureCelsius(float *celsius)
{
    if (celsius == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = GetSensorMutex();
    if (mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    SensorLock lock(mutex);
    if (!lock.IsAcquired())
    {
        return ESP_ERR_TIMEOUT;
    }

    // This native state lives for one hardware boot, independent of managed
    // deployments/soft restarts. Repeated readings reuse the same driver handle
    // and static mutex instead of allocating and leaking a handle each time.
    esp_err_t result = EnsureSensorEnabled();
    if (result != ESP_OK)
    {
        return result;
    }

    float measured;
    result = temperature_sensor_get_celsius(s_sensor, &measured);
    if (result != ESP_OK)
    {
        return result;
    }

    // Failed eFuse calibration can yield NaN even when the SDK reports ESP_OK.
    // Ordered comparisons reject NaN, infinities and unsupported temperatures.
    if (!(measured >= -40.0f && measured <= 125.0f))
    {
        return ESP_FAIL;
    }

    *celsius = measured;
    return ESP_OK;
}
