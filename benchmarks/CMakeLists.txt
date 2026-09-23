# Congestion Observatory - benchmarks. Every benchmark reports completed work.

add_executable(bench_observatory bench_observatory.cpp)
target_link_libraries(bench_observatory PRIVATE
  CongestionObservatory::congestion_observatory
  co_support)
target_link_libraries(bench_observatory PRIVATE
  $<BUILD_INTERFACE:congestion_warnings>
  $<BUILD_INTERFACE:congestion_sanitizers>)
