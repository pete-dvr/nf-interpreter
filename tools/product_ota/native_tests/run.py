#!/usr/bin/env python3
"""Compile the real product OTA engine with host fault-injection boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[3]
tests = Path(__file__).resolve().parent
headers = (
    "esp_err.h", "esp_partition.h", "esp_flash_partitions.h", "esp_heap_caps.h",
    "esp_littlefs.h", "esp_log.h", "esp_ota_ops.h", "esp_rom_crc.h", "esp_system.h",
    "esp_task_wdt.h", "esp_timer.h", "freertos/FreeRTOS.h", "freertos/idf_additions.h",
    "freertos/semphr.h", "freertos/task.h", "mbedtls/ecdsa.h", "mbedtls/sha256.h",
)
with tempfile.TemporaryDirectory(prefix="product-ota-native-") as directory:
    temporary = Path(directory)
    # Generate only include shims, never production code or production inputs.
    for header in headers:
        destination = temporary / header
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text('#include "fake_sdk.h"\n', encoding="utf-8")
    compile_command = [
        os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-g", "-O1", "-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1",
        "-DCONFIG_NF_FEATURE_PRODUCT_OTA=1",
        f"-I{temporary}", f"-I{tests / 'fakes'}",
        f"-I{root / 'targets/ESP32/_include'}",
        f"-I{root / 'targets/ESP32/_nanoCLR/Product.Firmware'}",
        f"-I{root / 'src/CLR/Include'}",
        str(tests / "test_engine.cpp"), "-lcrypto",
    ]
    for require_signature in (0, 1):
        for allow_vs_deploy in (0, 1):
            executable = temporary / f"test_engine_signature_{require_signature}_vs_deploy_{allow_vs_deploy}"
            subprocess.run(compile_command + [
                f"-DCONFIG_NF_PRODUCT_OTA_REQUIRE_SIGNATURE={require_signature}",
                f"-DNF_PRODUCT_OTA_ALLOW_VS_DEPLOY={allow_vs_deploy}",
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
