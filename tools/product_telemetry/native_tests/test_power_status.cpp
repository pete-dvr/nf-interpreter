// Runs the production latch against IDF-style write-through NVS, plus a
// deferred-commit model to exercise compatibility with a future cached backend.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include "../../../targets/ESP32/_common/ProductPowerStatus.cpp"

namespace
{
int stored = -1, staged = -1, writes = 0, commits = 0, handles = 0, passed = 0;
esp_reset_reason_t reason = ESP_RST_POWERON;
esp_err_t openError = ESP_OK, readError = ESP_OK, writeError = ESP_OK, commitError = ESP_OK;
bool lockAvailable = true;
bool writeThrough = true;
void Check(bool value) { if (!value) throw std::runtime_error("assertion failed"); }
void Boot(esp_reset_reason_t reset)
{
    s_powerInitialized = false;
    s_dropoutPending = false;
    reason = reset;
    staged = stored;
}
int Status()
{
    int32_t value = 99;
    Check(ProductTelemetry_GetPowerStatus(&value) == ESP_OK);
    return value;
}
void Run(const char *name, const std::function<void()> &test)
{
    stored = staged = -1;
    writes = commits = handles = 0;
    openError = readError = writeError = commitError = ESP_OK;
    lockAvailable = true;
    writeThrough = true;
    Boot(ESP_RST_POWERON);
    try { test(); Check(handles == 0); ++passed; }
    catch (...) { std::fprintf(stderr, "FAIL: %s\n", name); throw; }
}
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{
    return lockAvailable && mutex->mutex.try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->mutex.unlock(); return pdTRUE; }
esp_reset_reason_t esp_reset_reason() { return reason; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    Check(std::strcmp(name, "product_power") == 0 && mode == NVS_READWRITE);
    if (openError != ESP_OK) return openError;
    ++handles;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t, const char *key, uint8_t *value)
{
    Check(std::strcmp(key, "dropout") == 0);
    if (readError != ESP_OK) return readError;
    if (staged < 0) return ESP_ERR_NVS_NOT_FOUND;
    *value = static_cast<uint8_t>(staged);
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t, const char *key, uint8_t value)
{
    Check(std::strcmp(key, "dropout") == 0);
    ++writes;
    if (writeError != ESP_OK && writeError != ESP_ERR_NVS_REMOVE_FAILED) return writeError;
    staged = value;
    // IDF 5.5.5 writes the replacement before erasing the previous entry.
    // REMOVE_FAILED may therefore leave the new value durable despite error.
    if (writeThrough) stored = value;
    return writeError;
}
esp_err_t nvs_commit(nvs_handle_t)
{
    ++commits;
    if (commitError != ESP_OK) return commitError;
    // The installed IDF implementation is write-through and commit is a no-op.
    // Deferred mode represents a possible future API-compatible cached backend.
    if (!writeThrough) stored = staged;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) { --handles; }

int main()
{
    Run("normal boot does not write dropout key", [] { Check(Status() == 1 && writes == 0 && commits == 0); });
    Run("software restart is not brownout", [] { Boot(ESP_RST_SW); Check(Status() == 1); });
    Run("early boot persists brownout", [] {
        Boot(ESP_RST_BROWNOUT);
        Check(ProductTelemetry_InitializePowerStatus() == ESP_OK && stored == 1);
        Check(Status() == 2);
    });
    Run("pending survives power loss", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        Boot(ESP_RST_POWERON); Check(Status() == 2 && commits == 1);
    });
    Run("pending survives software reset", [] {
        stored = 1; Boot(ESP_RST_SW); Check(Status() == 2);
    });
    Run("reads do not consume", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2 && Status() == 2 && commits == 1);
    });
    Run("ack commits clear", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK && stored == 0 && Status() == 1);
    });
    Run("managed restart never relatches", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK);
        Check(ProductTelemetry_InitializePowerStatus() == ESP_OK && Status() == 1 && commits == 2);
    });
    Run("acked state survives hardware boot", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK);
        Boot(ESP_RST_POWERON); Check(Status() == 1);
    });
    Run("new brownout relatches", [] {
        stored = 0; Boot(ESP_RST_BROWNOUT); Check(Status() == 2 && stored == 1);
    });
    Run("ack OK does not wear flash", [] {
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK && writes == 0);
    });
    Run("open failure never reports false OK", [] {
        openError = ESP_FAIL; int32_t value = 99;
        Check(ProductTelemetry_GetPowerStatus(&value) == ESP_FAIL && value == 99);
        openError = ESP_OK; Check(Status() == 1);
    });
    Run("read failure preserves output", [] {
        readError = ESP_FAIL; int32_t value = 99;
        Check(ProductTelemetry_GetPowerStatus(&value) == ESP_FAIL && value == 99);
    });
    Run("failed boot set retries", [] {
        Boot(ESP_RST_BROWNOUT); writeError = ESP_FAIL;
        Check(ProductTelemetry_InitializePowerStatus() == ESP_FAIL && !s_powerInitialized);
        writeError = ESP_OK; Check(Status() == 2 && stored == 1);
    });
    Run("deferred model: failed boot commit retries staged value", [] {
        writeThrough = false;
        Boot(ESP_RST_BROWNOUT); commitError = ESP_FAIL;
        Check(ProductTelemetry_InitializePowerStatus() == ESP_FAIL && staged == 1 && stored == -1);
        commitError = ESP_OK; Check(Status() == 2 && stored == 1 && commits == 2);
    });
    Run("failed clear set retains event", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2); writeError = ESP_FAIL;
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_FAIL && Status() == 2 && stored == 1);
        writeError = ESP_OK; Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK && Status() == 1);
    });
    Run("write-through clear plus injected commit error retains RAM until retry", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2); commitError = ESP_FAIL;
        // The caller has already transmitted before acknowledging. An error
        // after a successful write cannot guarantee that flash was unchanged.
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_FAIL && Status() == 2 && stored == 0);
        commitError = ESP_OK; Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK && Status() == 1);
    });
    Run("deferred model: power loss after failed clear commit retains event", [] {
        writeThrough = false;
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2); commitError = ESP_FAIL;
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_FAIL);
        commitError = ESP_OK; Boot(ESP_RST_POWERON); Check(Status() == 2);
    });
    Run("write-through boot write survives injected later commit failure", [] {
        Boot(ESP_RST_BROWNOUT); commitError = ESP_FAIL;
        int32_t value = 99;
        Check(ProductTelemetry_GetPowerStatus(&value) == ESP_FAIL && value == 99);
        Check(stored == 1 && !s_powerInitialized);
        commitError = ESP_OK; Boot(ESP_RST_POWERON); Check(Status() == 2);
    });
    Run("write-through failed cleanup can persist brownout before returning error", [] {
        Boot(ESP_RST_BROWNOUT); writeError = ESP_ERR_NVS_REMOVE_FAILED;
        int32_t value = 99;
        Check(ProductTelemetry_GetPowerStatus(&value) == ESP_ERR_NVS_REMOVE_FAILED && value == 99);
        Check(stored == 1 && !s_powerInitialized && commits == 0);
        writeError = ESP_OK; Boot(ESP_RST_POWERON); Check(Status() == 2);
    });
    Run("write-through failed cleanup retries brownout persistence in same boot", [] {
        Boot(ESP_RST_BROWNOUT); writeError = ESP_ERR_NVS_REMOVE_FAILED;
        Check(ProductTelemetry_InitializePowerStatus() == ESP_ERR_NVS_REMOVE_FAILED && stored == 1);
        writeError = ESP_OK; Check(Status() == 2 && s_powerInitialized && commits == 1);
    });
    Run("reads never clear unacknowledged write-through event", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        const int writesBeforeReads = writes;
        for (int i = 0; i < 50; ++i) Check(Status() == 2);
        Check(stored == 1 && writes == writesBeforeReads);
        Boot(ESP_RST_POWERON); Check(Status() == 2 && stored == 1);
    });
    Run("write-through failed ack cleanup keeps conservative RAM until retry", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        writeError = ESP_ERR_NVS_REMOVE_FAILED;
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_ERR_NVS_REMOVE_FAILED);
        Check(stored == 0 && Status() == 2 && s_dropoutPending);
        writeError = ESP_OK;
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_OK && Status() == 1);
    });
    Run("write-through clear may survive reboot even when ack returns error", [] {
        Boot(ESP_RST_BROWNOUT); Check(Status() == 2);
        // Calling acknowledge means the report was already transmitted. The
        // API cannot undo an NVS replacement that completed before an error.
        writeError = ESP_ERR_NVS_REMOVE_FAILED;
        Check(ProductTelemetry_AcknowledgePowerStatus() == ESP_ERR_NVS_REMOVE_FAILED && Status() == 2);
        writeError = ESP_OK; Boot(ESP_RST_POWERON); Check(Status() == 1);
    });
    Run("corrupt value does not become false OK", [] {
        stored = 9; Boot(ESP_RST_POWERON);
        Check(ProductTelemetry_InitializePowerStatus() == ESP_ERR_INVALID_STATE);
    });
    Run("lock timeout preserves output", [] {
        lockAvailable = false; int32_t value = 99;
        Check(ProductTelemetry_GetPowerStatus(&value) == ESP_ERR_TIMEOUT && value == 99);
    });
    Run("null output rejected", [] { Check(ProductTelemetry_GetPowerStatus(nullptr) == ESP_ERR_INVALID_ARG); });
    std::printf("PASS: %d power-status persistence checks\n", passed);
}
