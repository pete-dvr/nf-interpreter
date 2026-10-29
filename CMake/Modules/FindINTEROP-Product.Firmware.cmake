#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

if(NOT NF_FEATURE_PRODUCT_OTA OR NOT TARGET_SERIES STREQUAL "ESP32_S3")
    message(FATAL_ERROR "Product.Firmware requires the PRODUCT_S3_OTA target.")
endif()

set(Product.Firmware_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/targets/ESP32/_nanoCLR/Product.Firmware"
    "${CMAKE_SOURCE_DIR}/targets/ESP32/_include")
set(Product.Firmware_SOURCES
    "${CMAKE_SOURCE_DIR}/targets/ESP32/_nanoCLR/Product.Firmware/Product_Firmware.cpp"
    "${CMAKE_SOURCE_DIR}/targets/ESP32/_nanoCLR/Product.Firmware/Product_Firmware_DeviceTelemetry.cpp"
    "${CMAKE_SOURCE_DIR}/targets/ESP32/_nanoCLR/Product.Firmware/Product_Firmware_Update.cpp")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(INTEROP-Product.Firmware DEFAULT_MSG
    Product.Firmware_INCLUDE_DIRS Product.Firmware_SOURCES)
