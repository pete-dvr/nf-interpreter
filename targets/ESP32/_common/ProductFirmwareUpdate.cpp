//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include "ProductFirmwareUpdate.h"
#include "ProductOtaPackage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_flash_partitions.h>
#include <esp_heap_caps.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>

#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "Product OTA requires a bootloader built with rollback enabled."
#endif
#if !defined(CONFIG_NF_FEATURE_PRODUCT_OTA) || !CONFIG_NF_FEATURE_PRODUCT_OTA
#error "Compile ProductFirmwareUpdate.cpp only with the product OTA Kconfig configuration."
#endif
#ifndef NF_PRODUCT_OTA_MIN_FREE_BYTES
#define NF_PRODUCT_OTA_MIN_FREE_BYTES (1024U * 1024U)
#endif
#ifndef NF_PRODUCT_OTA_ALLOW_VS_DEPLOY
#define NF_PRODUCT_OTA_ALLOW_VS_DEPLOY 0
#endif
#if NF_PRODUCT_OTA_ALLOW_VS_DEPLOY != 0 && NF_PRODUCT_OTA_ALLOW_VS_DEPLOY != 1
#error "NF_PRODUCT_OTA_ALLOW_VS_DEPLOY must be 0 or 1."
#endif

namespace
{
using namespace ProductOtaPackage;

const char *const Tag = "ProductOTA";
const size_t BufferSize = 4096;
const int64_t TrialTimeoutUs = 60LL * 1000 * 1000;
const int64_t QuiesceTimeoutUs = 15LL * 1000 * 1000;
const uint32_t WatchdogTimeoutMs = 10000;
const char StagingPath[] = "/I/updates/update.bin";
const bool AllowVsDeploy = NF_PRODUCT_OTA_ALLOW_VS_DEPLOY != 0;
#if defined(CONFIG_NF_PRODUCT_OTA_REQUIRE_SIGNATURE) && CONFIG_NF_PRODUCT_OTA_REQUIRE_SIGNATURE
const bool RequireSignature = true;
#else
// Kconfig defaults this option to enabled; disabling it is an explicit
// development build configuration checked by the product CMake module.
const bool RequireSignature = false;
#endif

StaticSemaphore_t s_lifecycleStorage;
SemaphoreHandle_t s_lifecycle;
TaskHandle_t s_worker;
TaskHandle_t s_supervisor;
bool s_initialized;
bool s_recovery;
bool s_activationAttempted;
bool s_pairValidated;
int64_t s_trialDeadline;
uint32_t s_nextToken;
portMUX_TYPE s_errorMux = portMUX_INITIALIZER_UNLOCKED;
char s_lastError[160];
char s_workerError[sizeof(s_lastError)];

struct Operation
{
    bool active;
    bool done;
    bool cancel;
    bool abandoned;
    uint32_t token;
    uint32_t crc;
    esp_err_t result;
    char path[sizeof(StagingPath)];
    char error[sizeof(s_lastError)];
};
Operation s_operation;

class LifecycleLock
{
  public:
    LifecycleLock()
    {
        xSemaphoreTake(s_lifecycle, portMAX_DELAY);
    }
    ~LifecycleLock()
    {
        xSemaphoreGive(s_lifecycle);
    }
};

esp_err_t Failure(esp_err_t productError, const char *phase, int cause)
{
    char message[sizeof(s_lastError)] = {};
    snprintf(
        message,
        sizeof(message),
        "Product OTA error=0x%04x phase=%s cause=0x%x",
        (unsigned)productError,
        phase,
        (unsigned)cause);
    portENTER_CRITICAL(&s_errorMux);
    memcpy(s_lastError, message, sizeof(s_lastError));
    s_lastError[sizeof(s_lastError) - 1] = 0;
    if (xTaskGetCurrentTaskHandle() == s_worker)
    {
        memcpy(s_workerError, message, sizeof(s_workerError));
    }
    portEXIT_CRITICAL(&s_errorMux);
    ESP_LOGE(Tag, "error=0x%04x phase=%s cause=0x%x", (unsigned)productError, phase, (unsigned)cause);
    return productError;
}

bool SamePartition(const esp_partition_t *a, const esp_partition_t *b)
{
    return a != NULL && b != NULL && a->address == b->address && a->size == b->size && a->type == b->type &&
           a->subtype == b->subtype && strcmp(a->label, b->label) == 0;
}

const esp_partition_t *ExactPartition(
    esp_partition_type_t type,
    esp_partition_subtype_t subtype,
    const char *label,
    uint32_t address,
    uint32_t size)
{
    const esp_partition_t *p = esp_partition_find_first(type, subtype, label);
    // Encrypted data writes need different alignment and provisioning semantics.
    // This initial product layout deliberately rejects encrypted partitions.
    if (p == NULL || p->address != address || p->size != size || p->erase_size != 4096 || p->encrypted)
    {
        return NULL;
    }
    return p;
}

const esp_partition_t *RuntimePartition(unsigned slot)
{
    return ExactPartition(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot),
        slot == 0 ? "ota_0" : "ota_1",
        slot == 0 ? 0x10000 : 0x1B0000,
        RuntimeCapacity);
}

