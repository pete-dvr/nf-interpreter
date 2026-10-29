#pragma once
#include <freertos/FreeRTOS.h>

struct StaticSemaphore_t
{
    std::timed_mutex mutex;
};
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
