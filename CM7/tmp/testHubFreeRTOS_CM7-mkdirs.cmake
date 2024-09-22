# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/build"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/tmp"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/src/testHubFreeRTOS_CM7-stamp"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/src"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/src/testHubFreeRTOS_CM7-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/src/testHubFreeRTOS_CM7-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM7/src/testHubFreeRTOS_CM7-stamp${cfgdir}") # cfgdir has leading slash
endif()
