cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

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

# Location of the STM32Cube H7 firmware package that CubeMX references.
# The generated mx-generated.cmake refers to it through ${CUBE_FW_PATH}; the
# rewrite from CubeMX's absolute paths is done by after_code_generation.py.
# Override with -DCUBE_FW_PATH=<path> or the CUBE_FW_PATH environment variable.
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

# CubeMX запікає у mx-generated.cmake абсолютні шляхи на Cube FW тієї машини,
# де генерувався код. Щоб закомічений файл працював будь-де, підміняємо цей
# префікс на ${CUBE_FW_PATH} на етапі configure і підключаємо оброблену копію.
# Регулярка не прив'язана ні до користувача, ні до версії FW.
set(MX_GENERATED_IN  "${CMAKE_CURRENT_SOURCE_DIR}/mx-generated.cmake")
set(MX_GENERATED_OUT "${CMAKE_CURRENT_BINARY_DIR}/mx-generated.portable.cmake")
file(READ "${MX_GENERATED_IN}" MX_GENERATED_TEXT)
string(REGEX REPLACE
    "/[^ \t\r\n]*STM32Cube/Repository/STM32Cube_FW_H7_V[0-9.]+/"
    "${CUBE_FW_PATH}/"
    MX_GENERATED_TEXT "${MX_GENERATED_TEXT}")
file(WRITE "${MX_GENERATED_OUT}" "${MX_GENERATED_TEXT}")
# перегенерувати копію, щойно CubeMX перепише оригінал
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

# --- mbedTLS -------------------------------------------------------------
# Vendored as a submodule rather than taken from the Cube FW package, which
# ships 2.16.2 (2019) - long out of support. 3.6 is the LTS line whose legacy
# API both ST's hardware *_alt files and lwIP's altcp_tls glue target; 4.x moved
# all crypto into a separate TF-PSA-Crypto repository and dropped that API.
# See docs/decisions/0011-mbedtls-on-cm7-only.md.
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
    Core/Src/newlib_stubs.c
    Core/Src/networking.c
    ../Common/src/rng.c
    ../Common/src/ipc.c
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

# Deliberately not named mbedtls_config.h: build_info.h sits in the same directory
# as mbedTLS's own mbedtls_config.h, so a quoted include of that name resolves
# there first and our settings are silently ignored.
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    MBEDTLS_CONFIG_FILE="openhub_mbedtls_config.h")

# Third-party code is not held to this project's warning settings, and is built
# optimised even in Debug: mbedTLS's constant-time primitives use inline asm whose
# register constraints cannot be satisfied at -O0, and its timing guarantees are
# written against optimised codegen in the first place.
set_source_files_properties(${MBEDTLS_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2")
