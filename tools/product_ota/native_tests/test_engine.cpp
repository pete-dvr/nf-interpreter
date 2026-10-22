#include "fake_sdk.h"
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

static int TestOpen(const char *, int);
static int TestClose(int);
static ssize_t TestRead(int, void *, size_t);
static off_t TestSeek(int, off_t, int);
static int TestStat(int, struct stat *);
#define open TestOpen
#define close TestClose
#define read TestRead
#define lseek TestSeek
#define fstat TestStat
// Execute the implementation, including its private worker and commit path.
#include "../../../targets/ESP32/_common/ProductFirmwareUpdate.cpp"
#undef fstat
#undef lseek
#undef read
#undef close
#undef open
#include "ProductFirmwareErrorMap.h"

const ProductReleaseDescriptor g_ProductReleaseDescriptor = {
    {'P', 'R', '-', 'S', '3', '-', 'N', '1', '6', 'R', '8', '-', '0', '1', 0, 0},
    2,
    {'r', 'u', 'n', 'n', 'i', 'n', 'g', 0},
    16,
    {0x54,0xa3,0xed,0x42,0x81,0xc0,0x7a,0x69,0x10,0xf2,0x89,0x3a,0xf7,0x40,0x38,0xf9,
     0x07,0xd2,0xfb,0xe0,0x3a,0xc5,0xa0,0x6f,0xfa,0x19,0x97,0x77,0xe5,0x1d,0xc2,0x36}
};
#if CONFIG_NF_PRODUCT_OTA_REQUIRE_SIGNATURE
// Intentionally invalid test key: the signature adapters also fail closed.
const uint8_t g_ProductOtaPublicKey[65] = {4};
const size_t g_ProductOtaPublicKeyLength = 65;
#else
const uint8_t g_ProductOtaPublicKey[] = {0};
const size_t g_ProductOtaPublicKeyLength = 0;
#endif

namespace Fake
{
struct Restart {};
struct StopTask {};
esp_partition_t partitions[] = {
    {1,2,0x9000,0x6000,"nvs",false,4096},
    {1,1,0xF000,0x1000,"phy_init",false,4096},
    {0,0x10,0x10000,0x1A0000,"ota_0",false,4096},
    {0,0x11,0x1B0000,0x1A0000,"ota_1",false,4096},
    {1,0x84,0x350000,0x2E0000,"deploy_a",false,4096},
    {1,0x84,0x630000,0x2E0000,"deploy_b",false,4096},
    {1,0,0x910000,0x2000,"otadata",false,4096},
    {1,0x83,0x912000,0x6EE000,"config",false,4096}
};
std::vector<uint8_t> flash;
std::string packagePath;
std::vector<uint8_t> package;
std::vector<std::string> logs;
int bootIndex;
int runningIndex;
esp_ota_img_states_t states[2];
esp_err_t stateResult;
esp_err_t beginResult;
esp_err_t nativeWriteResult;
esp_err_t endResult;
esp_err_t managedEraseResult;
esp_err_t managedWriteResult;
esp_err_t metadataResult;
esp_err_t selectionResult;
esp_err_t confirmResult;
esp_err_t rejectResult;
esp_err_t filesystemResult;
size_t filesystemTotal;
size_t filesystemUsed;
bool readbackCorrupt;
bool managedReadbackCorrupt;
bool managedTailCorrupt;
bool cancelOnClose;
bool stopOnDelay;
bool notifyAvailable;
bool liveHandle;
bool restartInsideLock;
int lockDepth;
int liveAllocations;
int nativeWrites;
int managedWrites;
int erases;
int metadataWrites;
int beginCalls;
int endCalls;
int abortCalls;
int selectionCalls;
int restartCalls;
int confirmCalls;
int rejectCalls;
int closeCalls;
int liveFiles;
int openError;
int closeError;
int seekError;
int statError;
int openInterrupts;
int readInterrupts;
int seekInterrupts;
int statInterrupts;
int readCalls;
int failReadCall;
int eofReadCall;
bool failReadAtEof;
size_t readLimit;
size_t maximumReadRequest;
size_t nativeOffset;
int64_t now;
TaskHandle_t currentTask;
unsigned tasksCreated;
unsigned scenarios;
}