esp_err_t ValidateLayout()
{
    if (esp_ota_get_app_partition_count() != 2 ||
        ProductOta_GetDeploymentPartition(RuntimePartition(0)) == NULL ||
        ProductOta_GetDeploymentPartition(RuntimePartition(1)) == NULL ||
        ExactPartition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs", 0x9000, 0x6000) == NULL ||
        ExactPartition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, "phy_init", 0xF000, 0x1000) == NULL ||
        ExactPartition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata", 0x910000, 0x2000) == NULL ||
        ExactPartition(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x83, "config", 0x912000, 0x6EE000) == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_LAYOUT, "partition-layout", ESP_ERR_INVALID_STATE);
    }
    // Reject extra executable partitions and duplicated managed labels/subtypes.
    unsigned apps = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL)
    {
        ++apps;
        it = esp_partition_next(it);
    }
    unsigned deployments = 0;
    it = esp_partition_find(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x84, NULL);
    while (it != NULL)
    {
        ++deployments;
        it = esp_partition_next(it);
    }
    unsigned data = 0;
    it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL)
    {
        ++data;
        it = esp_partition_next(it);
    }
    return apps == 2 && deployments == 2 && data == 6
               ? ESP_OK
               : Failure(PRODUCT_OTA_ERR_LAYOUT, "partition-count", ESP_FAIL);
}

esp_err_t RunningState(esp_ota_img_states_t &state)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!SamePartition(running, esp_ota_get_boot_partition()) ||
        ProductOta_GetDeploymentPartition(running) == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "running-boot-identity", ESP_ERR_INVALID_STATE);
    }
    // IDF 5.5.5 bootloader set_actual_ota_seq() writes initial ota_0 as VALID
    // before entering the app when both records are erased and no factory
    // exists. NOT_FOUND here is therefore not a healthy provisioning exception:
    // it can also mean a failed bootloader write or failed metadata mapping.
    esp_err_t error = esp_ota_get_state_partition(running, &state);
    return error == ESP_OK ? ESP_OK : Failure(PRODUCT_OTA_ERR_STATE, "running-state", error);
}

bool CancelRequested()
{
    LifecycleLock lock;
    return s_operation.cancel || s_recovery;
}

esp_err_t CheckCancellation()
{
    return CancelRequested() ? Failure(PRODUCT_OTA_ERR_CANCELLED, "cancel", ESP_OK) : ESP_OK;
}

class Sha256
{
  public:
    mbedtls_sha256_context context;
    int status;
    Sha256()
    {
        mbedtls_sha256_init(&context);
        status = mbedtls_sha256_starts(&context, 0);
    }
    ~Sha256()
    {
        mbedtls_sha256_free(&context);
    }
    bool Update(const uint8_t *bytes, size_t length)
    {
        if (status == 0)
        {
            status = mbedtls_sha256_update(&context, bytes, length);
        }
        return status == 0;
    }
    bool Finish(uint8_t *digest)
    {
        if (status == 0)
        {
            status = mbedtls_sha256_finish(&context, digest);
        }
        return status == 0;
    }
};

esp_err_t HashPartition(
    const esp_partition_t *partition,
    uint32_t length,
    uint8_t *buffer,
    const uint8_t expected[32],
    bool cancellable)
{
    if (partition == NULL || length == 0 || length > partition->size)
    {
        return Failure(PRODUCT_OTA_ERR_LAYOUT, "partition-hash-range", ESP_ERR_INVALID_SIZE);
    }
    Sha256 hash;
    for (uint32_t offset = 0; offset < length;)
    {
        if (cancellable && CancelRequested())
        {
            return Failure(PRODUCT_OTA_ERR_CANCELLED, "partition-hash", ESP_OK);
        }
        const size_t chunk = length - offset < BufferSize ? length - offset : BufferSize;
        esp_err_t error = esp_partition_read(partition, offset, buffer, chunk);
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, "partition-read", error);
        }
        if (!hash.Update(buffer, chunk))
        {
            return Failure(PRODUCT_OTA_ERR_HASH, "partition-sha256", hash.status);
        }
        offset += chunk;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    if (!hash.Finish(digest) || memcmp(digest, expected, sizeof(digest)) != 0)
    {
        return Failure(PRODUCT_OTA_ERR_HASH, "partition-hash-compare", hash.status);
    }
    return ESP_OK;
}

esp_err_t CheckErasedTail(const esp_partition_t *partition, uint32_t length, uint8_t *buffer, bool cancellable)
{
    for (uint32_t offset = length; offset < partition->size;)
    {
        if (cancellable && CancelRequested())
        {
            return Failure(PRODUCT_OTA_ERR_CANCELLED, "deployment-tail", ESP_OK);
        }
        const size_t chunk = partition->size - offset < BufferSize ? partition->size - offset : BufferSize;
        esp_err_t error = esp_partition_read(partition, offset, buffer, chunk);
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, "deployment-tail-read", error);
        }
        for (size_t i = 0; i < chunk; ++i)
        {
            if (buffer[i] != 0xff)
            {
                return Failure(PRODUCT_OTA_ERR_TAIL, "deployment-tail", ESP_ERR_INVALID_STATE);
            }
        }
        offset += chunk;
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t ValidateDescriptor(uint8_t *buffer)
{
    const ProductReleaseDescriptor &descriptor = g_ProductReleaseDescriptor;
    const esp_partition_t *deployment = ProductOta_GetDeploymentPartition(esp_ota_get_running_partition());
    if (deployment == NULL || descriptor.layoutId != LayoutId ||
        memcmp(descriptor.targetId, TargetId, sizeof(TargetId)) != 0 ||
        !ValidIdentifier(descriptor.releaseId) || descriptor.managedLength == 0 ||
        descriptor.managedLength > deployment->size)
    {
        return Failure(PRODUCT_OTA_ERR_DESCRIPTOR, "release-descriptor", ESP_ERR_INVALID_ARG);
    }
    // Explicit development builds permit Visual Studio to replace the running
    // managed image independently of the embedded release descriptor. Keep the
    // descriptor/partition checks above; incoming OTA payloads and inactive-slot
    // readback still use their unconditional hash and erased-tail checks.
    if (AllowVsDeploy)
    {
        return ESP_OK;
    }
    esp_err_t error = HashPartition(deployment, descriptor.managedLength, buffer, descriptor.managedSha256, false);
    if (error == ESP_OK)
    {
        error = CheckErasedTail(deployment, descriptor.managedLength, buffer, false);
    }
    return error;
}

