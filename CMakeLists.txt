cmake_minimum_required(VERSION 3.22)
include(ExternalProject)

# Generated once by CubeMX and owned by this project thereafter.

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Set the project name
set(CMAKE_PROJECT_NAME testHubFreeRTOS)

# Core project settings
project(${CMAKE_PROJECT_NAME})

# Include external project file
include("mx-generated.cmake")

