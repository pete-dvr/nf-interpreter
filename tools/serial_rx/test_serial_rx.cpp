// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

static unsigned lockDepth;
static bool productionProducer;
static std::function<void()> interruptAtEmpty;
static std::function<void()> deferredInterrupt;
static void TestEmptyResetCheckpoint()
{
    if (!interruptAtEmpty)
        return;
    auto interrupt = interruptAtEmpty;
    interruptAtEmpty = nullptr;
    if (lockDepth)
        deferredInterrupt = interrupt;
    else
        interrupt();
}
static void TestPushCheckpoint()
{
    // The reader lock alone cannot exclude a producer on the other ESP32 core.
    if (productionProducer)
        assert(lockDepth > 0);
}
static void Unlock()
{
    assert(lockDepth > 0);
    if (--lockDepth == 0 && deferredInterrupt)
    {
        auto interrupt = deferredInterrupt;
        deferredInterrupt = nullptr;
        interrupt();
    }
}
#define GLOBAL_LOCK() ++lockDepth
#define GLOBAL_UNLOCK() Unlock()
static void *platform_malloc(size_t count);
static void platform_free(void *buffer);
#include "actual_ring.h"

using TickType_t = uint32_t;
using uart_port_t = int;
using TaskHandle_t = void *;
using SemaphoreHandle_t = void *;
enum { UART_DATA, UART_PATTERN_DET, UART_FIFO_OVF, UART_BUFFER_FULL, UART_EVENT_MAX };
struct uart_event_t { int type; };
struct Queue { std::deque<uart_event_t> events; };
using QueueHandle_t = Queue *;
struct NF_PAL_UART
{
    uart_port_t UartNum = 0;
    TaskHandle_t UartEventTask = nullptr;
    TaskHandle_t TxWorkerTask = nullptr;
    QueueHandle_t UartEventQueue = nullptr;
    SemaphoreHandle_t StartTx = nullptr;
    uint8_t *TxBuffer = nullptr;
    HAL_RingBuffer<uint8_t> RxRingBuffer;
    uint8_t *RxBuffer = nullptr;
    uint16_t RxBytesToRead = 0;
    uint8_t NewLineChar = '\n';
    uint32_t ReceivedBytesThreshold = 1;
};
static const int pdTRUE = 1;
static const unsigned portMAX_DELAY = ~0u;
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGE(...) ((void)0)
#define UART_NUM_TO_PORT_INDEX(value) ((value) + 1)
enum { SYSTEM_EVENT_FLAG_COM_IN, SYSTEM_EVENT_FLAG_COM_OUT, EVENT_SERIAL, SerialData_WatchChar, SerialData_Chars };
static std::vector<uint8_t> uartBytes;
static std::function<void()> onDelay;
static bool queueRejects;
static bool driverInstalled;
static TickType_t ticks;
static unsigned ringFrees;
static unsigned driverDeletes;
static unsigned txDeletes;
static uint8_t *trackedRing;
static NF_PAL_UART *trackedPal;

static int xQueueReceive(QueueHandle_t queue, uart_event_t *event, unsigned)
{
    assert(!queue->events.empty());
    *event = queue->events.front();
    queue->events.pop_front();
    return pdTRUE;
}
static int xQueueSendToFront(QueueHandle_t queue, const uart_event_t *event, unsigned)
{
    if (queueRejects)
        return 0;
    queue->events.push_front(*event);
    return pdTRUE;
}
static void xQueueReset(QueueHandle_t queue) { queue->events.clear(); }
static TickType_t xTaskGetTickCount() { return ticks; }
static void vTaskDelay(unsigned amount)
{
    assert(lockDepth == 0);
    ticks += amount;
    if (onDelay)
    {
        auto callback = onDelay;
        onDelay = nullptr;
        callback();
    }
}
static void vTaskDelete(TaskHandle_t task) { if (task) ++txDeletes; }
static void vSemaphoreDelete(SemaphoreHandle_t) {}
static void Events_Set(int) { assert(lockDepth == 0); }
static void PostManagedEvent(int, int, int, int) { assert(lockDepth == 0); }
static bool uart_is_driver_installed(int) { return driverInstalled; }
static void uart_driver_delete(int)
{
    assert(trackedPal->UartEventTask == nullptr);
    ++driverDeletes;
    driverInstalled = false;
}
static void uart_flush_input(int) { uartBytes.clear(); }
static int uart_pattern_get_pos(int) { return -1; }
static void uart_pattern_pop_pos(int) {}
static void uart_get_buffered_data_len(int, size_t *count) { *count = uartBytes.size(); }
static int uart_read_bytes(int, uint8_t *buffer, size_t count, unsigned)
{
    assert(count <= uartBytes.size());
    memcpy(buffer, uartBytes.data(), count);
    uartBytes.erase(uartBytes.begin(), uartBytes.begin() + count);
    return static_cast<int>(count);
}
static void *platform_malloc(size_t count) { return malloc(count); }
static void platform_free(void *buffer)
{
    if (buffer == trackedRing)
    {
        assert(trackedPal->UartEventTask == nullptr);
        ++ringFrees;
    }
    free(buffer);
}
#include "actual_serial.h"

static void Setup(NF_PAL_UART &pal, Queue &queue)
{
    lockDepth = 0;
    interruptAtEmpty = deferredInterrupt = onDelay = nullptr;
    queueRejects = false;
    driverInstalled = true;
    ticks = ringFrees = driverDeletes = txDeletes = 0;
    uartBytes.clear();
    queue.events.clear();
    pal = NF_PAL_UART();
    pal.RxBuffer = static_cast<uint8_t *>(malloc(4096));
    pal.RxRingBuffer.Initialize(pal.RxBuffer, 4096);
    pal.UartEventTask = reinterpret_cast<void *>(1);
    pal.UartEventQueue = &queue;
    pal.TxWorkerTask = reinterpret_cast<void *>(2);
    pal.StartTx = reinterpret_cast<void *>(3);
    trackedRing = pal.RxBuffer;
    trackedPal = &pal;
}