esp_err_t VerifySignature(const uint8_t header[128], const uint8_t signature[64])
{
    if (g_ProductOtaPublicKeyLength != 65 || g_ProductOtaPublicKey[0] != 4)
    {
        return Failure(PRODUCT_OTA_ERR_SIGNATURE, "pinned-key", ESP_ERR_INVALID_SIZE);
    }
    const uint8_t domain[] = "PRDOTA-SIGN-v1";
    uint8_t digest[32];
    Sha256 hash;
    if (!hash.Update(domain, sizeof(domain) - 1) || !hash.Update(header, HeaderLength) || !hash.Finish(digest))
    {
        return Failure(PRODUCT_OTA_ERR_SIGNATURE, "signature-sha256", hash.status);
    }
    mbedtls_ecp_group group;
    mbedtls_ecp_point key;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&key);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int error = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (error == 0)
    {
        error = mbedtls_ecp_point_read_binary(&group, &key, g_ProductOtaPublicKey, g_ProductOtaPublicKeyLength);
    }
    if (error == 0)
    {
        error = mbedtls_ecp_check_pubkey(&group, &key);
    }
    if (error == 0)
    {
        error = mbedtls_mpi_read_binary(&r, signature, 32);
    }
    if (error == 0)
    {
        error = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    }
    if (error == 0)
    {
        error = mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &key, &r, &s);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&key);
    mbedtls_ecp_group_free(&group);
    return error == 0 ? ESP_OK : Failure(PRODUCT_OTA_ERR_SIGNATURE, "signature-verify", error);
}

struct InstallContext
{
    int file;
    uint8_t *buffer;
    esp_ota_handle_t handle;
    Header header;
    uint8_t headerBytes[HeaderLength];
    uint8_t signature[64];
    uint32_t expectedCrc;

    InstallContext() : file(-1), buffer(NULL), handle(0), expectedCrc(0)
    {
    }

    esp_err_t Cleanup(esp_err_t priorError)
    {
        if (handle != 0)
        {
            const esp_ota_handle_t owned = handle;
            handle = 0;
            esp_err_t error = esp_ota_abort(owned);
            if (error != ESP_OK)
            {
                error = Failure(PRODUCT_OTA_ERR_FLASH, "ota-abort", error);
                if (priorError == ESP_OK)
                {
                    priorError = error;
                }
            }
        }
        if (file >= 0)
        {
            const int owned = file;
            file = -1;
            // Do not retry close after EINTR: the descriptor may already have
            // been released and reused by another task.
            if (close(owned) != 0)
            {
                esp_err_t error = Failure(PRODUCT_OTA_ERR_IO, "package-close", errno);
                if (priorError == ESP_OK)
                {
                    priorError = error;
                }
            }
        }
        if (buffer != NULL)
        {
            heap_caps_free(buffer);
            buffer = NULL;
        }
        return priorError;
    }
};

ssize_t ReadSome(int file, uint8_t *buffer, size_t length)
{
    ssize_t count;
    do
    {
        count = read(file, buffer, length);
    } while (count < 0 && errno == EINTR);
    return count;
}

esp_err_t ReadExact(int file, uint8_t *buffer, size_t length)
{
    while (length != 0)
    {
        // Use the VFS directly. Size-optimized newlib fread on an _IONBF file
        // otherwise refills its one-byte buffer once per byte, turning each
        // package pass into millions of filesystem calls.
        const size_t chunk = length < BufferSize ? length : BufferSize;
        const ssize_t count = ReadSome(file, buffer, chunk);
        if (count <= 0)
        {
            return Failure(PRODUCT_OTA_ERR_IO, "package-short-read", count < 0 ? errno : ESP_ERR_INVALID_SIZE);
        }
        buffer += count;
        length -= static_cast<size_t>(count);
    }
    return ESP_OK;
}

esp_err_t Seek(int file, uint32_t offset)
{
    off_t position;
    do
    {
        position = lseek(file, static_cast<off_t>(offset), SEEK_SET);
    } while (position < 0 && errno == EINTR);
    return position == static_cast<off_t>(offset)
               ? ESP_OK
               : Failure(PRODUCT_OTA_ERR_IO, "package-seek", position < 0 ? errno : ESP_ERR_INVALID_SIZE);
}