struct FakeIterator { int type; int subtype; int index; };
static bool Matches(const esp_partition_t &p, int type, int subtype, const char *label)
{
    return p.type == type && (subtype == ESP_PARTITION_SUBTYPE_ANY || p.subtype == subtype)
        && (label == NULL || strcmp(p.label, label) == 0);
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    for (const auto &p : Fake::partitions)
        if (Matches(p, type, subtype, label)) return &p;
    return NULL;
}
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *)
{
    FakeIterator *iterator = new FakeIterator{type, subtype, -1};
    return esp_partition_next(iterator);
}
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    while (++iterator->index < 8)
        if (Matches(Fake::partitions[iterator->index], iterator->type, iterator->subtype, NULL)) return iterator;
    delete iterator;
    return NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t count)
{
    assert(offset + count <= p->size);
    memcpy(out, &Fake::flash[p->address + offset], count);
    if (Fake::readbackCorrupt && p == &Fake::partitions[3] && count)
        static_cast<uint8_t *>(out)[0] ^= 1;
    if (Fake::managedReadbackCorrupt && p == &Fake::partitions[5] && offset == 0 && count)
        static_cast<uint8_t *>(out)[0] ^= 1;
    if (Fake::managedTailCorrupt && p == &Fake::partitions[5] && offset >= 257 && count)
        static_cast<uint8_t *>(out)[0] = 0;
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t offset, const void *data, size_t count)
{
    // A regression that writes the running managed slot is immediately fatal.
    assert(p == &Fake::partitions[5]);
    assert(offset + count <= p->size);
    ++Fake::managedWrites;
    if (Fake::managedWriteResult != ESP_OK) return Fake::managedWriteResult;
    memcpy(&Fake::flash[p->address + offset], data, count);
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t count)
{
    assert(p == &Fake::partitions[5]);
    assert(offset % 4096 == 0 && count % 4096 == 0 && offset + count <= p->size);
    ++Fake::erases;
    if (Fake::managedEraseResult != ESP_OK) return Fake::managedEraseResult;
    memset(&Fake::flash[p->address + offset], 0xff, count);
    return ESP_OK;
}
int esp_ota_get_app_partition_count() { return 2; }
const esp_partition_t *esp_ota_get_running_partition() { return &Fake::partitions[Fake::runningIndex]; }
const esp_partition_t *esp_ota_get_boot_partition() { return &Fake::partitions[Fake::bootIndex]; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) { return &Fake::partitions[3]; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state)
{
    *state = Fake::states[p->subtype - 0x10];
    return Fake::stateResult;
}
esp_err_t esp_ota_invalidate_inactive_ota_data_slot()
{
    ++Fake::metadataWrites;
    return Fake::metadataResult;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t count, esp_ota_handle_t *handle)
{
    assert(p == &Fake::partitions[3] && count <= p->size);
    ++Fake::beginCalls;
    assert(!Fake::liveHandle);
    *handle = 42; // Also allocate on begin failure to exercise actual SDK ownership.
    Fake::liveHandle = true;
    Fake::nativeOffset = 0;
    return Fake::beginResult;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t count)
{
    assert(handle == 42 && Fake::liveHandle);
    ++Fake::nativeWrites;
    if (Fake::nativeWriteResult != ESP_OK) return Fake::nativeWriteResult;
    assert(Fake::nativeOffset + count <= Fake::partitions[3].size);
    memcpy(&Fake::flash[Fake::partitions[3].address + Fake::nativeOffset], data, count);
    Fake::nativeOffset += count;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 42 && Fake::liveHandle);
    ++Fake::endCalls;
    Fake::liveHandle = false; // SDK end consumes ownership on success and failure.
    return Fake::endResult;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == 42 && Fake::liveHandle);
    ++Fake::abortCalls;
    Fake::liveHandle = false;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    assert(Fake::lockDepth == 1 && s_activationAttempted);
    assert(p == &Fake::partitions[3]);
    ++Fake::selectionCalls;
    // Simulate an error returned after a persistent write may have completed.
    Fake::bootIndex = 3;
    return Fake::selectionResult;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback()
{
    assert(Fake::lockDepth == 1);
    ++Fake::confirmCalls;
    if (Fake::confirmResult == ESP_OK) Fake::states[0] = ESP_OTA_IMG_VALID;
    return Fake::confirmResult;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot()
{
    assert(Fake::lockDepth == 1);
    ++Fake::rejectCalls;
    if (Fake::rejectResult == ESP_OK) esp_restart();
    return Fake::rejectResult;
}
bool esp_ota_check_rollback_is_possible() { return true; }
uint32_t esp_rom_crc32_le(uint32_t initial, const uint8_t *data, size_t count)
{
    return ProductOtaPackage::UpdateCrc(initial, data, count);
}
[[noreturn]] void esp_restart()
{
    ++Fake::restartCalls;
    Fake::restartInsideLock = Fake::lockDepth != 0;
    throw Fake::Restart();
}
esp_err_t esp_littlefs_info(const char *, size_t *total, size_t *used)
{
    *total = Fake::filesystemTotal; *used = Fake::filesystemUsed;
    return Fake::filesystemResult;
}
int esp_reset_reason() { return 3; }
int64_t esp_timer_get_time() { return Fake::now; }
void *heap_caps_malloc(size_t size, int caps)
{
    assert((caps & MALLOC_CAP_INTERNAL) && (caps & MALLOC_CAP_8BIT));
    void *result = malloc(size);
    if (result) ++Fake::liveAllocations;
    return result;
}
void heap_caps_free(void *p) { if (p) --Fake::liveAllocations; free(p); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
int xSemaphoreTake(SemaphoreHandle_t handle, uint32_t)
{
    assert(handle != NULL && Fake::lockDepth == 0);
    ++Fake::lockDepth; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t)
{
    assert(Fake::lockDepth == 1);
    --Fake::lockDepth; return pdTRUE;
}
void vTaskDelay(uint32_t)
{
    if (Fake::stopOnDelay) throw Fake::StopTask();
}
int xTaskCreateWithCaps(void (*)(void *), const char *, size_t, void *, int, TaskHandle_t *task, int)
{
    *task = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(++Fake::tasksCreated));
    return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return Fake::currentTask; }
uint32_t ulTaskNotifyTake(int, uint32_t)
{
    if (!Fake::notifyAvailable) throw Fake::StopTask();
    Fake::notifyAvailable = false; return 1;
}
void xTaskNotifyGive(TaskHandle_t) { Fake::notifyAvailable = true; }
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *) { return ESP_OK; }
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *) { return ESP_OK; }
esp_err_t esp_task_wdt_add(TaskHandle_t) { return ESP_OK; }
esp_err_t esp_task_wdt_reset() { return ESP_OK; }
void fake_log(const char *, const char *format, ...)
{
    char message[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    Fake::logs.push_back(message);
}

static int TestOpen(const char *path, int flags)
{
    assert(strcmp(path, "/I/updates/update.bin") == 0);
    assert(flags == O_RDONLY);
    if (Fake::openInterrupts > 0)
    {
        --Fake::openInterrupts;
        errno = EINTR;
        return -1;
    }
    if (Fake::openError) { errno = Fake::openError; return -1; }
    int file = open(Fake::packagePath.c_str(), flags);
    if (file >= 0) ++Fake::liveFiles;
    return file;
}
static int TestClose(int file)
{
    assert(Fake::liveFiles == 1);
    ++Fake::closeCalls;
    int result = close(file);
    --Fake::liveFiles;
    // Injection at the final cleanup/activation boundary. The actual installer
    // must re-read cancellation under its commit lock after closing the file.
    if (Fake::cancelOnClose) s_operation.cancel = true;
    if (Fake::closeError) { errno = Fake::closeError; return -1; }
    return result;
}
static ssize_t TestRead(int file, void *buffer, size_t length)
{
    assert(Fake::liveFiles == 1 && length > 0 && length <= 4096);
    ++Fake::readCalls;
    Fake::maximumReadRequest = std::max(Fake::maximumReadRequest, length);
    if (Fake::readInterrupts > 0)
    {
        --Fake::readInterrupts;
        errno = EINTR;
        return -1;
    }
    if (Fake::readCalls == Fake::failReadCall ||
        (Fake::failReadAtEof && lseek(file, 0, SEEK_CUR) == static_cast<off_t>(Fake::package.size())))
    {
        errno = EIO;
        return -1;
    }
    if (Fake::readCalls == Fake::eofReadCall) return 0;
    if (Fake::readLimit > 0) length = std::min(length, Fake::readLimit);
    return read(file, buffer, length);
}
static off_t TestSeek(int file, off_t offset, int origin)
{
    assert(Fake::liveFiles == 1);
    if (Fake::seekInterrupts > 0)
    {
        --Fake::seekInterrupts;
        errno = EINTR;
        return -1;
    }
    if (Fake::seekError) { errno = Fake::seekError; return -1; }
    return lseek(file, offset, origin);
}
static int TestStat(int file, struct stat *info)
{
    assert(Fake::liveFiles == 1);
    if (Fake::statInterrupts > 0)
    {
        --Fake::statInterrupts;
        errno = EINTR;
        return -1;
    }
    if (Fake::statError) { errno = Fake::statError; return -1; }
    return fstat(file, info);
}
static void Put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
static void Hash(const uint8_t *data, size_t length, uint8_t *out)
{
    unsigned produced = 0;
    assert(EVP_Digest(data, length, out, &produced, EVP_sha256(), NULL) == 1 && produced == 32);
}
static void SavePackage()
{
    FILE *file = fopen(Fake::packagePath.c_str(), "wb");
    assert(file);
    assert(fwrite(Fake::package.data(), 1, Fake::package.size(), file) == Fake::package.size());
    assert(fclose(file) == 0);
}
static void MakePackage(size_t nativeLength, size_t managedLength)
{
    Fake::package.assign(128 + nativeLength + managedLength, 0);
    memcpy(Fake::package.data(), "PRDOTA01", 8);
    Fake::package[8] = 1; Fake::package[10] = 128;
    Put32(Fake::package, 16, Fake::package.size());
    Put32(Fake::package, 20, nativeLength);
    Put32(Fake::package, 24, managedLength);
    Put32(Fake::package, 28, 2);
    memcpy(&Fake::package[32], ProductOtaPackage::TargetId, 16);
    memcpy(&Fake::package[48], "candidate", 9);
    memset(&Fake::package[128], 0xe9, nativeLength);
    memset(&Fake::package[128 + nativeLength], 0x6d, managedLength);
    Hash(&Fake::package[128], nativeLength, &Fake::package[64]);
    Hash(&Fake::package[128 + nativeLength], managedLength, &Fake::package[96]);
    SavePackage();
}
static void Reset(bool initialize = true)
{
    assert(Fake::liveAllocations == 0 && !Fake::liveHandle && Fake::lockDepth == 0 && Fake::liveFiles == 0);
    ++Fake::scenarios;
    Fake::bootIndex = Fake::runningIndex = 2;
    Fake::states[0] = Fake::states[1] = ESP_OTA_IMG_VALID;
    Fake::stateResult = Fake::beginResult = Fake::nativeWriteResult = Fake::endResult = ESP_OK;
    Fake::managedEraseResult = Fake::managedWriteResult = Fake::metadataResult = ESP_OK;
    Fake::selectionResult = Fake::confirmResult = Fake::rejectResult = ESP_OK;
    Fake::filesystemResult = ESP_OK;
    Fake::readbackCorrupt = Fake::cancelOnClose = Fake::stopOnDelay = false;
    Fake::managedReadbackCorrupt = Fake::managedTailCorrupt = false;
    Fake::logs.clear();
    Fake::notifyAvailable = Fake::restartInsideLock = false;
    Fake::nativeWrites = Fake::managedWrites = Fake::erases = Fake::metadataWrites = 0;
    Fake::beginCalls = Fake::endCalls = Fake::abortCalls = Fake::selectionCalls = 0;
    Fake::restartCalls = Fake::confirmCalls = Fake::rejectCalls = Fake::closeCalls = 0;
    Fake::openError = Fake::closeError = Fake::seekError = Fake::statError = 0;
    Fake::openInterrupts = Fake::readInterrupts = Fake::seekInterrupts = Fake::statInterrupts = 0;
    Fake::readCalls = Fake::failReadCall = Fake::eofReadCall = 0;
    Fake::failReadAtEof = false;
    Fake::readLimit = Fake::maximumReadRequest = 0;
    Fake::now = 1000000;
    Fake::currentTask = reinterpret_cast<TaskHandle_t>(99);
    Fake::tasksCreated = 0;
    s_lifecycle = NULL; s_worker = s_supervisor = NULL;
    s_initialized = s_recovery = s_activationAttempted = s_pairValidated = false;
    s_trialDeadline = 0; s_nextToken = 0;
    memset(&s_operation, 0, sizeof(s_operation));
    Fake::flash.assign(0x1000000, 0xff);
    memcpy(&Fake::flash[0x350000], "running-managed!", 16);
    for (unsigned sector = 0; sector < 2; ++sector)
    {
        esp_ota_select_entry_t record = {};
        record.ota_seq = sector + 1;
        record.ota_state = ESP_OTA_IMG_VALID;
        record.crc = esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<uint8_t *>(&record.ota_seq), 4);
        memcpy(&Fake::flash[0x910000 + sector * 4096], &record, sizeof(record));
    }
    MakePackage(8192, 257);
    Fake::filesystemTotal = 0x6EE000;
    Fake::filesystemUsed = Fake::package.size() + 4096;
    if (initialize)
    {
        assert(ProductOta_Initialize() == ESP_OK);
        assert(ProductOta_ValidateRunningPair() == ESP_OK);
    }
}
static uint32_t Start(bool wrongCrc = false)
{
    uint32_t token = 0;
    uint32_t crc = ProductOtaPackage::UpdateCrc(UINT32_MAX, Fake::package.data(), Fake::package.size()) ^ UINT32_MAX;
    assert(ProductOta_StartInstall("I:\\updates\\update.bin", crc ^ (wrongCrc ? 1 : 0), &token) == ESP_OK);
    assert(token != 0);
    return token;
}
static void RunWorker()
{
    Fake::currentTask = s_worker;
    try { Worker(NULL); } catch (const Fake::StopTask &) {}
    Fake::currentTask = reinterpret_cast<TaskHandle_t>(99);
}
static esp_err_t FailedResult(uint32_t token)
{
    const std::vector<uint8_t> originalActive(Fake::flash.begin() + 0x350000, Fake::flash.begin() + 0x630000);
    RunWorker();
    bool done = false;
    esp_err_t result = ProductOta_PollInstall(token, &done);
    assert(done && result != ESP_OK);
    assert(Fake::selectionCalls == 0 && Fake::restartCalls == 0);
    assert(Fake::bootIndex == Fake::runningIndex);
    assert(std::equal(originalActive.begin(), originalActive.end(), Fake::flash.begin() + 0x350000));
    assert(!Fake::liveHandle && Fake::liveAllocations == 0 && Fake::liveFiles == 0);
    ProductOta_ReleaseInstall(token);
    assert(!s_operation.active);
    return result;
}
static void ExpectRestart()
{
    bool restarted = false;
    try { RunWorker(); } catch (const Fake::Restart &) { restarted = true; }
    assert(restarted && Fake::restartCalls == 1 && Fake::selectionCalls == 1);
    assert(Fake::restartInsideLock && s_activationAttempted);
    assert(Fake::liveAllocations == 0 && !Fake::liveHandle && Fake::liveFiles == 0);
    assert(Fake::closeCalls == 1);
}