static void Produce(NF_PAL_UART &pal, Queue &queue, const std::vector<uint8_t> &bytes)
{
    assert(lockDepth == 0);
    uartBytes = bytes;
    queue.events.push_back({UART_DATA});
    queue.events.push_back({UART_EVENT_MAX});
    productionProducer = true;
    uart_event_task_sys(&pal);
    productionProducer = false;
}

static void BinaryResponseRace(bool locked, size_t split)
{
    NF_PAL_UART pal;
    Queue queue;
    Setup(pal, queue);
    const char header[] = "\r\n+CIPRXGET: 2,0,1500,0\r\n";
    std::vector<uint8_t> expected(header, header + sizeof(header) - 1);
    for (size_t i = 0; i < 1500; ++i)
        expected.push_back(static_cast<uint8_t>((i * 73 + 19) % 256));
    const size_t separator = expected.size();
    const char trailer[] = "\r\nOK\r\n";
    expected.insert(expected.end(), trailer, trailer + sizeof(trailer) - 1);
    pal.RxRingBuffer.Push(expected.data(), split);
    std::vector<uint8_t> tail(expected.begin() + split, expected.end());
    interruptAtEmpty = [&] { Produce(pal, queue, tail); };
    std::vector<uint8_t> observed(expected.size());
    if (locked)
        assert(ReadRxBuffer(&pal, observed.data(), split) == split);
    else
        assert(pal.RxRingBuffer.Pop(observed.data(), split) == split);
    assert(ReadRxBuffer(&pal, observed.data() + split, tail.size()) == tail.size());
    assert(GetRxBufferLength(&pal) == 0);
    if (locked)
        assert(observed == expected);
    else
    {
        assert(observed != expected);
        assert(observed[separator] != '\r');
    }
    assert(UninitializePalUart_sys(&pal));
}

static void CloseLifecycle()
{
    NF_PAL_UART pal;
    Queue queue;
    Setup(pal, queue);
    queue.events.push_back({UART_DATA});
    onDelay = [&] { uart_event_task_sys(&pal); };
    assert(UninitializePalUart_sys(&pal));
    assert(ringFrees == 1 && driverDeletes == 1 && txDeletes == 1);
    assert(pal.RxBuffer == nullptr && pal.UartEventQueue == nullptr);

    Setup(pal, queue);
    queueRejects = true;
    assert(!UninitializePalUart_sys(&pal));
    assert(ringFrees == 0 && driverDeletes == 0 && txDeletes == 0);
    assert(pal.RxBuffer == trackedRing && pal.UartEventTask != nullptr);
    queueRejects = false;
    onDelay = [&] { uart_event_task_sys(&pal); };
    assert(UninitializePalUart_sys(&pal));

    Setup(pal, queue);
    assert(!UninitializePalUart_sys(&pal));
    assert(ticks == 1000 && ringFrees == 0 && driverDeletes == 0 && txDeletes == 0);
    assert(pal.RxBuffer == trackedRing && pal.UartEventTask != nullptr);
    uart_event_task_sys(&pal); // Late acknowledgement; next close can safely clean up.
    assert(UninitializePalUart_sys(&pal));
    assert(ringFrees == 1 && driverDeletes == 1);
}

static void BoundedReadAndNoNewline()
{
    NF_PAL_UART pal;
    Queue queue;
    Setup(pal, queue);
    Produce(pal, queue, {1, 2, 3}); // ASan checks the backwards newline scan too.
    uint8_t output[8] = {};
    assert(ReadRxBuffer(&pal, output, sizeof(output)) == 3);
    assert(output[0] == 1 && output[1] == 2 && output[2] == 3);
    assert(ReadRxBuffer(&pal, output, sizeof(output)) == 0);
    assert(UninitializePalUart_sys(&pal));
}

static void WrappedReceive()
{
    NF_PAL_UART pal;
    Queue queue;
    Setup(pal, queue);
    std::vector<uint8_t> first(3000), second(2000), output(3000);
    for (size_t i = 0; i < first.size(); ++i)
        first[i] = static_cast<uint8_t>(i * 71);
    for (size_t i = 0; i < second.size(); ++i)
        second[i] = static_cast<uint8_t>(i * 29 + 3);
    Produce(pal, queue, first);
    assert(ReadRxBuffer(&pal, output.data(), 2000) == 2000);
    assert(std::equal(output.begin(), output.begin() + 2000, first.begin()));
    Produce(pal, queue, second);
    assert(GetRxBufferLength(&pal) == 3000);
    assert(ReadRxBuffer(&pal, output.data(), output.size()) == output.size());
    assert(std::equal(first.begin() + 2000, first.end(), output.begin()));
    assert(std::equal(second.begin(), second.end(), output.begin() + 1000));
    assert(UninitializePalUart_sys(&pal));
}

int main()
{
    BinaryResponseRace(false, 128); // Reproduces the old missing-CR corruption.
    for (size_t split = 1; split < 1500; ++split)
        BinaryResponseRace(true, split);
    CloseLifecycle();
    BoundedReadAndNoNewline();
    WrappedReceive();
    puts("PASS: old UART race reproduced; 1499 guarded binary boundaries and close/failure lifecycles verified");
}
