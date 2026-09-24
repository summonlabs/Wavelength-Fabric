# One executable per benchmark. Benchmarks link only the core library: they
# measure in-process work over abstract spectrum data and never open a socket.
file(GLOB WF_BENCHMARK_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp)

foreach(source IN LISTS WF_BENCHMARK_SOURCES)
  get_filename_component(name ${source} NAME_WE)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE WavelengthFabric::WavelengthFabric)
  target_compile_options(${name} PRIVATE ${WF_WARN_FLAGS})
  # Collect the benchmarks next to the libraries so that a build tree can be
  # exercised from a single directory.
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
