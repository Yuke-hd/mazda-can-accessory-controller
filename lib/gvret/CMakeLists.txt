if(COMMAND idf_component_register)
  message(FATAL_ERROR "gvret_parser is host-only and cannot be built as an ESP-IDF component")
endif()

add_library(gvret_parser STATIC src/csv_parser.cpp src/replay.cpp)
target_include_directories(gvret_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvret_parser PUBLIC vehicle_core)
target_compile_features(gvret_parser PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_parser PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_parser PRIVATE -Wall -Wextra -Wpedantic)
endif()
