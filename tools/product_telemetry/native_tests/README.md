# Native device telemetry tests

Run inside the existing ESP32 development container:

```sh
python3 tools/product_telemetry/native_tests/run.py --sanitize
```

The harness compiles the production `ProductDeviceTelemetry.cpp` with ESP-IDF
sensor calls replaced by fakes and FreeRTOS mutexes replaced by host mutexes.
It covers initialization, SDK failures, cleanup failure and recovery, output
preservation on errors, repeated reads without repeated allocations, negative
and elevated temperatures through 125 C, non-finite and out-of-range rejection,
concurrent initialization, and bounded contention. It does not simulate sensor
accuracy or prove the physical temperature on a board.

The installed ESP-IDF 5.5.5 `temperature_sensor_get_celsius()` delegates to
`temp_sensor_get_raw_value()` in `esp_hw_support/sar_periph_ctrl_common.c`.
That implementation switches between all five ESP32-S3 measurement ranges
automatically and validates -40 through 125 C after calibration. Installation
still requires an initial range supported by one analog setting; requesting
-40 through 125 directly fails. The helper starts at -10 through 80 C and lets
the SDK switch to the hotter or colder ranges as needed. The test's accepted
high readings ensure the wrapper does not incorrectly limit readings to that
initial range.

The helper owns a single enabled SDK sensor handle for the hardware boot's
lifetime. Native state survives CLR soft restarts, so deployment/restart does
not allocate another handle or leave an orphaned sensor. Reads are serialized
with a static mutex and a 50 ms lock timeout. A failed initial enable attempts
uninstallation; if that cleanup fails, ownership is retained for recovery.

The same command compiles the production `ProductPowerStatus.cpp` against NVS
and reset-cause fakes. Its 27 checks cover normal/brownout/software boots,
early persistence, pending events across hardware restarts, non-consuming
reads, durable acknowledgement, no relatching across CLR restarts, subsequent
brownouts, storage failures, retries after failed commits, corrupt data, and
lock timeouts. Failed reads never produce a false OK status.

The default NVS fake models the installed IDF 5.5.5 write-through behavior:
`NVSHandleSimple::set_typed_item()` calls `Storage::writeItem()` immediately,
and `nvs_commit()` is currently a no-op for a valid handle. A write returning
`ESP_ERR_NVS_REMOVE_FAILED` can have persisted its replacement already; NVS
finishes cleanup on reinitialization. Tests cover this partial success for
both setting and clearing the latch, retrying in the same boot, and rebooting.
After an acknowledgement error the in-memory latch stays pending, but the
new cleared flash value may already survive reboot. Acknowledgement is valid
only after the report was successfully transmitted; that caller-side ordering
is covered by the managed client tests, not by the native API in isolation.

Two explicitly named deferred-model scenarios also exercise a possible future
NVS implementation that caches changes until commit. Their failed-commit
durability expectations apply to that model only. Injected commit failures in
write-through mode check conservative in-memory error handling; the installed
IDF does not produce these failures for an otherwise valid owned handle. The
host tests do not replace brownout and connection-loss testing on the board.
