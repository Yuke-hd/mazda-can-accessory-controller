cmake_minimum_required(VERSION 3.20)

project(vehicle_can_rx_binding_adapter_tests LANGUAGES CXX)

set(VEHICLE_CAN_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/esp32-vehicle-can-core"
    CACHE PATH "Pinned vehicle CAN core checkout")

add_executable(vehicle_can_rx_binding_adapter_tests
  ../src/driver_binding.cpp
  binding_adapter_tests.cpp
)
target_include_directories(vehicle_can_rx_binding_adapter_tests PRIVATE
  ${VEHICLE_CAN_CORE_SOURCE_DIR}/components/can_bus/tests/fake_idf
  ${VEHICLE_CAN_CORE_SOURCE_DIR}/components/can_bus/include
  ${CMAKE_CURRENT_LIST_DIR}/../../board/include
  ${CMAKE_CURRENT_LIST_DIR}/../include
)
target_compile_features(vehicle_can_rx_binding_adapter_tests PRIVATE cxx_std_17)

enable_testing()
add_test(NAME vehicle_can_rx_binding_adapter_tests
         COMMAND vehicle_can_rx_binding_adapter_tests)
