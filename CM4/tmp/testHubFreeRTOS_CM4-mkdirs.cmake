# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/build"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/tmp"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/src/testHubFreeRTOS_CM4-stamp"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/src"
  "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/src/testHubFreeRTOS_CM4-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/src/testHubFreeRTOS_CM4-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/aleks/OneDrive/Documents/repos/testHubFreeRTOS/CM4/src/testHubFreeRTOS_CM4-stamp${cfgdir}") # cfgdir has leading slash
endif()
