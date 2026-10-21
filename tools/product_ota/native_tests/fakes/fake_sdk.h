#ifndef PRODUCT_OTA_FAKE_SDK_H
#define PRODUCT_OTA_FAKE_SDK_H

// Narrow host substitutes for the SDK boundary. The installer itself is the
// production translation unit; none of its state transitions are copied here.
#include <stddef.h>
#include <stdint.h>
#include <openssl/evp.h>

typedef int esp_err_t;
static const esp_err_t ESP_OK = 0;
static const esp_err_t ESP_FAIL = -1;
static const esp_err_t ESP_ERR_NO_MEM = 0x101;
static const esp_err_t ESP_ERR_INVALID_ARG = 0x102;
static const esp_err_t ESP_ERR_INVALID_STATE = 0x103;
static const esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
static const esp_err_t ESP_ERR_NOT_FOUND = 0x105;
static const esp_err_t ESP_ERR_TIMEOUT = 0x107;
static const esp_err_t ESP_ERR_INVALID_CRC = 0x109;
typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
static const int ESP_PARTITION_TYPE_APP = 0;
static const int ESP_PARTITION_TYPE_DATA = 1;
static const int ESP_PARTITION_SUBTYPE_ANY = 0xff;
static const int ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10;
static const int ESP_PARTITION_SUBTYPE_DATA_OTA = 0;
static const int ESP_PARTITION_SUBTYPE_DATA_PHY = 1;
static const int ESP_PARTITION_SUBTYPE_DATA_NVS = 2;
struct esp_partition_t
{
    int type;
    int subtype;
    uint32_t address;
    uint32_t size;
    char label[17];
    bool encrypted;
    uint32_t erase_size;
};
struct FakeIterator;
typedef FakeIterator *esp_partition_iterator_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);

enum esp_ota_img_states_t
{
    ESP_OTA_IMG_NEW = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
    ESP_OTA_IMG_UNDEFINED = -1
};
typedef uint32_t esp_ota_handle_t;
struct esp_ota_select_entry_t
{
    uint32_t ota_seq;
    uint8_t seq_label[20];
    uint32_t ota_state;
    uint32_t crc;
};
int esp_ota_get_app_partition_count();
const esp_partition_t *esp_ota_get_running_partition();
const esp_partition_t *esp_ota_get_boot_partition();
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_invalidate_inactive_ota_data_slot();
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
bool esp_ota_check_rollback_is_possible();
uint32_t esp_rom_crc32_le(uint32_t, const uint8_t *, size_t);
[[noreturn]] void esp_restart();
int esp_reset_reason();
esp_err_t esp_littlefs_info(const char *, size_t *, size_t *);
int64_t esp_timer_get_time();

static const int MALLOC_CAP_INTERNAL = 1;
static const int MALLOC_CAP_8BIT = 2;
void *heap_caps_malloc(size_t, int);
void heap_caps_free(void *);
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static const uint32_t portMAX_DELAY = 0xffffffff;
static const int portNUM_PROCESSORS = 2;
static const int pdPASS = 1;
static const int pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(uint32_t);
int xTaskCreateWithCaps(void (*)(void *), const char *, size_t, void *, int, TaskHandle_t *, int);
TaskHandle_t xTaskGetCurrentTaskHandle();
uint32_t ulTaskNotifyTake(int, uint32_t);
void xTaskNotifyGive(TaskHandle_t);
struct esp_task_wdt_config_t
{
    uint32_t timeout_ms;
    uint32_t idle_core_mask;
    bool trigger_panic;
};
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *);
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *);
esp_err_t esp_task_wdt_add(TaskHandle_t);
esp_err_t esp_task_wdt_reset();
void fake_log(const char *, const char *, ...);
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)

// OpenSSL supplies real SHA256 for file/flash integrity checks. Signature
// primitives fail closed: production signature correctness has separate tests;
// this harness compiles an explicitly unsigned development policy.
struct mbedtls_sha256_context { EVP_MD_CTX *context; };
inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { c->context = EVP_MD_CTX_new(); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int)
{
    return c->context && EVP_DigestInit_ex(c->context, EVP_sha256(), NULL) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const uint8_t *p, size_t n)
{
    return EVP_DigestUpdate(c->context, p, n) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, uint8_t *out)
{
    return EVP_DigestFinal_ex(c->context, out, NULL) == 1 ? 0 : -1;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { EVP_MD_CTX_free(c->context); }
struct mbedtls_ecp_group {};
struct mbedtls_ecp_point {};
struct mbedtls_mpi {};
static const int MBEDTLS_ECP_DP_SECP256R1 = 3;
inline void mbedtls_ecp_group_init(mbedtls_ecp_group *) {}
inline void mbedtls_ecp_point_init(mbedtls_ecp_point *) {}
inline void mbedtls_mpi_init(mbedtls_mpi *) {}
inline void mbedtls_ecp_group_free(mbedtls_ecp_group *) {}
inline void mbedtls_ecp_point_free(mbedtls_ecp_point *) {}
inline void mbedtls_mpi_free(mbedtls_mpi *) {}
inline int mbedtls_ecp_group_load(mbedtls_ecp_group *, int) { return -1; }
inline int mbedtls_ecp_point_read_binary(mbedtls_ecp_group *, mbedtls_ecp_point *, const uint8_t *, size_t) { return -1; }
inline int mbedtls_ecp_check_pubkey(mbedtls_ecp_group *, mbedtls_ecp_point *) { return -1; }
inline int mbedtls_mpi_read_binary(mbedtls_mpi *, const uint8_t *, size_t) { return -1; }
inline int mbedtls_ecdsa_verify(mbedtls_ecp_group *, const uint8_t *, size_t, mbedtls_ecp_point *, mbedtls_mpi *, mbedtls_mpi *)
{
    return -1;
}
#endif
