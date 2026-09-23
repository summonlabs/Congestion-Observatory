# Verifies that an installed Congestion Observatory package can be consumed by an independent
# CMake project through find_package(CongestionObservatory CONFIG).
#
# Required variables:
#   CO_SOURCE_DIR   repository root
#   CO_BUILD_DIR    scratch directory for the downstream build
#   CO_INSTALL_DIR  install prefix produced by "cmake --install"

if(NOT DEFINED CO_SOURCE_DIR OR NOT DEFINED CO_BUILD_DIR OR NOT DEFINED CO_INSTALL_DIR)
  message(FATAL_ERROR "DownstreamVerify.cmake requires CO_SOURCE_DIR, CO_BUILD_DIR and CO_INSTALL_DIR")
endif()

file(REMOVE_RECURSE "${CO_BUILD_DIR}")
file(MAKE_DIRECTORY "${CO_BUILD_DIR}")

# Install the library into a scratch prefix exactly as a distribution would.
file(REMOVE_RECURSE "${CO_INSTALL_DIR}")
set(install_command "${CMAKE_COMMAND}" --install "${CO_PROJECT_BUILD_DIR}" --prefix "${CO_INSTALL_DIR}")
if(DEFINED CO_CONFIG AND NOT CO_CONFIG STREQUAL "")
  list(APPEND install_command --config "${CO_CONFIG}")
endif()
execute_process(COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install step failed:\n${install_output}\n${install_error}")
endif()

set(configure_command "${CMAKE_COMMAND}"
    -S "${CO_SOURCE_DIR}/tests/downstream"
    -B "${CO_BUILD_DIR}"
    -DCMAKE_PREFIX_PATH=${CO_INSTALL_DIR}
    -DCMAKE_BUILD_TYPE=Release)
if(DEFINED CO_GENERATOR AND NOT CO_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${CO_GENERATOR}")
endif()
# The downstream project is configured with the ambient toolchain, exactly as an external user
# would configure it. CO_CXX_COMPILER is honoured when a caller explicitly supplies one, but it is
# deliberately not forwarded from this build: naming a compiler by absolute path inside a scratch
# build changes how the resource compiler is located and is not what a consumer does.
if(DEFINED CO_CXX_COMPILER AND NOT CO_CXX_COMPILER STREQUAL "")
  list(APPEND configure_command -DCMAKE_CXX_COMPILER=${CO_CXX_COMPILER})
endif()

execute_process(COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${CO_BUILD_DIR}" --config Release
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed:\n${build_output}\n${build_error}")
endif()

find_program(CO_DOWNSTREAM_EXECUTABLE
  NAMES downstream_consumer
  PATHS "${CO_BUILD_DIR}/Release" "${CO_BUILD_DIR}" "${CO_BUILD_DIR}/bin"
  NO_DEFAULT_PATH)
if(NOT CO_DOWNSTREAM_EXECUTABLE)
  message(FATAL_ERROR "downstream executable was not produced in ${CO_BUILD_DIR}")
endif()

execute_process(COMMAND "${CO_DOWNSTREAM_EXECUTABLE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream run failed (exit ${run_result}):\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream consumer output: ${run_output}")
