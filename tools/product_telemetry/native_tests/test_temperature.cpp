// Host tests execute the actual firmware helper with mocked ESP-IDF boundaries.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../../../targets/ESP32/_common/ProductDeviceTelemetry.cpp"

namespace
{
constexpr esp_err_t kDriverError = 0x771;
int g_installCalls;
int g_enableCalls;
int g_uninstallCalls;
int g_readCalls;
int g_mutexCreateCalls;
int g_liveHandles;
int g_rangeMin;
int g_rangeMax;
int g_readDelayMs;
bool g_failMutexCreation;
esp_err_t g_installResult;
esp_err_t g_enableResult;
esp_err_t g_uninstallResult;
esp_err_t g_readResult;
float g_value;
std::atomic<int> g_activeReads{0};
std::atomic<int> g_maxActiveReads{0};
int g_passed;

void Require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Reset()
{
    // Reset fake hardware between independent hardware-boot scenarios.
    delete s_sensor;
    s_sensor = nullptr;
    s_sensorEnabled = false;
    s_sensorMutex = nullptr;
    g_installCalls = g_enableCalls = g_uninstallCalls = g_readCalls = g_mutexCreateCalls = g_liveHandles = 0;
    g_rangeMin = g_rangeMax = g_readDelayMs = 0;
    g_failMutexCreation = false;
    g_installResult = g_enableResult = g_uninstallResult = g_readResult = ESP_OK;
    g_value = 42.5f;
    g_activeReads = g_maxActiveReads = 0;
}

void Run(const char *name, const std::function<void()> &test)
{
    Reset();
    try
    {
        test();
        ++g_passed;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
        throw;
    }
}

void ReadSucceeds(float expected)
{
    g_value = expected;
    float output = -999.0f;
    Require(ProductTelemetry_GetInternalTemperatureCelsius(&output) == ESP_OK, "read should succeed");
    Require(output == expected, "must return exact sensor value");
}

void ReadFails(esp_err_t expected)
{
    float output = -999.0f;
    Require(ProductTelemetry_GetInternalTemperatureCelsius(&output) == expected, "wrong error propagation");
    Require(output == -999.0f, "failed read modified caller output");
}
} // namespace

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    ++g_mutexCreateCalls;
    return g_failMutexCreation ? nullptr : storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    return semaphore->mutex.try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->mutex.unlock();
    return pdTRUE;
}

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *config, temperature_sensor_handle_t *handle)
{
    ++g_installCalls;
    Require(g_liveHandles == 0, "installed second sensor while first is owned");
    g_rangeMin = config->range_min;
    g_rangeMax = config->range_max;
    if (g_installResult != ESP_OK)
    {
        return g_installResult;
    }
    *handle = new temperature_sensor_obj_t;
    ++g_liveHandles;
    return ESP_OK;
}

esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t handle)
{
    ++g_uninstallCalls;
    Require(handle != nullptr && !handle->enabled, "uninstall requires disabled owned sensor");
    if (g_uninstallResult != ESP_OK)
    {
        return g_uninstallResult;
    }
    delete handle;
    --g_liveHandles;
    return ESP_OK;
}

esp_err_t temperature_sensor_enable(temperature_sensor_handle_t handle)
{
    ++g_enableCalls;
    Require(handle != nullptr && !handle->enabled, "enable requires initialized sensor");
    if (g_enableResult != ESP_OK)
    {
        return g_enableResult;
    }
    handle->enabled = true;
    return ESP_OK;
}

esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t handle, float *celsius)
{
    Require(handle != nullptr && handle->enabled, "read requires enabled sensor");
    ++g_readCalls;
    int active = ++g_activeReads;
    int maximum = g_maxActiveReads.load();
    while (active > maximum && !g_maxActiveReads.compare_exchange_weak(maximum, active))
    {
    }
    if (g_readDelayMs != 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_readDelayMs));
    }
    *celsius = g_value;
    --g_activeReads;
    return g_readResult;
}