esp_err_t ValidateFile(InstallContext &context, bool firstPass)
{
    struct stat info;
    int status;
    do
    {
        status = fstat(context.file, &info);
    } while (status < 0 && errno == EINTR);
    if (status != 0 || !S_ISREG(info.st_mode) || info.st_size < HeaderLength)
    {
        return Failure(PRODUCT_OTA_ERR_IO, "package-stat", status != 0 ? errno : ESP_ERR_INVALID_SIZE);
    }
    esp_err_t error = Seek(context.file, 0);
    if (error != ESP_OK)
    {
        return error;
    }
    uint8_t bytes[HeaderLength];
    error = ReadExact(context.file, bytes, sizeof(bytes));
    if (error != ESP_OK)
    {
        return error;
    }
    Header parsed;
    if (!ParseHeader(bytes, sizeof(bytes), static_cast<uint64_t>(info.st_size), RequireSignature, parsed))
    {
        return Failure(PRODUCT_OTA_ERR_HEADER, "package-header", ESP_ERR_INVALID_ARG);
    }
    if (!firstPass && memcmp(bytes, context.headerBytes, sizeof(bytes)) != 0)
    {
        return Failure(PRODUCT_OTA_ERR_HEADER, "package-header-changed", ESP_ERR_INVALID_STATE);
    }
    if (firstPass)
    {
        context.header = parsed;
        memcpy(context.headerBytes, bytes, sizeof(bytes));
    }
    uint32_t crc = UpdateCrc(UINT32_MAX, bytes, sizeof(bytes));
    for (unsigned payload = 0; payload != 2; ++payload)
    {
        Sha256 hash;
        uint32_t remaining = payload == 0 ? parsed.runtimeLength : parsed.managedLength;
        while (remaining != 0)
        {
            if (CancelRequested())
            {
                return Failure(PRODUCT_OTA_ERR_CANCELLED, "package-validation", ESP_OK);
            }
            const size_t chunk = remaining < BufferSize ? remaining : BufferSize;
            error = ReadExact(context.file, context.buffer, chunk);
            if (error != ESP_OK)
            {
                return error;
            }
            crc = UpdateCrc(crc, context.buffer, chunk);
            if (!hash.Update(context.buffer, chunk))
            {
                return Failure(PRODUCT_OTA_ERR_HASH, "package-sha256", hash.status);
            }
            remaining -= chunk;
            vTaskDelay(1);
        }
        uint8_t digest[32];
        const uint8_t *expected = payload == 0 ? parsed.runtimeSha256 : parsed.managedSha256;
        if (!hash.Finish(digest) || memcmp(digest, expected, sizeof(digest)) != 0)
        {
            return Failure(PRODUCT_OTA_ERR_HASH, "package-payload-hash", hash.status);
        }
    }
    if (parsed.flags & 1)
    {
        uint8_t signature[64];
        error = ReadExact(context.file, signature, sizeof(signature));
        if (error != ESP_OK)
        {
            return error;
        }
        crc = UpdateCrc(crc, signature, sizeof(signature));
        if (!firstPass && memcmp(signature, context.signature, sizeof(signature)) != 0)
        {
            return Failure(PRODUCT_OTA_ERR_SIGNATURE, "signature-changed", ESP_ERR_INVALID_STATE);
        }
        error = VerifySignature(bytes, signature);
        if (error != ESP_OK)
        {
            return error;
        }
        if (firstPass)
        {
            memcpy(context.signature, signature, sizeof(signature));
        }
    }
    uint8_t trailing;
    const ssize_t trailingCount = ReadSome(context.file, &trailing, sizeof(trailing));
    if (trailingCount != 0)
    {
        return Failure(PRODUCT_OTA_ERR_IO, "package-trailing-data", trailingCount < 0 ? errno : ESP_ERR_INVALID_SIZE);
    }
    if ((crc ^ UINT32_MAX) != context.expectedCrc)
    {
        return Failure(PRODUCT_OTA_ERR_CRC, "package-crc32", ESP_ERR_INVALID_CRC);
    }
    return ESP_OK;
}

esp_err_t RetireInactiveMetadata(const esp_partition_t *target)
{
    const esp_partition_t *metadata =
        ExactPartition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata", 0x910000, 0x2000);
    if (metadata == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_LAYOUT, "otadata-geometry", ESP_ERR_NOT_FOUND);
    }
    // esp_ota_get_state_partition conflates missing records with failed mmap/read.
    // Read both records with checked public reads instead of accepting NOT_FOUND.
    // We never write the OTA record representation ourselves.
    bool targetRecord = false;
    bool runningRecord = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    for (unsigned sector = 0; sector != 2; ++sector)
    {
        esp_ota_select_entry_t record;
        esp_err_t error = esp_partition_read(metadata, sector * metadata->erase_size, &record, sizeof(record));
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, "otadata-read", error);
        }
        const uint32_t crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&record.ota_seq, sizeof(record.ota_seq));
        if (record.ota_seq != UINT32_MAX && record.ota_seq != 0 && record.crc == crc)
        {
            const unsigned slot = (record.ota_seq - 1) % 2;
            if (slot == (unsigned)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0))
            {
                targetRecord = true;
            }
            if (slot == (unsigned)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) &&
                record.ota_state == ESP_OTA_IMG_VALID)
            {
                runningRecord = true;
            }
        }
    }
    if (!runningRecord)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "otadata-running-valid", ESP_ERR_INVALID_STATE);
    }
    if (targetRecord)
    {
        esp_err_t error = esp_ota_invalidate_inactive_ota_data_slot();
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, "otadata-retire", error);
        }
    }
    return ESP_OK;
}

