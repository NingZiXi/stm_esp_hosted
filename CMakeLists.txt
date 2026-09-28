cmake_minimum_required(VERSION 3.22)
project(stm_esp_hosted C)

option(STM_ESP_HOSTED_LINK_CUBEMX "Link stm32cubemx target automatically" ON)
add_library(stm_esp_hosted STATIC stm_esp_hosted.c stm_esp_hosted_control.c)
target_compile_features(stm_esp_hosted PUBLIC c_std_11)
target_include_directories(stm_esp_hosted PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(STM_ESP_HOSTED_LINK_CUBEMX AND TARGET stm32cubemx)
    target_link_libraries(stm_esp_hosted PUBLIC stm32cubemx)
endif()

if(NOT TARGET stm_common)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../stm_common/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../stm_common" "${CMAKE_CURRENT_BINARY_DIR}/stm_common")
    else()
        include(FetchContent)
        FetchContent_Declare(stm_common
            GIT_REPOSITORY https://github.com/NingZiXi/stm_common.git
            GIT_TAG v1.0.0
        )
        FetchContent_MakeAvailable(stm_common)
    endif()
endif()
if(NOT TARGET stm_common)
    message(FATAL_ERROR "stm_common dependency did not provide the stm_common target")
endif()
target_link_libraries(stm_esp_hosted PUBLIC stm_common)

# The lwIP Ethernet netif remains optional for applications using only SPI/RPC.
if(TARGET lwip)
    add_library(stm_esp_hosted_lwip STATIC stm_esp_hosted_lwip.c)
    target_compile_features(stm_esp_hosted_lwip PUBLIC c_std_11)
    target_include_directories(stm_esp_hosted_lwip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(stm_esp_hosted_lwip PUBLIC stm_esp_hosted lwip)
endif()
