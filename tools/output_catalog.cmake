# Output catalog generation shared by the host build and the firmware project
# (docs/specs/companion/output-catalog.md, Build integration).
#
#   output_catalog_generate(BOARD_FILE <board.cmake> OUTPUT_DIR <dir>)
#
# The board file sets OUTPUT_CATALOG_DESCRIPTORS and OUTPUT_CATALOG_BOARD. The
# configure-time run makes a malformed descriptor fail configuration; the build
# rule regenerates when a descriptor, the board file or the generator changes.
# Sets OUTPUT_CATALOG_JSON and OUTPUT_CATALOG_INCLUDE_DIR in the caller's scope
# and defines the output_catalog target, so it may be called once per project.

set(_output_catalog_generator "${CMAKE_CURRENT_LIST_DIR}/generate_output_catalog.py")

function(output_catalog_generate)
  cmake_parse_arguments(PARSE_ARGV 0 _arg "" "BOARD_FILE;OUTPUT_DIR" "")
  if(NOT _arg_BOARD_FILE OR NOT _arg_OUTPUT_DIR)
    message(FATAL_ERROR "output_catalog_generate requires BOARD_FILE and OUTPUT_DIR")
  endif()
  if(TARGET output_catalog)
    message(FATAL_ERROR "output_catalog_generate may be called once per project")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  include("${_arg_BOARD_FILE}")
  set(_catalog "${_arg_OUTPUT_DIR}/output_catalog.json")
  set(_include_dir "${_arg_OUTPUT_DIR}/generated")
  set(_header "${_include_dir}/controller_config/generated/output_limits.hpp")
  set(_command "${Python3_EXECUTABLE}" "${_output_catalog_generator}")
  foreach(_descriptor IN LISTS OUTPUT_CATALOG_DESCRIPTORS)
    list(APPEND _command --descriptor "${_descriptor}")
  endforeach()
  foreach(_value IN LISTS OUTPUT_CATALOG_BOARD)
    list(APPEND _command --board "${_value}")
  endforeach()
  list(APPEND _command --catalog "${_catalog}" --limits-header "${_header}")
  set(_inputs ${OUTPUT_CATALOG_DESCRIPTORS} "${_arg_BOARD_FILE}" "${_output_catalog_generator}")

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Output catalog generation failed:\n${_output}${_error}")
  endif()

  add_custom_command(
    OUTPUT "${_catalog}" "${_header}"
    COMMAND ${_command}
    DEPENDS ${_inputs}
    COMMENT "Generating output catalog and limits header"
    VERBATIM)
  add_custom_target(output_catalog ALL DEPENDS "${_catalog}" "${_header}")

  set(OUTPUT_CATALOG_JSON "${_catalog}" PARENT_SCOPE)
  set(OUTPUT_CATALOG_INCLUDE_DIR "${_include_dir}" PARENT_SCOPE)
endfunction()
