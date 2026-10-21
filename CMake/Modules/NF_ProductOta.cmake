#
# Product OTA release inputs deliberately have no default image, identity or key.
# A managed-only release must also rebuild its native companion.
#
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NF_PRODUCT_MANAGED_IMAGE "" CACHE FILEPATH "Final managed application.bin")
set(NF_PRODUCT_RELEASE_ID "" CACHE STRING "Product release identifier (1-16 ASCII bytes)")
set(NF_PRODUCT_PUBLIC_KEY "" CACHE FILEPATH "Pinned P-256 public key PEM")
option(NF_PRODUCT_OTA_ALLOW_VS_DEPLOY
    "Development only: allow Visual Studio to replace the managed application" OFF)

if(NF_PRODUCT_OTA_ALLOW_VS_DEPLOY)
    if(NOT NF_FEATURE_DEBUGGER OR NF_BUILD_RTM)
        message(FATAL_ERROR
            "NF_PRODUCT_OTA_ALLOW_VS_DEPLOY requires the managed debugger and a non-RTM build.")
    endif()
    message(WARNING
        "DEVELOPMENT BUILD: Visual Studio deployment is enabled. The running managed "
        "application is not bound to the native release hash. Do not deploy to production.")
endif()
# This module is loaded from targets/ESP32, but nanoCLR.elf and its OTA source
# are compiled in the root directory. Scope the definition to that source in
# the root build directory so it reaches the actual engine translation unit.
set_property(SOURCE "${CMAKE_SOURCE_DIR}/targets/ESP32/_common/ProductFirmwareUpdate.cpp"
    DIRECTORY "${CMAKE_BINARY_DIR}"
    APPEND PROPERTY COMPILE_DEFINITIONS
        NF_PRODUCT_OTA_ALLOW_VS_DEPLOY=$<BOOL:${NF_PRODUCT_OTA_ALLOW_VS_DEPLOY}>)

if(NOT EXISTS "${NF_PRODUCT_MANAGED_IMAGE}" OR NF_PRODUCT_RELEASE_ID STREQUAL "")
    message(FATAL_ERROR
        "PRODUCT_S3_OTA requires NF_PRODUCT_MANAGED_IMAGE and NF_PRODUCT_RELEASE_ID. "
        "Build SeadragonS3 and its Product.Firmware interop library first.")
endif()
if(NF_PRODUCT_OTA_REQUIRE_SIGNATURE AND NOT EXISTS "${NF_PRODUCT_PUBLIC_KEY}")
    message(FATAL_ERROR "Production OTA requires NF_PRODUCT_PUBLIC_KEY (P-256 public PEM).")
endif()
if(NOT NF_INTEROP_ASSEMBLIES MATCHES "(^|[ ;])Product\\.Firmware($|[ ;])")
    message(FATAL_ERROR "PRODUCT_S3_OTA must include Product.Firmware in NF_INTEROP_ASSEMBLIES.")
endif()

set(_product_ota_tool "${CMAKE_SOURCE_DIR}/tools/product_ota/product_ota.py")
set(_product_ota_csv "${CMAKE_SOURCE_DIR}/targets/ESP32/_IDF/esp32s3/partitions_product_ota_16mb.csv")
set(NF_PRODUCT_RELEASE_SOURCE "${CMAKE_BINARY_DIR}/ProductReleaseDescriptor.cpp")
set(_product_descriptor_args
    descriptor --managed "${NF_PRODUCT_MANAGED_IMAGE}"
    --release-id "${NF_PRODUCT_RELEASE_ID}" --output "${NF_PRODUCT_RELEASE_SOURCE}")
if(EXISTS "${NF_PRODUCT_PUBLIC_KEY}")
    list(APPEND _product_descriptor_args --public-key "${NF_PRODUCT_PUBLIC_KEY}")
endif()
if(NOT NF_PRODUCT_OTA_REQUIRE_SIGNATURE)
    message(WARNING "DEVELOPMENT OTA BUILD: unsigned packages are allowed. Do not deploy to production.")
    list(APPEND _product_descriptor_args --allow-unsigned-development)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_product_ota_tool}" ${_product_descriptor_args}
    RESULT_VARIABLE _product_descriptor_result)
if(NOT _product_descriptor_result EQUAL 0)
    message(FATAL_ERROR "Product release descriptor generation failed.")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_product_ota_tool}"
    check-layout --csv "${_product_ota_csv}" RESULT_VARIABLE _product_layout_result)
if(NOT _product_layout_result EQUAL 0)
    message(FATAL_ERROR "Product OTA partition layout validation failed.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${NF_PRODUCT_MANAGED_IMAGE}" "${_product_ota_tool}" "${_product_ota_csv}")
if(EXISTS "${NF_PRODUCT_PUBLIC_KEY}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NF_PRODUCT_PUBLIC_KEY}")
endif()
if(NF_PRODUCT_OTA_REQUIRE_SIGNATURE)
    add_compile_definitions(NF_PRODUCT_OTA_REQUIRE_SIGNATURE=1)
else()
    add_compile_definitions(NF_PRODUCT_OTA_REQUIRE_SIGNATURE=0)
endif()
