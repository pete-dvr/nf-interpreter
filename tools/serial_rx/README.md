# ESP32 serial RX regression

Run `python3 tools/serial_rx/run.py` with a C++11 host compiler (or set `CXX`).
AddressSanitizer and UndefinedBehaviorSanitizer are enabled.

The harness compiles the actual `HAL_RingBuffer` template and ESP32 RX/close
functions extracted from the source tree. A deterministic scheduler checkpoint
at the ring's empty-buffer index reset reproduces a producer interrupting a
reader. The old unlocked call corrupts a 1500-byte modem response and moves the
expected CR; the guarded production reader preserves every byte at 1499 split
positions. This is a native concurrency regression, not a UART hardware test.

The actual receiver and close functions also verify acknowledgement before
freeing storage, queue failure and timeout preserving resources for retry,
bounded reads, and a received chunk without a newline.
