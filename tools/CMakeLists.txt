# Congestion Observatory - command line tooling.

add_library(co_support STATIC co_support.cpp co_support.hpp)
target_include_directories(co_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(co_support PUBLIC CongestionObservatory::congestion_observatory)
target_link_libraries(co_support PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)

add_executable(congestion-observatory co_main.cpp)
target_link_libraries(congestion-observatory PRIVATE co_support)
target_link_libraries(congestion-observatory PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)

install(TARGETS congestion-observatory RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
