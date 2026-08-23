cmake_minimum_required(VERSION 3.22)

# Generated once by CubeMX and owned by this project thereafter.

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT custom_m7_flash.ld)
set(STM32_LINKER_OPTION  )

# Include toolchain file
include("../gcc-arm-none-eabi.cmake")

# Set the project name
set(CMAKE_PROJECT_NAME testHubFreeRTOS_CM7)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# The build id is refreshed every build; the header changes only when git does.
# radio_devices_docs/open_hub/arch/build-and-generation.md
set(BUILD_ID_H ${CMAKE_CURRENT_BINARY_DIR}/build_id.h)
execute_process(COMMAND ${CMAKE_COMMAND}
    -DOUT=${BUILD_ID_H} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
    -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/build_id.cmake)
add_custom_target(build_id ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${BUILD_ID_H} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/build_id.cmake
    BYPRODUCTS ${BUILD_ID_H}
    COMMENT "build id")
add_dependencies(${CMAKE_PROJECT_NAME} build_id)

# The STM32Cube H7 firmware package CubeMX references.
# radio_devices_docs/open_hub/arch/build-and-generation.md
if(NOT DEFINED CUBE_FW_PATH)
    if(DEFINED ENV{CUBE_FW_PATH})
        set(CUBE_FW_PATH "$ENV{CUBE_FW_PATH}")
    else()
        set(CUBE_FW_PATH "$ENV{HOME}/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0")
    endif()
endif()
if(NOT EXISTS "${CUBE_FW_PATH}/Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal.h")
    message(FATAL_ERROR
        "STM32Cube H7 firmware package not found at:\n  ${CUBE_FW_PATH}\n"
        "Install it via CubeMX, or point CUBE_FW_PATH at your copy.")
endif()

# Rewrite CubeMX's baked-in absolute Cube FW paths so a committed file builds anywhere.
set(MX_GENERATED_IN  "${CMAKE_CURRENT_SOURCE_DIR}/mx-generated.cmake")
set(MX_GENERATED_OUT "${CMAKE_CURRENT_BINARY_DIR}/mx-generated.portable.cmake")
file(READ "${MX_GENERATED_IN}" MX_GENERATED_TEXT)
string(REGEX REPLACE
    "/[^ \t\r\n]*STM32Cube/Repository/STM32Cube_FW_H7_V[0-9.]+/"
    "${CUBE_FW_PATH}/"
    MX_GENERATED_TEXT "${MX_GENERATED_TEXT}")
file(WRITE "${MX_GENERATED_OUT}" "${MX_GENERATED_TEXT}")
# Redo the copy as soon as CubeMX rewrites the original.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MX_GENERATED_IN}")

# Include mx-generated
include("${MX_GENERATED_OUT}")

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true") 

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE 
    # Add user defined symbols
    DATA_IN_D2_SRAM
)

# --- mbedTLS ---
# Submodule, 3.6 LTS. ADR-0011.
set(MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mbedtls)
if(NOT EXISTS ${MBEDTLS_DIR}/library/ssl_tls.c)
    message(FATAL_ERROR
        "mbedTLS submodule missing at ${MBEDTLS_DIR}\n"
        "Run: git submodule update --init third_party/mbedtls")
endif()
file(GLOB MBEDTLS_SOURCES ${MBEDTLS_DIR}/library/*.c)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_BINARY_DIR}
    ../Common/inc
    crypto
    ../Common/test/vectors
    ${MBEDTLS_DIR}/include
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/cli.c
    Core/Src/hubipc.c
    Core/Src/pairing.c
    Core/Src/keystore.c
    Core/Src/erasetest.c
    Core/Src/newlib_stubs.c
    Core/Src/networking.c
    Core/Src/telemetry.c
    ../Common/src/rng.c
    ../Common/src/ipc.c
    ../Common/src/oht_codec.c
    crypto/entropy_hw.c
    crypto/crypto.c
    crypto/mbedtls_alloc.c
    ${MBEDTLS_SOURCES}
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

# Validate that STM32CubeMX code is compatible with C standard
if(CMAKE_C_STANDARD LESS 11)
    message(ERROR "Generated code requires C11 or higher")
endif()

# Deliberately not named mbedtls_config.h, which would resolve to mbedTLS's own.
# radio_devices_docs/open_hub/security/crypto-architecture.md

# Opt-in: it programs and erases sector 3 at boot.
# radio_devices_docs/open_hub/arch/config-store.md
set(HUB_ERASE_TEST 0 CACHE STRING "run the bank 1 erase measurement at boot")

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    HUB_ERASE_TEST=${HUB_ERASE_TEST}
    MBEDTLS_CONFIG_FILE="openhub_mbedtls_config.h")

# Third-party code keeps its own warnings and is optimised even in Debug.
# radio_devices_docs/open_hub/security/crypto-architecture.md
set_source_files_properties(${MBEDTLS_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2")

# The -T lives in CMAKE_C_LINK_FLAGS, so ninja never knew it was an input.
# radio_devices_docs/open_hub/arch/build-and-generation.md
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    LINK_DEPENDS "${CMAKE_SOURCE_DIR}/${STM32_LINKER_SCRIPT}")
