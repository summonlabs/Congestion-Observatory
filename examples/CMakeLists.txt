# Congestion Observatory - worked examples.

set(CONGESTION_EXAMPLES
  minimal_pipeline
  localize_episode
  persist_and_recover)

foreach(example IN LISTS CONGESTION_EXAMPLES)
  add_executable(example_${example} ${example}.cpp)
  target_link_libraries(example_${example} PRIVATE CongestionObservatory::congestion_observatory)
  target_link_libraries(example_${example} PRIVATE
    $<BUILD_INTERFACE:congestion_warnings>
    $<BUILD_INTERFACE:congestion_sanitizers>)
endforeach()