esp_err_t WritePayload(InstallContext &context, const esp_partition_t *deployment)
{
    const bool native = deployment == NULL;
    const uint32_t length = native ? context.header.runtimeLength : context.header.managedLength;
    const uint32_t start = HeaderLength + (native ? 0 : context.header.runtimeLength);
    esp_err_t error = Seek(context.file, start);
    if (error != ESP_OK)
    {
        return error;
    }
    Sha256 hash;
    for (uint32_t offset = 0; offset < length;)
    {
        error = CheckCancellation();
        if (error != ESP_OK)
        {
            return error;
        }
        const size_t chunk = length - offset < BufferSize ? length - offset : BufferSize;
        error = ReadExact(context.file, context.buffer, chunk);
        if (error != ESP_OK)
        {
            return error;
        }
        if (!hash.Update(context.buffer, chunk))
        {
            return Failure(PRODUCT_OTA_ERR_HASH, "written-sha256", hash.status);
        }
        error = native ? esp_ota_write(context.handle, context.buffer, chunk)
                       : esp_partition_write(deployment, offset, context.buffer, chunk);
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, native ? "runtime-write" : "managed-write", error);
        }
        offset += chunk;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    if (!hash.Finish(digest) ||
        memcmp(digest, native ? context.header.runtimeSha256 : context.header.managedSha256, sizeof(digest)) != 0)
    {
        return Failure(PRODUCT_OTA_ERR_HASH, "written-payload-changed", hash.status);
    }
    return ESP_OK;
}

esp_err_t Install(InstallContext &context, const Operation &operation)
{
    context.expectedCrc = operation.crc;
    context.buffer = (uint8_t *)heap_caps_malloc(BufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (context.buffer == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_MEMORY, "io-buffer", ESP_ERR_NO_MEM);
    }
    esp_err_t error = ValidateLayout();
    if (error != ESP_OK)
    {
        return error;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    const esp_partition_t *deployment = ProductOta_GetDeploymentPartition(target);
    if (target == NULL || deployment == NULL || SamePartition(running, target) ||
        SamePartition(ProductOta_GetDeploymentPartition(running), deployment))
    {
        return Failure(PRODUCT_OTA_ERR_LAYOUT, "inactive-pair", ESP_ERR_INVALID_STATE);
    }
    // Apply the running/fallback validation policy before any inactive write.
    // Production requires the exact managed image embedded in its descriptor.
    error = ValidateDescriptor(context.buffer);
    if (error != ESP_OK)
    {
        return error;
    }
    size_t total = 0;
    size_t used = 0;
    error = esp_littlefs_info("config", &total, &used);
    if (error != ESP_OK || used > total || total - used < NF_PRODUCT_OTA_MIN_FREE_BYTES)
    {
        return Failure(PRODUCT_OTA_ERR_SPACE, "filesystem-reserve", error);
    }
    do
    {
        context.file = open(operation.path, O_RDONLY);
    } while (context.file < 0 && errno == EINTR);
    if (context.file < 0)
    {
        return Failure(PRODUCT_OTA_ERR_IO, "package-open", errno);
    }
    error = ValidateFile(context, true);
    if (error != ESP_OK)
    {
        return error;
    }
    error = CheckCancellation();
    if (error != ESP_OK)
    {
        return error;
    }
    error = RetireInactiveMetadata(target);
    if (error != ESP_OK)
    {
        return error;
    }
    ESP_LOGI(Tag, "phase=runtime-program target=%s release=%.*s", target->label, 16, context.header.releaseId);
    // The pinned SDK may assign the handle before an erase failure. Cleanup owns
    // every nonzero handle, including a failed begin.
    error = esp_ota_begin(target, context.header.runtimeLength, &context.handle);
    if (error != ESP_OK)
    {
        return Failure(PRODUCT_OTA_ERR_FLASH, "ota-begin", error);
    }
    error = WritePayload(context, NULL);
    if (error != ESP_OK)
    {
        return error;
    }
    const esp_ota_handle_t completed = context.handle;
    context.handle = 0; // esp_ota_end frees ownership even when verification fails.
    error = esp_ota_end(completed);
    if (error != ESP_OK)
    {
        return Failure(PRODUCT_OTA_ERR_FLASH, "ota-end", error);
    }
    error = CheckCancellation();
    if (error != ESP_OK)
    {
        return error;
    }
    ESP_LOGI(Tag, "phase=managed-program target=%s", deployment->label);
    // Erase every sector, including the unused tail. Sector boundaries permit
    // acknowledged cancellation without killing a task in a flash operation.
    for (uint32_t offset = 0; offset < deployment->size; offset += deployment->erase_size)
    {
        error = CheckCancellation();
        if (error != ESP_OK)
        {
            return error;
        }
        error = esp_partition_erase_range(deployment, offset, deployment->erase_size);
        if (error != ESP_OK)
        {
            return Failure(PRODUCT_OTA_ERR_FLASH, "managed-erase", error);
        }
        vTaskDelay(1);
    }
    error = WritePayload(context, deployment);
    if (error != ESP_OK)
    {
        return error;
    }
    error = HashPartition(target, context.header.runtimeLength, context.buffer, context.header.runtimeSha256, true);
    if (error != ESP_OK)
    {
        return error;
    }
    error = HashPartition(
        deployment,
        context.header.managedLength,
        context.buffer,
        context.header.managedSha256,
        true);
    if (error != ESP_OK)
    {
        return error;
    }
    error = CheckErasedTail(deployment, context.header.managedLength, context.buffer, true);
    if (error != ESP_OK)
    {
        return error;
    }
    error = ValidateFile(context, false);
    if (error != ESP_OK)
    {
        return error;
    }
    error = context.Cleanup(ESP_OK);
    if (error != ESP_OK)
    {
        return error;
    }
    LifecycleLock lock;
    esp_ota_img_states_t state;
    error = RunningState(state);
    if (s_operation.cancel || s_recovery)
    {
        return Failure(PRODUCT_OTA_ERR_CANCELLED, "activation-cancel", ESP_OK);
    }
    if (error != ESP_OK || state != ESP_OTA_IMG_VALID)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "activation-running-state", error);
    }
    s_activationAttempted = true;
    error = esp_ota_set_boot_partition(target);
    ESP_LOGI(Tag, "phase=activation-attempt slot=%s result=0x%x; hardware restart", target->label, (unsigned)error);
    // An error does not prove that this persistent write did not take effect.
    // Never release the lifecycle lock or resume the CLR after attempting it.
    esp_restart();
    __builtin_unreachable();
}

