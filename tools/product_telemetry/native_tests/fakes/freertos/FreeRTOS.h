#pragma once
#include <mutex>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef std::mutex portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0