int main()
{
    Run("null output does not initialize", [] {
        Require(ProductTelemetry_GetInternalTemperatureCelsius(nullptr) == ESP_ERR_INVALID_ARG, "null must fail");
        Require(g_installCalls == 0 && g_mutexCreateCalls == 0, "null argument allocated resources");
    });
    Run("mutex creation failure and retry", [] {
        g_failMutexCreation = true;
        ReadFails(ESP_ERR_NO_MEM);
        Require(g_installCalls == 0, "installed without mutex");
        g_failMutexCreation = false;
        ReadSucceeds(34.25f);
    });
    Run("install failure and retry", [] {
        g_installResult = ESP_ERR_NO_MEM;
        ReadFails(ESP_ERR_NO_MEM);
        Require(g_enableCalls == 0 && g_uninstallCalls == 0, "used failed install handle");
        g_installResult = ESP_OK;
        ReadSucceeds(41.0f);
        Require(g_installCalls == 2 && g_liveHandles == 1, "install did not recover");
    });
    Run("existing driver ownership failure", [] {
        g_installResult = ESP_ERR_INVALID_STATE;
        ReadFails(ESP_ERR_INVALID_STATE);
        Require(g_enableCalls == 0 && g_uninstallCalls == 0, "touched another owner's sensor");
    });
    Run("enable failure cleanup and retry", [] {
        g_enableResult = kDriverError;
        ReadFails(kDriverError);
        Require(g_uninstallCalls == 1 && g_liveHandles == 0 && s_sensor == nullptr, "enable failure leaked sensor");
        g_enableResult = ESP_OK;
        ReadSucceeds(40.0f);
        Require(g_installCalls == 2 && g_enableCalls == 2, "failed initialization did not recover");
    });
    Run("cleanup failure preserves ownership", [] {
        g_enableResult = kDriverError;
        g_uninstallResult = ESP_ERR_INVALID_STATE;
        ReadFails(kDriverError);
        Require(s_sensor != nullptr && g_liveHandles == 1, "cleanup failure lost owned handle");
        g_enableResult = g_uninstallResult = ESP_OK;
        ReadSucceeds(42.0f);
        Require(g_installCalls == 1 && g_enableCalls == 2, "recovery reinstalled owned sensor");
    });
    Run("read failure preserves output and recovers", [] {
        g_readResult = kDriverError;
        ReadFails(kDriverError);
        g_readResult = ESP_OK;
        ReadSucceeds(83.5f);
        Require(g_installCalls == 1 && g_enableCalls == 1, "transient read failure reallocated sensor");
    });
    Run("repeated readings reuse boot lifetime resources", [] {
        for (int i = 0; i != 1000; ++i)
        {
            ReadSucceeds(40.5f + static_cast<float>(i % 70));
        }
        Require(g_installCalls == 1 && g_enableCalls == 1 && g_mutexCreateCalls == 1, "repeated calls reallocated resources");
        Require(g_liveHandles == 1 && g_readCalls == 1000, "sensor ownership/read count mismatch");
        Require(g_rangeMin == -10 && g_rangeMax == 80, "initial range must use best-accuracy supported range");
    });
    for (float value : {-40.0f, -30.0f, -10.0f, -0.5f, 0.0f, 20.0f, 50.0f, 80.0f, 85.0f, 100.0f, 110.0f, 125.0f})
    {
        Run("full automatically ranged operating interval", [value] { ReadSucceeds(value); });
    }
    for (float value : {-40.01f, 125.01f, INFINITY, -INFINITY, NAN})
    {
        Run("invalid SDK success value fails closed", [value] {
            g_value = value;
            ReadFails(ESP_FAIL);
            ReadSucceeds(37.0f);
        });
    }
    Run("concurrent first reads serialize initialization and SDK calls", [] {
        g_readDelayMs = 1;
        std::atomic<int> succeeded{0};
        std::vector<std::thread> readers;
        for (int i = 0; i != 16; ++i)
        {
            readers.emplace_back([&succeeded] {
                float value = -999.0f;
                if (ProductTelemetry_GetInternalTemperatureCelsius(&value) == ESP_OK && value == 42.5f)
                {
                    ++succeeded;
                }
            });
        }
        for (std::thread &reader : readers)
        {
            reader.join();
        }
        Require(succeeded == 16, "concurrent readers failed");
        Require(g_installCalls == 1 && g_enableCalls == 1 && g_mutexCreateCalls == 1, "concurrent initialization raced");
        Require(g_maxActiveReads == 1, "SDK reads overlapped");
    });
    Run("busy sensor returns bounded timeout and then recovers", [] {
        ReadSucceeds(35.0f);
        Require(xSemaphoreTake(s_sensorMutex, 0) == pdTRUE, "test cannot hold sensor mutex");
        esp_err_t result = ESP_OK;
        float output = -999.0f;
        std::thread reader([&] { result = ProductTelemetry_GetInternalTemperatureCelsius(&output); });
        reader.join();
        xSemaphoreGive(s_sensorMutex);
        Require(result == ESP_ERR_TIMEOUT && output == -999.0f, "busy read did not preserve output and fail");
        Require(g_readCalls == 1, "busy read entered SDK");
        ReadSucceeds(36.0f);
    });
    Reset();
    std::printf("PASS: %d native temperature sensor scenarios\n", g_passed);
}