void Worker(void *)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        Operation operation;
        {
            LifecycleLock lock;
            if (!s_operation.active || s_operation.done)
            {
                continue;
            }
            operation = s_operation;
        }
        InstallContext context;
        esp_err_t result = Install(context, operation);
        char diagnostic[sizeof(s_lastError)];
        portENTER_CRITICAL(&s_errorMux);
        memcpy(diagnostic, s_workerError, sizeof(diagnostic));
        portEXIT_CRITICAL(&s_errorMux);
        const esp_err_t cleaned = context.Cleanup(result);
        if (result == ESP_OK)
        {
            portENTER_CRITICAL(&s_errorMux);
            memcpy(diagnostic, s_workerError, sizeof(diagnostic));
            portEXIT_CRITICAL(&s_errorMux);
        }
        {
            LifecycleLock lock;
            s_operation.result = cleaned;
            memcpy(s_operation.error, diagnostic, sizeof(s_operation.error));
            s_operation.done = true;
            if (s_operation.abandoned)
            {
                memset(&s_operation, 0, sizeof(s_operation));
            }
        }
    }
}

// Must be called holding the lifecycle mutex. On success, the SDK does not
// return; on error the caller enters a non-destructive native recovery state.
esp_err_t RejectPendingLocked()
{
    esp_ota_img_states_t state;
    esp_err_t error = RunningState(state);
    if (error != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY || s_operation.active || s_activationAttempted)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "reject-state", error);
    }
    error = esp_ota_mark_app_invalid_rollback_and_reboot();
    return Failure(PRODUCT_OTA_ERR_STATE, "rollback-reboot-failed", error);
}

void Supervisor(void *)
{
    esp_err_t error = esp_task_wdt_add(NULL);
    if (error != ESP_OK)
    {
        Failure(PRODUCT_OTA_ERR_WATCHDOG, "supervisor-watchdog-add", error);
        esp_restart();
    }
    for (;;)
    {
        error = esp_task_wdt_reset();
        if (error != ESP_OK)
        {
            Failure(PRODUCT_OTA_ERR_WATCHDOG, "supervisor-watchdog-reset", error);
            esp_restart();
        }
        esp_err_t rejected = ESP_OK;
        {
            LifecycleLock lock;
            if (!s_recovery && s_trialDeadline != 0 && esp_timer_get_time() >= s_trialDeadline)
            {
                // Re-read under the same lock as confirmation; an expired
                // deadline can never reject an image that became VALID.
                esp_ota_img_states_t state;
                error = RunningState(state);
                if (error == ESP_OK && state == ESP_OTA_IMG_VALID)
                {
                    s_trialDeadline = 0;
                }
                else if (error == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
                {
                    ESP_LOGE(Tag, "phase=trial-timeout deadline_ms=60000");
                    rejected = RejectPendingLocked();
                }
                else
                {
                    rejected = Failure(PRODUCT_OTA_ERR_STATE, "trial-state", error);
                }
            }
        }
        if (rejected != ESP_OK)
        {
            // The CLR may already be executing. Parking only this task would
            // leave a failed trial application running. A hardware reset stops
            // all tasks and lets the bootloader resolve the pending metadata.
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
} // namespace

extern "C" const esp_partition_t *ProductOta_GetDeploymentPartition(const esp_partition_t *runtime)
{
    if (runtime == NULL)
    {
        return NULL;
    }
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        if (SamePartition(runtime, RuntimePartition(slot)))
        {
            return ExactPartition(
                ESP_PARTITION_TYPE_DATA,
                (esp_partition_subtype_t)0x84,
                slot == 0 ? "deploy_a" : "deploy_b",
                slot == 0 ? 0x350000 : 0x630000,
                ManagedCapacity);
        }
    }
    return NULL;
}

extern "C" esp_err_t ProductOta_Initialize(void)
{
    // app_main is the sole initializer. State and the supervisor deliberately
    // survive CLR-only restarts.
    if (s_initialized)
    {
        return ESP_OK;
    }
    if (s_lifecycle == NULL)
    {
        s_lifecycle = xSemaphoreCreateMutexStatic(&s_lifecycleStorage);
    }
    if (s_lifecycle == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_MEMORY, "lifecycle-mutex", ESP_ERR_NO_MEM);
    }
    esp_err_t error = ValidateLayout();
    if (error != ESP_OK)
    {
        return error;
    }
    esp_ota_img_states_t state;
    error = RunningState(state);
    if (error != ESP_OK || (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_PENDING_VERIFY))
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "boot-state", error);
    }
    if (RequireSignature && g_ProductOtaPublicKeyLength != 65)
    {
        return Failure(PRODUCT_OTA_ERR_SIGNATURE, "production-key-missing", ESP_ERR_INVALID_SIZE);
    }
    s_trialDeadline = state == ESP_OTA_IMG_PENDING_VERIFY ? esp_timer_get_time() + TrialTimeoutUs : 0;
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = WatchdogTimeoutMs;
    watchdog.idle_core_mask = (1U << portNUM_PROCESSORS) - 1;
    watchdog.trigger_panic = true;
    error = esp_task_wdt_init(&watchdog);
    if (error == ESP_ERR_INVALID_STATE)
    {
        error = esp_task_wdt_reconfigure(&watchdog);
    }
    if (error != ESP_OK)
    {
        return Failure(PRODUCT_OTA_ERR_WATCHDOG, "watchdog-configure", error);
    }
    if (xTaskCreateWithCaps(Supervisor, "ProductOTABoot", 4096, NULL, 6, &s_supervisor, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) !=
        pdPASS)
    {
        return Failure(PRODUCT_OTA_ERR_MEMORY, "supervisor-create", ESP_ERR_NO_MEM);
    }
    if (xTaskCreateWithCaps(Worker, "ProductOTAWrite", 10240, NULL, 4, &s_worker, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) !=
        pdPASS)
    {
        return Failure(PRODUCT_OTA_ERR_MEMORY, "worker-create", ESP_ERR_NO_MEM);
    }
    s_initialized = true;
    if (AllowVsDeploy)
    {
        ESP_LOGW(Tag, "DEVELOPMENT: Visual Studio deployment enabled; running managed image hash/tail checks disabled");
    }
    ESP_LOGI(
        Tag,
        "slot=%s managed=%s release=%.*s state=%u trial_timeout_ms=60000 signature_required=%d vs_deploy=%d",
        esp_ota_get_running_partition()->label,
        ProductOta_GetDeploymentPartition(esp_ota_get_running_partition())->label,
        16,
        g_ProductReleaseDescriptor.releaseId,
        (unsigned)state,
        (int)RequireSignature,
        (int)AllowVsDeploy);
    return ESP_OK;
}

