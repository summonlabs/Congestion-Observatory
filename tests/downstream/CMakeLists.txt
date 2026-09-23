cmake_minimum_required(VERSION 3.25)

# Independent downstream consumer project. It knows nothing about this repository's build tree:
# the package is resolved purely through find_package from an install prefix.
project(CongestionObservatoryDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CongestionObservatory 1.0 REQUIRED CONFIG)

add_executable(downstream_consumer consumer.cpp)
target_link_libraries(downstream_consumer PRIVATE CongestionObservatory::congestion_observatory)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()
