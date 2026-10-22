#!/usr/bin/env python3
"""Run ESP32 serial RX concurrency/teardown regressions against real native code."""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def definition(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


hal = (ROOT / "src/HAL/Include/nanoHAL.h").read_text(encoding="utf-8")
ring = definition(hal, "template <typename T> class HAL_RingBuffer") + ";\n"
# The deterministic scheduler interrupts at the exact unsafe empty-buffer reset.
# The production algorithm is otherwise unchanged. With the real reader lock,
# the producer is deferred until the critical section exits.
checkpoint = "// reset the read/write index"
assert checkpoint in ring
ring = ring.replace(checkpoint, "TestEmptyResetCheckpoint();\n            " + checkpoint)
push_entry = "size_t Push(const T *data, size_t length)\n    {"
assert push_entry in ring
ring = ring.replace(push_entry, push_entry + "\n        TestPushCheckpoint();")
serial = (ROOT / "targets/ESP32/_nanoCLR/System.IO.Ports/"
          "sys_io_ser_native_System_IO_Ports_SerialPort.cpp").read_text(encoding="utf-8")
functions = "\n\n".join(definition(serial, signature) for signature in (
    "static size_t GetRxBufferLength(",
    "static size_t ReadRxBuffer(",
    "bool UninitializePalUart_sys(",
    "void uart_event_task_sys(",
))
with tempfile.TemporaryDirectory(prefix="serial-rx-native-") as directory:
    temporary = Path(directory)
    (temporary / "actual_ring.h").write_text(ring, encoding="utf-8")
    (temporary / "actual_serial.h").write_text(functions, encoding="utf-8")
    executable = temporary / "serial_rx_tests"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{temporary}", str(TESTS / "test_serial_rx.cpp"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
