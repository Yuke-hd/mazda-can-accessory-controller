# Shared factory JSON generation for the host and WeAct firmware builds.
set(_CONTROLLER_CONFIG_FACTORY_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(controller_config_factory_generate)
  cmake_parse_arguments(PARSE_ARGV 0 _factory ""
    "YAML;OUTPUT_JSON;OUTPUT_CATALOG;SIGNAL_CATALOG;PROFILE_JSON;ARTIFACT_TARGET" "")
  if(_factory_UNPARSED_ARGUMENTS OR NOT _factory_YAML OR NOT _factory_OUTPUT_JSON
     OR NOT _factory_OUTPUT_CATALOG OR NOT _factory_SIGNAL_CATALOG)
    message(FATAL_ERROR
      "controller_config_factory_generate requires YAML, OUTPUT_JSON, OUTPUT_CATALOG and SIGNAL_CATALOG")
  endif()

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(_compiler "${_CONTROLLER_CONFIG_FACTORY_TOOLS_DIR}/compile_controller_config.py")
  set(_generator "${_CONTROLLER_CONFIG_FACTORY_TOOLS_DIR}/generate_factory_config.py")
  set(_builtin_yaml "${_CONTROLLER_CONFIG_FACTORY_TOOLS_DIR}/../config/default.yaml")
  get_filename_component(_input_yaml "${_factory_YAML}" REALPATH)
  get_filename_component(_builtin_yaml "${_builtin_yaml}" REALPATH)
  set(_command "${Python3_EXECUTABLE}")
  set(_inputs "${_factory_YAML}" "${_factory_OUTPUT_CATALOG}"
      "${_factory_SIGNAL_CATALOG}" "${_compiler}")

  if(_input_yaml STREQUAL _builtin_yaml)
    list(APPEND _command "${_generator}" "${_factory_YAML}" "${_factory_OUTPUT_JSON}"
         --output-catalog "${_factory_OUTPUT_CATALOG}"
         --signal-catalog "${_factory_SIGNAL_CATALOG}")
    list(APPEND _inputs "${_generator}")
    if(_factory_PROFILE_JSON)
      list(APPEND _command --profile-json "${_factory_PROFILE_JSON}")
      list(APPEND _inputs "${_factory_PROFILE_JSON}")
    else()
      list(APPEND _command --native)
    endif()
  else()
    list(APPEND _command "${_compiler}" "${_factory_YAML}" "${_factory_OUTPUT_JSON}"
         --output-catalog "${_factory_OUTPUT_CATALOG}"
         --signal-catalog "${_factory_SIGNAL_CATALOG}")
  endif()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  execute_process(COMMAND ${_command}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "Factory controller configuration compilation failed:\n${_output}${_error}")
  endif()
  if(_output)
    string(STRIP "${_output}" _output)
    message(STATUS "${_output}")
  endif()

  add_custom_command(
    OUTPUT "${_factory_OUTPUT_JSON}"
    COMMAND ${_command}
    DEPENDS ${_inputs}
    COMMENT "Compiling factory controller configuration"
    VERBATIM)
  add_custom_target(controller_config_factory_json
    DEPENDS "${_factory_OUTPUT_JSON}")
  if(TARGET output_catalog)
    add_dependencies(controller_config_factory_json output_catalog)
  endif()
  if(_factory_ARTIFACT_TARGET)
    if(NOT TARGET "${_factory_ARTIFACT_TARGET}")
      message(FATAL_ERROR
        "selected profile artifact target does not exist: ${_factory_ARTIFACT_TARGET}")
    endif()
    add_dependencies(controller_config_factory_json "${_factory_ARTIFACT_TARGET}")
  endif()
  set(CONTROLLER_CONFIG_FACTORY_JSON "${_factory_OUTPUT_JSON}" PARENT_SCOPE)
endfunction()
