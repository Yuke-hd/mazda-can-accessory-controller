# Offline opt-in artifact generation; usable by host and ESP-IDF configure.
set(_VEHICLE_PROFILE_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(vehicle_profile_generate)
  cmake_parse_arguments(PROFILE "" "MANIFEST;POLICIES;GENERATOR_REVISION;OUTPUT_DIR"
                        "SOURCE_FILES;GENERATOR_INPUTS" ${ARGN})
  if(PROFILE_UNPARSED_ARGUMENTS OR NOT PROFILE_MANIFEST OR NOT PROFILE_POLICIES
     OR NOT PROFILE_GENERATOR_REVISION)
    message(FATAL_ERROR "vehicle_profile_generate requires MANIFEST, POLICIES and GENERATOR_REVISION")
  endif()
  get_property(_already GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED)
  if(_already)
    message(FATAL_ERROR "vehicle_profile_generate may be called once per project")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  get_filename_component(_manifest "${PROFILE_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(_policies "${PROFILE_POLICIES}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT PROFILE_OUTPUT_DIR)
    set(PROFILE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/vehicle-profile")
  endif()
  get_filename_component(_output "${PROFILE_OUTPUT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  file(RELATIVE_PATH _relative "${CMAKE_BINARY_DIR}" "${_output}")
  if(IS_ABSOLUTE "${_relative}" OR _relative MATCHES "^[.][.]/")
    message(FATAL_ERROR "vehicle profile output must be inside the build directory")
  endif()
  # Path.resolve follows existing parent symlinks even when the final output
  # directory does not yet exist; CMake REAL_PATH requires the whole path.
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "from pathlib import Path; import sys; p=Path(sys.argv[1]).resolve(); b=Path(sys.argv[2]).resolve(); print(p); sys.exit(0 if p.is_relative_to(b) else 1)"
    "${_output}" "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE _inside OUTPUT_VARIABLE _resolved_output
    ERROR_VARIABLE _path_error OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _inside EQUAL 0)
    message(FATAL_ERROR "vehicle profile output must resolve inside the build directory: ${_path_error}")
  endif()
  set(_output "${_resolved_output}")
  set(_command "${Python3_EXECUTABLE}" "${_VEHICLE_PROFILE_TOOLS_DIR}/generate_vehicle_profile.py"
      --manifest "${_manifest}" --policies "${_policies}"
      --generator-revision "${PROFILE_GENERATOR_REVISION}" --output-dir "${_output}")
  set(_inputs "${_manifest}" "${_policies}")
  foreach(_kind SOURCE_FILES GENERATOR_INPUTS)
    foreach(_assignment IN LISTS PROFILE_${_kind})
      if(NOT _assignment MATCHES "^([^=]+)=(.+)$")
        message(FATAL_ERROR "profile inputs must use NAME=PATH")
      endif()
      set(_name "${CMAKE_MATCH_1}")
      get_filename_component(_path "${CMAKE_MATCH_2}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      if(_kind STREQUAL "SOURCE_FILES")
        list(APPEND _command --source "${_name}=${_path}")
      else()
        list(APPEND _command --generator-input "${_name}=${_path}")
      endif()
      list(APPEND _inputs "${_path}")
    endforeach()
  endforeach()
  file(GLOB _modules CONFIGURE_DEPENDS "${_VEHICLE_PROFILE_TOOLS_DIR}/vehicle_profiles/*.py")
  list(APPEND _inputs "${_VEHICLE_PROFILE_TOOLS_DIR}/generate_vehicle_profile.py" ${_modules})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  execute_process(COMMAND ${_command} --print-directory
    RESULT_VARIABLE _result OUTPUT_VARIABLE _directory ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "vehicle profile generation failed: ${_error}")
  endif()
  # A failing build rule may delete its declared outputs. Keep immutable sets
  # and the old index outside that mechanism; unchanged publication is a no-op.
  add_custom_target(vehicle_profile_artifacts COMMAND ${_command}
                    DEPENDS ${_inputs} VERBATIM)
  set_property(GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED TRUE)
  set(VEHICLE_PROFILE_ARTIFACT_DIR "${_directory}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_COMPILER_CATALOG "${_directory}/controller_signal_catalog.json" PARENT_SCOPE)
  set(VEHICLE_PROFILE_RUNTIME_HEADER "${_directory}/vehicle_profile_runtime.hpp" PARENT_SCOPE)
  set(VEHICLE_PROFILE_INCLUDE_DIR "${_directory}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_CAPABILITY_REPORT "${_directory}/capability_report.json" PARENT_SCOPE)
endfunction()
