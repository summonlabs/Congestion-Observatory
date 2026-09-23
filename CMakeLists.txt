cmake_minimum_required(VERSION 3.25)

project(CongestionObservatory
  VERSION 1.0.0
  LANGUAGES CXX
  DESCRIPTION "Vendor-neutral congestion evidence aggregation and causal localization runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Congestion-Observatory")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CONGESTION_IS_TOP_LEVEL ON)
else()
  set(CONGESTION_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CONGESTION_BUILD_TOOLS "Build the congestion-observatory command line tool" ${CONGESTION_IS_TOP_LEVEL})
option(CONGESTION_BUILD_TESTS "Build the test suites" ${CONGESTION_IS_TOP_LEVEL})
option(CONGESTION_BUILD_EXAMPLES "Build the examples" ${CONGESTION_IS_TOP_LEVEL})
option(CONGESTION_BUILD_BENCHMARKS "Build the benchmarks" ${CONGESTION_IS_TOP_LEVEL})
option(CONGESTION_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(CONGESTION_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration. First-party warning count must be zero in every
# configuration; the standard is enforced rather than aspirational.
# ---------------------------------------------------------------------------
add_library(congestion_warnings INTERFACE)
if(MSVC)
  target_compile_options(congestion_warnings INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP
    /wd4267)
  if(CONGESTION_WARNINGS_AS_ERRORS)
    target_compile_options(congestion_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(congestion_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
    -Wimplicit-fallthrough)
  if(CONGESTION_WARNINGS_AS_ERRORS)
    target_compile_options(congestion_warnings INTERFACE -Werror)
  endif()
endif()

add_library(congestion_sanitizers INTERFACE)
if(CONGESTION_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(congestion_sanitizers INTERFACE /fsanitize=address /Zi)
    target_link_options(congestion_sanitizers INTERFACE /INCREMENTAL:NO)
  else()
    target_compile_options(congestion_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(congestion_sanitizers INTERFACE -fsanitize=address)
  endif()
  target_compile_definitions(congestion_sanitizers INTERFACE CONGESTION_ASAN_ENABLED=1)
endif()

# ---------------------------------------------------------------------------
# The runtime library.
# ---------------------------------------------------------------------------
set(CONGESTION_PUBLIC_HEADERS
  include/congestion/congestion.hpp
  include/congestion/version.hpp
  include/congestion/core/cancel.hpp
  include/congestion/core/checked.hpp
  include/congestion/core/crc64.hpp
  include/congestion/core/hash.hpp
  include/congestion/core/json.hpp
  include/congestion/core/limits.hpp
  include/congestion/core/name.hpp
  include/congestion/core/result.hpp
  include/congestion/core/strong_id.hpp
  include/congestion/core/time.hpp
  include/congestion/model/generation.hpp
  include/congestion/model/identities.hpp
  include/congestion/model/topology.hpp
  include/congestion/evidence/evidence.hpp
  include/congestion/evidence/store.hpp
  include/congestion/assessment/policy.hpp
  include/congestion/assessment/classify.hpp
  include/congestion/localization/graph.hpp
  include/congestion/localization/localize.hpp
  include/congestion/episode/episode.hpp
  include/congestion/correlation/group.hpp
  include/congestion/persistence/snapshot.hpp
  include/congestion/persistence/store.hpp
  include/congestion/runtime/runtime.hpp
  include/congestion/transport/frame.hpp
  include/congestion/transport/socket.hpp
  include/congestion/transport/server.hpp
  include/congestion/transport/client.hpp)

set(CONGESTION_SOURCES
  src/version.cpp
  src/core/result.cpp
  src/core/name.cpp
  src/core/time.cpp
  src/core/hash.cpp
  src/core/crc64.cpp
  src/core/limits.cpp
  src/core/json.cpp
  src/core/cancel.cpp
  src/model/identities.cpp
  src/model/generation.cpp
  src/model/topology.cpp
  src/evidence/evidence.cpp
  src/evidence/store.cpp
  src/assessment/policy.cpp
  src/assessment/classify.cpp
  src/localization/graph.cpp
  src/localization/localize.cpp
  src/episode/episode.cpp
  src/correlation/group.cpp
  src/persistence/snapshot.cpp
  src/persistence/store.cpp
  src/runtime/runtime.cpp
  src/transport/frame.cpp
  src/transport/socket.cpp
  src/transport/server.cpp
  src/transport/client.cpp)

add_library(congestion_observatory STATIC ${CONGESTION_SOURCES} ${CONGESTION_PUBLIC_HEADERS})
add_library(CongestionObservatory::congestion_observatory ALIAS congestion_observatory)

target_include_directories(congestion_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(congestion_observatory PUBLIC Threads::Threads)
# Warning and sanitizer settings are build-time only: they must not leak into the exported
# package interface.
target_link_libraries(congestion_observatory PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)

# Winsock is required by the transport on Windows.
if(WIN32)
  target_link_libraries(congestion_observatory PUBLIC ws2_32)
endif()

set_target_properties(congestion_observatory PROPERTIES
  OUTPUT_NAME congestion_observatory
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Tooling, examples, benchmarks, tests.
# ---------------------------------------------------------------------------
if(CONGESTION_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(CONGESTION_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(CONGESTION_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(CONGESTION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and package export.
# ---------------------------------------------------------------------------
install(TARGETS congestion_observatory
  EXPORT CongestionObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CongestionObservatoryTargets
  FILE CongestionObservatoryTargets.cmake
  NAMESPACE CongestionObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionObservatory)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CongestionObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionObservatory)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionObservatory)

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/CongestionObservatory)

message(STATUS "Congestion Observatory ${PROJECT_VERSION}: tests=${CONGESTION_BUILD_TESTS} tools=${CONGESTION_BUILD_TOOLS} examples=${CONGESTION_BUILD_EXAMPLES} benchmarks=${CONGESTION_BUILD_BENCHMARKS} asan=${CONGESTION_ENABLE_ASAN}")
