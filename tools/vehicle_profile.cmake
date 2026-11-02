# Shared host/ESP-IDF selection and offline profile artifact generation.
set(_VEHICLE_PROFILE_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(_VEHICLE_PROFILE_REPOSITORY_ROOT "${_VEHICLE_PROFILE_TOOLS_DIR}/.." ABSOLUTE)
set(VEHICLE_PROFILE "native:mazda" CACHE STRING
    "Vehicle profile: native:mazda or a mapping.json path")
set_property(CACHE VEHICLE_PROFILE PROPERTY STRINGS native:mazda)

function(_vehicle_profile_resolve_output input_dir output_var)
  get_filename_component(_output "${input_dir}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
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
  set(${output_var} "${_resolved_output}" PARENT_SCOPE)
endfunction()

function(vehicle_profile_select)
  get_property(_already GLOBAL PROPERTY VEHICLE_PROFILE_SELECTION_DONE)
  if(_already)
    message(FATAL_ERROR "vehicle_profile_select may be called once per project")
  endif()
  set_property(GLOBAL PROPERTY VEHICLE_PROFILE_SELECTION_DONE TRUE)

  if("${VEHICLE_PROFILE}" STREQUAL "")
    message(FATAL_ERROR "VEHICLE_PROFILE must be native:mazda or a mapping.json path")
  endif()

  if(VEHICLE_PROFILE STREQUAL "native:mazda")
    set(VEHICLE_PROFILE_IS_NATIVE TRUE PARENT_SCOPE)
    set(VEHICLE_PROFILE_MAPPING_PATH "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_ARTIFACT_DIR "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_COMPILER_CATALOG
        "${_VEHICLE_PROFILE_REPOSITORY_ROOT}/tools/controller_signal_catalog.json" PARENT_SCOPE)
    set(VEHICLE_PROFILE_METADATA_HEADER "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_RUNTIME_HEADER "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_INCLUDE_DIR "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_CAPABILITY_REPORT "" PARENT_SCOPE)
    set(VEHICLE_PROFILE_ARTIFACT_TARGET "" PARENT_SCOPE)
    return()
  endif()

  get_filename_component(_mapping "${VEHICLE_PROFILE}" ABSOLUTE
                         BASE_DIR "${_VEHICLE_PROFILE_REPOSITORY_ROOT}")
  if(NOT EXISTS "${_mapping}" OR IS_DIRECTORY "${_mapping}")
    message(FATAL_ERROR "selected vehicle profile mapping does not exist: ${_mapping}")
  endif()
  get_property(_already_generated GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED)
  if(_already_generated)
    message(FATAL_ERROR "vehicle profile artifacts may be generated once per project")
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_output "${CMAKE_BINARY_DIR}/vehicle-profile")
  _vehicle_profile_resolve_output("${_output}" _output)
  set(_command "${Python3_EXECUTABLE}" "${_VEHICLE_PROFILE_TOOLS_DIR}/import_vehicle_profile.py"
      --mapping "${_mapping}" --output-dir "${_output}")
  execute_process(COMMAND ${_command} --print-directory
    RESULT_VARIABLE _result OUTPUT_VARIABLE _directory ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "vehicle profile import failed: ${_error}")
  endif()

  execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "from pathlib import Path; import json,sys; p=Path(sys.argv[1]); print(';'.join(str((p.parent / item['path']).resolve()).replace(';', r'\\;') for item in json.loads(p.read_text(encoding='utf-8'))['sources']))"
    "${_mapping}" RESULT_VARIABLE _sources_result OUTPUT_VARIABLE _source_paths
    ERROR_VARIABLE _sources_error OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _sources_result EQUAL 0)
    message(FATAL_ERROR "could not list vehicle profile sources: ${_sources_error}")
  endif()
  set(_inputs "${_mapping}" "${_VEHICLE_PROFILE_TOOLS_DIR}/import_vehicle_profile.py"
      "${_VEHICLE_PROFILE_TOOLS_DIR}/controller_signal_catalog.json")
  list(APPEND _inputs ${_source_paths})
  file(GLOB _modules CONFIGURE_DEPENDS "${_VEHICLE_PROFILE_TOOLS_DIR}/vehicle_profiles/*.py")
  list(APPEND _inputs ${_modules})
  get_property(_configure_inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
  foreach(_input IN LISTS _inputs)
    get_filename_component(_resolved_input "${_input}" REALPATH)
    set(_already_configure_input FALSE)
    foreach(_configured_input IN LISTS _configure_inputs)
      get_filename_component(_resolved_configured_input "${_configured_input}" REALPATH)
      if("${_resolved_input}" STREQUAL "${_resolved_configured_input}")
        set(_already_configure_input TRUE)
        break()
      endif()
    endforeach()
    if(NOT _already_configure_input)
      list(APPEND _configure_inputs "${_input}")
    endif()
  endforeach()
  set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS ${_configure_inputs})
  add_custom_target(vehicle_profile_artifacts COMMAND ${_command}
                    DEPENDS ${_inputs} VERBATIM)
  set_property(GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED TRUE)

  set(VEHICLE_PROFILE_IS_NATIVE FALSE PARENT_SCOPE)
  set(VEHICLE_PROFILE_MAPPING_PATH "${_mapping}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_ARTIFACT_DIR "${_directory}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_COMPILER_CATALOG "${_directory}/controller_signal_catalog.json" PARENT_SCOPE)
  set(VEHICLE_PROFILE_METADATA_HEADER "${_directory}/vehicle_profile_metadata.hpp" PARENT_SCOPE)
  set(VEHICLE_PROFILE_RUNTIME_HEADER "${_directory}/vehicle_profile_runtime.hpp" PARENT_SCOPE)
  set(VEHICLE_PROFILE_INCLUDE_DIR "${_directory}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_CAPABILITY_REPORT "${_directory}/capability_report.json" PARENT_SCOPE)
  set(VEHICLE_PROFILE_ARTIFACT_TARGET vehicle_profile_artifacts PARENT_SCOPE)
endfunction()

function(vehicle_profile_select_max_resource_fixture)
  get_property(_already GLOBAL PROPERTY VEHICLE_PROFILE_SELECTION_DONE)
  if(_already)
    message(FATAL_ERROR "vehicle profile selection may be called once per project")
  endif()
  if(NOT VEHICLE_PROFILE STREQUAL "native:mazda")
    message(FATAL_ERROR
      "maximum profile resource fixture requires VEHICLE_PROFILE=native:mazda")
  endif()
  set_property(GLOBAL PROPERTY VEHICLE_PROFILE_SELECTION_DONE TRUE)
  get_property(_already_generated GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED)
  if(_already_generated)
    message(FATAL_ERROR "vehicle profile artifacts may be generated once per project")
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_output "${CMAKE_BINARY_DIR}/vehicle_profile_resource_fixture/max")
  _vehicle_profile_resolve_output("${_output}" _output)
  set(_generator "${_VEHICLE_PROFILE_TOOLS_DIR}/generate_profile_runtime_link_probe.py")
  set(_fixture "${_VEHICLE_PROFILE_REPOSITORY_ROOT}/tests/fixtures/vehicle-profiles/runtime-limits.json")
  set(_command "${Python3_EXECUTABLE}" "${_generator}" --selection max
               --application-artifacts --output-dir "${_output}")
  execute_process(COMMAND ${_command}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "Maximum profile resource fixture generation failed:\n${_stdout}${_stderr}")
  endif()

  file(GLOB _modules CONFIGURE_DEPENDS "${_VEHICLE_PROFILE_TOOLS_DIR}/vehicle_profiles/*.py")
  set(_inputs "${_generator}" "${_fixture}" ${_modules})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  add_custom_target(vehicle_profile_artifacts COMMAND ${_command} DEPENDS ${_inputs} VERBATIM)
  set_property(GLOBAL PROPERTY VEHICLE_PROFILE_GENERATED TRUE)
  message(STATUS
    "Selected profile resource fixture: maximum generated profile (not a vehicle profile selection)")

  set(VEHICLE_PROFILE_IS_NATIVE FALSE PARENT_SCOPE)
  set(VEHICLE_PROFILE_MAPPING_PATH "" PARENT_SCOPE)
  set(VEHICLE_PROFILE_ARTIFACT_DIR "${_output}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_COMPILER_CATALOG "${_output}/controller_signal_catalog.json" PARENT_SCOPE)
  set(VEHICLE_PROFILE_METADATA_HEADER "${_output}/vehicle_profile_metadata.hpp" PARENT_SCOPE)
  set(VEHICLE_PROFILE_RUNTIME_HEADER "${_output}/vehicle_profile_runtime.hpp" PARENT_SCOPE)
  set(VEHICLE_PROFILE_INCLUDE_DIR "${_output}" PARENT_SCOPE)
  set(VEHICLE_PROFILE_CAPABILITY_REPORT "${_output}/capability_report.json" PARENT_SCOPE)
  set(VEHICLE_PROFILE_ARTIFACT_TARGET vehicle_profile_artifacts PARENT_SCOPE)
endfunction()

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
  _vehicle_profile_resolve_output("${PROFILE_OUTPUT_DIR}" _output)
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
