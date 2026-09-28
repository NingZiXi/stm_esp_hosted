cmake_minimum_required(VERSION 3.22)
project(stm_esp_hosted_tests C)

enable_testing()

# Allow the same tests to run inside the H723 demo and in a standalone clone.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../stm_common/stm_err.h")
    set(STM_ESP_HOSTED_TEST_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/../../stm_common")
else()
    include(FetchContent)
    FetchContent_Declare(stm_common
        GIT_REPOSITORY https://github.com/NingZiXi/stm_common.git
        GIT_TAG v1.0.0
    )
    FetchContent_GetProperties(stm_common)
    if(NOT stm_common_POPULATED)
        FetchContent_Populate(stm_common)
    endif()
    set(STM_ESP_HOSTED_TEST_COMMON "${stm_common_SOURCE_DIR}")
endif()

add_executable(stm_esp_hosted_tests
    ../stm_esp_hosted.c
    ../stm_esp_hosted_control.c
    hal_stub.c
    stm_esp_hosted_test.c
)
target_compile_features(stm_esp_hosted_tests PRIVATE c_std_11)
target_compile_definitions(stm_esp_hosted_tests PRIVATE
    STM_ESP_HOSTED_HAL_HEADER="hal_stub.h"
)
target_include_directories(stm_esp_hosted_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ..
    ${STM_ESP_HOSTED_TEST_COMMON}
)
add_test(NAME stm_esp_hosted_tests COMMAND stm_esp_hosted_tests)

add_executable(stm_esp_hosted_lwip_tests
    ../stm_esp_hosted.c ../stm_esp_hosted_control.c ../stm_esp_hosted_lwip.c
    hal_stub.c mock_lwip/mock_lwip.c lwip_adapter_test.c)
target_compile_features(stm_esp_hosted_lwip_tests PRIVATE c_std_11)
target_compile_definitions(stm_esp_hosted_lwip_tests PRIVATE STM_ESP_HOSTED_HAL_HEADER="hal_stub.h")
target_include_directories(stm_esp_hosted_lwip_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_lwip ${CMAKE_CURRENT_SOURCE_DIR} .. ${STM_ESP_HOSTED_TEST_COMMON})
add_test(NAME stm_esp_hosted_lwip_tests COMMAND stm_esp_hosted_lwip_tests)