int main()
{
    Fake::packagePath = "/tmp/product-ota-engine-" + std::to_string(getpid()) + ".bin";

    // Compile the real mapping and real CLR HRESULT definitions. In particular,
    // positive SDK error values must never accidentally pass SUCCEEDED().
    assert(MapOtaError(ESP_OK) == S_OK && SUCCEEDED(MapOtaError(ESP_OK)));
    const esp_err_t errors[] = {
        ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
        ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_CRC,
        PRODUCT_OTA_ERR_NOT_READY, PRODUCT_OTA_ERR_BUSY, PRODUCT_OTA_ERR_LAYOUT,
        PRODUCT_OTA_ERR_STATE, PRODUCT_OTA_ERR_PATH, PRODUCT_OTA_ERR_IO,
        PRODUCT_OTA_ERR_HEADER, PRODUCT_OTA_ERR_SIGNATURE, PRODUCT_OTA_ERR_CRC,
        PRODUCT_OTA_ERR_HASH, PRODUCT_OTA_ERR_FLASH, PRODUCT_OTA_ERR_CANCELLED,
        PRODUCT_OTA_ERR_MEMORY, PRODUCT_OTA_ERR_DESCRIPTOR, PRODUCT_OTA_ERR_TAIL,
        PRODUCT_OTA_ERR_SPACE, PRODUCT_OTA_ERR_WATCHDOG, PRODUCT_OTA_ERR_TIMEOUT,
        0x7fffffff
    };
    for (esp_err_t error : errors) assert(FAILED(MapOtaError(error)));
    assert(MapOtaError(ESP_ERR_NO_MEM) == CLR_E_OUT_OF_MEMORY);
    assert(MapOtaError(ESP_ERR_INVALID_ARG) == CLR_E_INVALID_PARAMETER);
    assert(MapOtaError(ESP_ERR_TIMEOUT) == CLR_E_TIMEOUT);
    assert(MapOtaError(PRODUCT_OTA_ERR_FLASH) == CLR_E_FILE_IO);
    assert(MapOtaError(PRODUCT_OTA_ERR_SIGNATURE) == CLR_E_INVALID_OPERATION);

    // Execute the public native helpers used by the managed diagnostics API.
    Reset();
    uint64_t freeBytes = 123;
    assert(ProductOta_GetStagingFreeBytes(NULL) == ESP_ERR_INVALID_ARG);
    assert(ProductOta_GetStagingFreeBytes(&freeBytes) == ESP_OK);
    assert(freeBytes == Fake::filesystemTotal - Fake::filesystemUsed);
    Fake::filesystemUsed = Fake::filesystemTotal;
    assert(ProductOta_GetStagingFreeBytes(&freeBytes) == ESP_OK && freeBytes == 0);
    Fake::filesystemUsed = Fake::filesystemTotal + 1;
    freeBytes = 123;
    assert(ProductOta_GetStagingFreeBytes(&freeBytes) == PRODUCT_OTA_ERR_IO && freeBytes == 0);
    Fake::filesystemUsed = 0; Fake::filesystemResult = ESP_ERR_TIMEOUT;
    freeBytes = 123;
    assert(ProductOta_GetStagingFreeBytes(&freeBytes) == PRODUCT_OTA_ERR_IO && freeBytes == 0);

    char bootStatus[192] = {};
    assert(ProductOta_GetBootStatus(NULL, sizeof(bootStatus)) == ESP_ERR_INVALID_ARG);
    assert(ProductOta_GetBootStatus(bootStatus, 0) == ESP_ERR_INVALID_ARG);
    assert(ProductOta_GetBootStatus(bootStatus, sizeof(bootStatus)) == ESP_OK);
    assert(strstr(bootStatus, "slot=ota_0 managed=deploy_a release=running layout=2 trial=0 reset=3") != NULL);
    assert(strstr(bootStatus, RequireSignature ? "signature_required=1" : "signature_required=0") != NULL);
    assert(strstr(bootStatus, AllowVsDeploy ? "vs_deploy=1" : "vs_deploy=0") != NULL);
    bool developmentWarning = false;
    for (const auto &message : Fake::logs)
        developmentWarning |= message.find("DEVELOPMENT: Visual Studio deployment enabled") != std::string::npos;
    assert(developmentWarning == AllowVsDeploy);
    char truncated[4] = {'x', 'x', 'x', 'x'};
    assert(ProductOta_GetBootStatus(truncated, sizeof(truncated)) == ESP_ERR_INVALID_SIZE);
    assert(truncated[sizeof(truncated) - 1] == 0);
    Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ProductOta_GetBootStatus(bootStatus, sizeof(bootStatus)) == ESP_OK);
    assert(strstr(bootStatus, "trial=1") != NULL);
    Fake::stateResult = ESP_ERR_NOT_FOUND;
    assert(ProductOta_GetBootStatus(bootStatus, sizeof(bootStatus)) == PRODUCT_OTA_ERR_STATE);
    s_initialized = false;
    assert(ProductOta_GetBootStatus(bootStatus, sizeof(bootStatus)) == PRODUCT_OTA_ERR_NOT_READY);

    // The same public validation path protects first startup and every CLR soft
    // restart. VS development accepts changed content/length; strict builds
    // still reject a byte change, a shorter image, an appended assembly, and a
    // non-erased byte at the very end of the deployment partition.
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        Reset(false);
        if (mode == 0) Fake::flash[0x350000] ^= 1;
        if (mode == 1) memset(&Fake::flash[0x350000 + 8], 0xff, 8);
        if (mode == 2) Fake::flash[0x350000 + 16] = 0x42;
        if (mode == 3) Fake::flash[0x630000 - 1] = 0;
        assert(ProductOta_Initialize() == ESP_OK);
        const esp_err_t expected = AllowVsDeploy ? ESP_OK : (mode < 2 ? PRODUCT_OTA_ERR_HASH : PRODUCT_OTA_ERR_TAIL);
        assert(ProductOta_ValidateRunningPair() == expected);
        assert(s_pairValidated == AllowVsDeploy);
        ProductOta_PrepareSoftReboot();
        assert(Fake::restartCalls == 0);
        assert(ProductOta_ValidateRunningPair() == expected);
        assert(s_pairValidated == AllowVsDeploy);
        assert(Fake::nativeWrites == 0 && Fake::managedWrites == 0 && Fake::erases == 0);
        assert(Fake::liveAllocations == 0);
    }

    // Even development mode requires the exact supported partition geometry.
    Reset(false);
    --Fake::partitions[4].size;
    assert(ProductOta_Initialize() == PRODUCT_OTA_ERR_LAYOUT && !s_initialized);
    ++Fake::partitions[4].size;
    assert(ProductOta_Initialize() == ESP_OK);
    --Fake::partitions[4].size;
    assert(ProductOta_ValidateRunningPair() == PRODUCT_OTA_ERR_DESCRIPTOR && !s_pairValidated);
    ++Fake::partitions[4].size;

    // The development switch must not extend the 60-second trial or permit a
    // debugger-induced CLR reboot to escape the bootloader rollback path.
    Reset(false);
    Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ProductOta_Initialize() == ESP_OK);
    assert(s_trialDeadline == Fake::now + 60000000);
    assert(ProductOta_ValidateRunningPair() == ESP_OK);
    Fake::now = s_trialDeadline;
    assert(ProductOta_ConfirmBoot() == PRODUCT_OTA_ERR_TIMEOUT && Fake::confirmCalls == 0);
    bool developmentTrialRestart = false;
    try { ProductOta_PrepareSoftReboot(); } catch (const Fake::Restart &) { developmentTrialRestart = true; }
    assert(developmentTrialRestart && Fake::restartCalls == 1);

    // Both package payload hashes remain mandatory under every combination of
    // signature policy and VS deployment. A signed envelope reaches these
    // checks before the deliberately failing cryptographic SDK boundary.
    for (unsigned payload = 0; payload < 2; ++payload)
    {
        Reset();
        Fake::package.resize(Fake::package.size() + 64, 0);
        Put32(Fake::package, 12, 1);
        Put32(Fake::package, 16, Fake::package.size());
        Fake::package[128 + (payload == 0 ? 0 : 8192)] ^= 1;
        SavePackage();
        assert(FailedResult(Start()) == PRODUCT_OTA_ERR_HASH);
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0);
        assert(Fake::nativeWrites == 0 && Fake::managedWrites == 0 && Fake::erases == 0);
    }

    // POSIX I/O failures still fail closed before an inactive write. The signed
    // envelope reaches payload reads under both signature policies; its fake
    // crypto is irrelevant because these injected failures happen first.
    for (unsigned fault = 0; fault < 5; ++fault)
    {
        Reset();
        Fake::package.resize(Fake::package.size() + 64, 0);
        Put32(Fake::package, 12, 1);
        Put32(Fake::package, 16, Fake::package.size());
        SavePackage();
        if (fault == 0) Fake::openError = EIO;
        if (fault == 1) Fake::statError = EIO;
        if (fault == 2) Fake::seekError = EIO;
        if (fault == 3) Fake::failReadCall = 2;
        if (fault == 4) Fake::eofReadCall = 2;
        assert(FailedResult(Start()) == PRODUCT_OTA_ERR_IO);
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0 && Fake::erases == 0);
        assert(Fake::closeCalls == (fault == 0 ? 0 : 1));
    }

    if (RequireSignature)
    {
        // Exercise the real production policy branch independently of the
        // development installation tests. Unsigned input must fail even when
        // CRC and SHA256 are correct, before metadata/native/deployment writes.
        Reset();
        assert(FailedResult(Start()) == PRODUCT_OTA_ERR_HEADER);
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0);
        assert(Fake::nativeWrites == 0 && Fake::managedWrites == 0);

        // Well-formed signed envelope with an invalid key/signature reaches
        // signature validation but never any destructive operation.
        Reset();
        Fake::package.resize(Fake::package.size() + 64, 0);
        Put32(Fake::package, 12, 1);
        Put32(Fake::package, 16, Fake::package.size());
        SavePackage();
        assert(FailedResult(Start()) == PRODUCT_OTA_ERR_SIGNATURE);
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0);
        assert(Fake::nativeWrites == 0 && Fake::managedWrites == 0);
        assert(remove(Fake::packagePath.c_str()) == 0);
        printf("Product OTA actual-engine signature_required=1 vs_deploy=%u passed: %u scenarios; HRESULT map passed\n",
               static_cast<unsigned>(AllowVsDeploy), Fake::scenarios);
        return 0;
    }

    // A representative full-size transaction must issue block reads, not a
    // stdio refill for each byte. All three package passes retain their hashes
    // and exact byte content. The wrapper enforces the 4096-byte upper bound.
    Reset();
    const size_t largeNative = 1400000, largeManaged = 313276;
    MakePackage(largeNative, largeManaged);
    Fake::filesystemUsed = Fake::package.size() + 4096;
    (void)Start(); ExpectRestart();
    const size_t payloadReads = (largeNative + 4095) / 4096 + (largeManaged + 4095) / 4096;
    assert(Fake::readCalls == static_cast<int>(3 * payloadReads + 4));
    assert(Fake::maximumReadRequest == 4096);
    assert(memcmp(&Fake::flash[0x1B0000], &Fake::package[128], largeNative) == 0);
    assert(memcmp(&Fake::flash[0x630000], &Fake::package[128 + largeNative], largeManaged) == 0);

    // Interrupted operations and positive short reads must resume at the exact
    // byte offset without skipping or duplicating any data.
    Reset();
    Fake::openInterrupts = Fake::statInterrupts = Fake::seekInterrupts = Fake::readInterrupts = 2;
    Fake::readLimit = 31;
    (void)Start(); ExpectRestart();
    assert(Fake::openInterrupts == 0 && Fake::statInterrupts == 0);
    assert(Fake::seekInterrupts == 0 && Fake::readInterrupts == 0);
    assert(memcmp(&Fake::flash[0x1B0000], &Fake::package[128], 8192) == 0);
    assert(memcmp(&Fake::flash[0x630000], &Fake::package[128 + 8192], 257) == 0);

    Reset(); Fake::failReadAtEof = true;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_IO);
    assert(Fake::beginCalls == 0 && Fake::closeCalls == 1);

    // A final close failure, including EINTR, forbids activation and must never
    // close a possibly reused descriptor a second time during worker cleanup.
    for (int error : {EIO, EINTR})
    {
        Reset(); Fake::closeError = error;
        assert(FailedResult(Start()) == PRODUCT_OTA_ERR_IO);
        assert(Fake::nativeWrites > 0 && Fake::managedWrites > 0);
        assert(Fake::closeCalls == 1);
    }

    // Each SDK fault executes the real public start -> worker -> cleanup -> poll
    // path. The fake additionally rejects any active deployment erase/write.
    Reset(); Fake::metadataResult = ESP_ERR_TIMEOUT;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::beginCalls == 0);

    Reset(); Fake::beginResult = ESP_ERR_TIMEOUT;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::abortCalls == 1 && Fake::endCalls == 0);

    Reset(); Fake::nativeWriteResult = ESP_ERR_TIMEOUT;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::abortCalls == 1 && Fake::managedWrites == 0);

    Reset(); Fake::endResult = ESP_ERR_INVALID_CRC;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::abortCalls == 0 && Fake::erases == 0);

    Reset(); Fake::managedEraseResult = ESP_ERR_TIMEOUT;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::abortCalls == 0 && Fake::managedWrites == 0);

    Reset(); Fake::managedWriteResult = ESP_ERR_TIMEOUT;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_FLASH);
    assert(Fake::abortCalls == 0);

    Reset(); Fake::readbackCorrupt = true;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_HASH);

    Reset(); Fake::managedReadbackCorrupt = true;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_HASH);

    Reset(); Fake::managedTailCorrupt = true;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_TAIL);

    // A VS-deployed running image is also accepted as the fallback in the
    // explicit development mode. Strict builds reject it before any write.
    Reset(); Fake::flash[0x350000] ^= 1;
    uint32_t modifiedActive = Start();
    if (AllowVsDeploy)
    {
        ExpectRestart();
        assert(Fake::flash[0x350000] == (static_cast<uint8_t>('r') ^ 1));
    }
    else
    {
        assert(FailedResult(modifiedActive) == PRODUCT_OTA_ERR_HASH);
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0 && Fake::erases == 0);
    }

    // Signature/CRC/format/integrity failures precede ALL inactive writes.
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        Reset();
        if (mode == 0) Fake::package[128] ^= 1;
        if (mode == 1) Fake::package.resize(Fake::package.size() - 1);
        if (mode == 2) Fake::package[32] = 'X';
        SavePackage();
        (void)FailedResult(Start(mode == 3));
        assert(Fake::metadataWrites == 0 && Fake::beginCalls == 0);
        assert(Fake::nativeWrites == 0 && Fake::managedWrites == 0 && Fake::erases == 0);
    }

    Reset(); Fake::cancelOnClose = true;
    assert(FailedResult(Start()) == PRODUCT_OTA_ERR_CANCELLED);
    assert(Fake::nativeWrites > 0 && Fake::managedWrites > 0);

    Reset(); (void)Start();
    uint32_t second = 0;
    assert(ProductOta_StartInstall("I:\\updates\\update.bin", 0, &second) == PRODUCT_OTA_ERR_BUSY);
    assert(second == 0);
    assert(ProductOta_ConfirmBoot() == PRODUCT_OTA_ERR_STATE && Fake::confirmCalls == 0);
    assert(FailedResult((s_operation.cancel = true, s_operation.token)) == PRODUCT_OTA_ERR_CANCELLED);

    Reset(); uint32_t abandoned = Start();
    ProductOta_ReleaseInstall(abandoned);
    assert(s_operation.active && s_operation.cancel && s_operation.abandoned);
    RunWorker();
    assert(!s_operation.active && Fake::liveAllocations == 0 && !Fake::liveHandle);
    assert(Fake::selectionCalls == 0);

    Reset(); (void)Start(); Fake::selectionResult = ESP_ERR_TIMEOUT;
    ExpectRestart(); // An error returned by persistent selection MUST still reset.

    Reset(); (void)Start(); ExpectRestart();
    assert(memcmp(&Fake::flash[0x630000], &Fake::package[128 + 8192], 257) == 0);
    assert(Fake::flash[0x630000 + 257] == 0xff && Fake::flash[0x910000 - 1] == 0xff);

    // With an initially erased table, the pinned bootloader creates A's VALID
    // sequence-1 record before app_main. B legitimately has no prior record.
    Reset();
    memset(&Fake::flash[0x911000], 0xff, 4096);
    (void)Start(); ExpectRestart();
    assert(Fake::metadataWrites == 0 && Fake::beginCalls == 1);

    // Conversely, NOT_FOUND at app_main is an SDK/metadata failure, not evidence
    // that the runtime may invent or silently confirm initial metadata.
    Reset();
    memset(&Fake::flash[0x910000], 0xff, 8192);
    Fake::stateResult = ESP_ERR_NOT_FOUND;
    s_initialized = false;
    assert(ProductOta_Initialize() == PRODUCT_OTA_ERR_STATE && !s_initialized);
    uint32_t unavailableToken = 0;
    assert(ProductOta_StartInstall("I:\\updates\\update.bin", 0, &unavailableToken) == PRODUCT_OTA_ERR_NOT_READY);
    assert(Fake::selectionCalls == 0 && Fake::confirmCalls == 0 && Fake::metadataWrites == 0);

    Reset();
    Fake::stateResult = ESP_ERR_NOT_FOUND;
    assert(ProductOta_StartInstall("I:\\updates\\update.bin", 0, &unavailableToken) == PRODUCT_OTA_ERR_STATE);
    assert(Fake::selectionCalls == 0 && Fake::confirmCalls == 0 && Fake::metadataWrites == 0);

    // Confirmation has independent identity, state, deadline, and mutex guards.
    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    s_trialDeadline = Fake::now + 1000;
    assert(ProductOta_ConfirmBoot() == ESP_OK && Fake::confirmCalls == 1 && s_trialDeadline == 0);
    assert(ProductOta_ConfirmBoot() == ESP_OK && Fake::confirmCalls == 1);
    // A stale supervisor callback after success must not reject a VALID image.
    s_trialDeadline = Fake::now - 1; Fake::stopOnDelay = true;
    try { Supervisor(NULL); } catch (const Fake::StopTask &) {}
    Fake::stopOnDelay = false;
    assert(Fake::rejectCalls == 0 && Fake::restartCalls == 0 && s_trialDeadline == 0);

    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY; s_trialDeadline = Fake::now;
    assert(ProductOta_ConfirmBoot() == PRODUCT_OTA_ERR_TIMEOUT && Fake::confirmCalls == 0);

    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY; s_trialDeadline = Fake::now + 1000;
    Fake::bootIndex = 3;
    assert(ProductOta_ConfirmBoot() == PRODUCT_OTA_ERR_STATE && Fake::confirmCalls == 0);

    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY; s_trialDeadline = Fake::now + 1000;
    Fake::confirmResult = ESP_ERR_TIMEOUT;
    assert(ProductOta_ConfirmBoot() == PRODUCT_OTA_ERR_STATE);
    assert(Fake::states[0] == ESP_OTA_IMG_PENDING_VERIFY && s_trialDeadline != 0);

    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY; s_trialDeadline = Fake::now - 1;
    Fake::rejectResult = ESP_ERR_TIMEOUT;
    bool supervisorRestart = false;
    try { Supervisor(NULL); } catch (const Fake::Restart &) { supervisorRestart = true; }
    assert(supervisorRestart && Fake::rejectCalls == 1 && Fake::restartCalls == 1);

    Reset(); Fake::states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    bool trialRestart = false;
    try { ProductOta_PrepareSoftReboot(); } catch (const Fake::Restart &) { trialRestart = true; }
    assert(trialRestart && Fake::restartCalls == 1);

    assert(Fake::lockDepth == 0 && Fake::liveAllocations == 0 && !Fake::liveHandle);
    assert(remove(Fake::packagePath.c_str()) == 0);
    printf("Product OTA actual-engine signature_required=0 vs_deploy=%u passed: %u scenarios; HRESULT map passed\n",
           static_cast<unsigned>(AllowVsDeploy), Fake::scenarios);
    return 0;
}
