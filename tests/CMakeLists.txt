# Congestion Observatory - test suites.
#
# Every suite is registered as its own CTest test so that a failure names the suite that failed.
# No test relies on a timeout: the framework has none, and the runtime's transport deadlines are
# product level network deadlines configured generously.

set(CONGESTION_TEST_SUPPORT test_framework.cpp test_support.cpp)

add_library(co_test_support STATIC ${CONGESTION_TEST_SUPPORT})
target_include_directories(co_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(co_test_support PUBLIC CongestionObservatory::congestion_observatory)
target_link_libraries(co_test_support PUBLIC co_support)
target_link_libraries(co_test_support PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)
target_compile_definitions(co_test_support PUBLIC
  CO_CLI_PATH="$<TARGET_FILE:congestion-observatory>")

set(CONGESTION_SUITE_FILES
  suite_core.cpp
  suite_model.cpp
  suite_evidence.cpp
  suite_assess.cpp
  suite_episodes.cpp
  suite_persistence.cpp
  suite_runtime.cpp
  suite_restart.cpp
  suite_transport.cpp
  suite_property.cpp
  suite_adversarial.cpp
  suite_concurrency.cpp
  suite_hardening.cpp
  suite_endtoend.cpp)

add_executable(co_tests ${CONGESTION_SUITE_FILES})
target_link_libraries(co_tests PRIVATE co_test_support)
target_link_libraries(co_tests PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)

# The suite names as registered inside the binary (independent of the file names).
set(CONGESTION_SUITE_NAMES
  core
  model
  evidence
  assess
  localize_episode
  persistence
  runtime
  restart
  transport
  property
  adversarial
  concurrency
  hardening
  endtoend)

foreach(suite IN LISTS CONGESTION_SUITE_NAMES)
  add_test(NAME suite.${suite} COMMAND co_tests --suite ${suite})
endforeach()

# The package proof: install into a scratch prefix, then configure, build and run an
# independent project that resolves the package with find_package(CongestionObservatory CONFIG).
add_test(NAME package.downstream
  COMMAND ${CMAKE_COMMAND}
    -DCO_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DCO_PROJECT_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DCO_BUILD_DIR=${CMAKE_BINARY_DIR}/downstream-build
    -DCO_INSTALL_DIR=${CMAKE_BINARY_DIR}/downstream-install
    -DCO_GENERATOR=${CMAKE_GENERATOR}
    -DCO_CONFIG=$<CONFIG>
    -P ${CMAKE_SOURCE_DIR}/cmake/DownstreamVerify.cmake)

# A downstream consumer that links the exported package interface the same way an external
# project would. The find_package proof itself lives in cmake/DownstreamVerify.cmake.
add_executable(co_downstream_check downstream/consumer.cpp)
target_link_libraries(co_downstream_check PRIVATE CongestionObservatory::congestion_observatory)
target_link_libraries(co_downstream_check PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)
