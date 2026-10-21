# Create a provisioning image with the filename limit accepted by the runtime.
# The SDK's image helper passes CONFIG_LITTLEFS_OBJ_NAME_LEN straight to
# littlefs-python, although this Kconfig value includes the NUL terminator.
function(nf_product_ota_create_config_image base_directory)
    idf_component_get_property(_littlefs_directory littlefs COMPONENT_DIR)
    set(_littlefs_header "${_littlefs_directory}/src/littlefs/lfs.h")
    file(STRINGS "${_littlefs_header}" _littlefs_name_definition
        REGEX "^#define[ \t]+LFS_NAME_MAX[ \t]+[0-9]+[ \t]*$")
    list(LENGTH _littlefs_name_definition _littlefs_name_definition_count)
    if(NOT _littlefs_name_definition_count EQUAL 1)
        message(FATAL_ERROR "Cannot determine the runtime LittleFS filename limit from ${_littlefs_header}.")
    endif()
    string(REGEX REPLACE ".*LFS_NAME_MAX[ \t]+([0-9]+).*" "\\1"
        _littlefs_runtime_name_max "${_littlefs_name_definition}")

    math(EXPR _littlefs_image_name_max "${CONFIG_LITTLEFS_OBJ_NAME_LEN} - 1")
    if(_littlefs_image_name_max GREATER _littlefs_runtime_name_max)
        set(_littlefs_image_name_max "${_littlefs_runtime_name_max}")
    endif()
    if(_littlefs_image_name_max LESS 1)
        message(FATAL_ERROR "The LittleFS filename buffer must include room for a name and its NUL terminator.")
    endif()

    # Function scope changes only the image helper's argument. The runtime's
    # sdkconfig, filename buffers and compiled library remain unchanged.
    set(CONFIG_LITTLEFS_OBJ_NAME_LEN "${_littlefs_image_name_max}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_littlefs_header}")
    # Deliberately exclude FLASH_IN_PROJECT: routine flashing must preserve config.
    littlefs_create_partition_image(config "${base_directory}")
endfunction()