extern "C" esp_err_t ProductOta_GetStagingFreeBytes(uint64_t *freeBytes)
{
    if (freeBytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *freeBytes = 0;
    size_t total = 0;
    size_t used = 0;
    esp_err_t error = esp_littlefs_info("config", &total, &used);
    if (error != ESP_OK || used > total)
    {
        return Failure(PRODUCT_OTA_ERR_IO, "staging-free-space", error);
    }
    *freeBytes = total - used;
    return ESP_OK;
}

extern "C" esp_err_t ProductOta_ValidateRunningPair(void)
{
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(BufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_MEMORY, "boot-io-buffer", ESP_ERR_NO_MEM);
    }
    esp_err_t error = ValidateDescriptor(buffer);
    heap_caps_free(buffer);
    {
        LifecycleLock lock;
        s_pairValidated = error == ESP_OK;
    }
    return error;
}

extern "C" esp_err_t ProductOta_GetBootStatus(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    bool trial = false;
    esp_err_t error = ProductOta_IsTrialBoot(&trial);
    if (error != ESP_OK)
    {
        return error;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *deployment = ProductOta_GetDeploymentPartition(running);
    if (running == NULL || deployment == NULL)
    {
        return Failure(PRODUCT_OTA_ERR_LAYOUT, "boot-status", ESP_ERR_NOT_FOUND);
    }
    int written = snprintf(
        buffer,
        size,
        "slot=%s managed=%s release=%.*s layout=%u trial=%u reset=%u signature_required=%u vs_deploy=%u",
        running->label,
        deployment->label,
        16,
        (const char *)g_ProductReleaseDescriptor.releaseId,
        (unsigned int)g_ProductReleaseDescriptor.layoutId,
        (unsigned int)trial,
        (unsigned int)esp_reset_reason(),
        (unsigned int)RequireSignature,
        (unsigned int)AllowVsDeploy);
    return written >= 0 && (size_t)written < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

extern "C" void ProductOta_FailBoot(esp_err_t error)
{
    ESP_LOGE(Tag, "error=0x%04x phase=boot-failure; CLR will not start", (unsigned)error);
    bool canRollback = false;
    if (s_lifecycle != NULL)
    {
        LifecycleLock lock;
        s_recovery = true;
        s_operation.cancel = true;
        s_trialDeadline = 0;
        canRollback = SamePartition(esp_ota_get_running_partition(), esp_ota_get_boot_partition()) &&
                      !s_activationAttempted && !s_operation.active && esp_ota_check_rollback_is_possible();
        if (canRollback)
        {
            esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(Tag, "phase=boot-rollback-failed cause=0x%x", (unsigned)rollback);
        }
    }
    // Defined recovery state: preserve all persistent partitions, stop the CLR,
    // and wait for physical service/reprovisioning. Never erase config or mark a
    // failed image healthy merely to escape an OTA error.
    ESP_LOGE(Tag, "phase=native-recovery; configuration preserved; service required");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" esp_err_t ProductOta_StartInstall(const char *path, uint32_t expectedCrc32, uint32_t *token)
{
    if (path == NULL || token == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *token = 0;
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    if (strcmp(path, StagingPath) != 0 && strcmp(path, "I:\\updates\\update.bin") != 0 &&
        strcmp(path, "I:/updates/update.bin") != 0)
    {
        return Failure(PRODUCT_OTA_ERR_PATH, "staging-path", ESP_ERR_INVALID_ARG);
    }
    LifecycleLock lock;
    if (s_operation.active || s_activationAttempted || s_recovery)
    {
        return Failure(PRODUCT_OTA_ERR_BUSY, "start-install", ESP_ERR_INVALID_STATE);
    }
    esp_ota_img_states_t state;
    esp_err_t error = RunningState(state);
    if (error != ESP_OK || state != ESP_OTA_IMG_VALID || !s_pairValidated)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "install-requires-valid-pair", error);
    }
    memset(&s_operation, 0, sizeof(s_operation));
    s_operation.active = true;
    s_operation.crc = expectedCrc32;
    if (++s_nextToken == 0)
    {
        ++s_nextToken;
    }
    s_operation.token = s_nextToken;
    memcpy(s_operation.path, StagingPath, sizeof(StagingPath));
    *token = s_operation.token;
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

extern "C" esp_err_t ProductOta_PollInstall(uint32_t token, bool *done)
{
    if (done == NULL || token == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *done = true;
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    LifecycleLock lock;
    if (!s_operation.active || s_operation.token != token || s_operation.abandoned)
    {
        return PRODUCT_OTA_ERR_STATE;
    }
    *done = s_operation.done;
    if (s_operation.done)
    {
        // A second caller's rejected/busy call must not replace the first
        // operation's terminal error before its interop continuation resumes.
        portENTER_CRITICAL(&s_errorMux);
        memcpy(s_lastError, s_operation.error, sizeof(s_lastError));
        portEXIT_CRITICAL(&s_errorMux);
    }
    return s_operation.done ? s_operation.result : ESP_OK;
}

extern "C" void ProductOta_ReleaseInstall(uint32_t token)
{
    if (!s_initialized)
    {
        return;
    }
    LifecycleLock lock;
    if (s_operation.active && s_operation.token == token)
    {
        if (s_activationAttempted)
        {
            esp_restart();
        }
        if (s_operation.done)
        {
            memset(&s_operation, 0, sizeof(s_operation));
        }
        else
        {
            s_operation.cancel = true;
            s_operation.abandoned = true;
        }
    }
}

extern "C" void ProductOta_GetLastError(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0)
    {
        return;
    }
    const size_t count = size < sizeof(s_lastError) ? size : sizeof(s_lastError);
    portENTER_CRITICAL(&s_errorMux);
    memcpy(buffer, s_lastError, count);
    portEXIT_CRITICAL(&s_errorMux);
    buffer[count - 1] = 0;
}

extern "C" void ProductOta_PrepareSoftReboot(void)
{
    if (!s_initialized)
    {
        return;
    }
    const int64_t deadline = esp_timer_get_time() + QuiesceTimeoutUs;
    for (;;)
    {
        bool restart = false;
        bool complete = false;
        {
            LifecycleLock lock;
            esp_ota_img_states_t state;
            esp_err_t error = RunningState(state);
            restart = s_activationAttempted || error != ESP_OK || state == ESP_OTA_IMG_PENDING_VERIFY;
            if (!restart)
            {
                if (!s_operation.active || s_operation.done)
                {
                    memset(&s_operation, 0, sizeof(s_operation));
                    complete = true;
                }
                else
                {
                    s_operation.cancel = true;
                    s_operation.abandoned = true;
                }
            }
        }
        if (restart || esp_timer_get_time() >= deadline)
        {
            ESP_LOGW(Tag, "phase=soft-reboot hardware restart required");
            esp_restart();
        }
        if (complete)
        {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" esp_err_t ProductOta_IsTrialBoot(bool *isTrial)
{
    if (isTrial == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *isTrial = false;
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    LifecycleLock lock;
    if (s_activationAttempted || s_recovery)
    {
        return PRODUCT_OTA_ERR_STATE;
    }
    esp_ota_img_states_t state;
    esp_err_t error = RunningState(state);
    if (error == ESP_OK)
    {
        *isTrial = state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return error;
}

extern "C" esp_err_t ProductOta_ConfirmBoot(void)
{
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    LifecycleLock lock;
    if (s_operation.active || s_activationAttempted || s_recovery || !s_pairValidated)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "confirm-lifecycle", ESP_ERR_INVALID_STATE);
    }
    esp_ota_img_states_t state;
    esp_err_t error = RunningState(state);
    if (error != ESP_OK)
    {
        return error;
    }
    if (state == ESP_OTA_IMG_VALID)
    {
        return ESP_OK; // Documented idempotent confirmation of the running pair.
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "confirm-state", ESP_ERR_INVALID_STATE);
    }
    // A caller cannot win after the fixed deadline merely because the supervisor
    // task has not yet been scheduled.
    if (s_trialDeadline == 0 || esp_timer_get_time() >= s_trialDeadline)
    {
        return Failure(PRODUCT_OTA_ERR_TIMEOUT, "confirm-deadline", ESP_ERR_TIMEOUT);
    }
    error = esp_ota_mark_app_valid_cancel_rollback();
    if (error != ESP_OK)
    {
        return Failure(PRODUCT_OTA_ERR_STATE, "confirm-write", error);
    }
    s_trialDeadline = 0;
    ESP_LOGI(Tag, "phase=confirmed slot=%s", esp_ota_get_running_partition()->label);
    return ESP_OK;
}

extern "C" esp_err_t ProductOta_RejectBootAndRestart(void)
{
    if (!s_initialized)
    {
        return PRODUCT_OTA_ERR_NOT_READY;
    }
    esp_err_t error;
    {
        LifecycleLock lock;
        esp_ota_img_states_t state;
        error = RunningState(state);
        if (error != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY || s_operation.active ||
            s_activationAttempted || s_recovery)
        {
            return Failure(PRODUCT_OTA_ERR_STATE, "reject-lifecycle", error);
        }
        error = RejectPendingLocked();
    }
    // This API is called by a running managed application. Do not enter a
    // recovery loop on just its calling task and leave other tasks executing.
    esp_restart();
    __builtin_unreachable();
}
