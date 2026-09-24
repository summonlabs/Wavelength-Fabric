# One executable per example. Examples demonstrate the core library only and
# never touch the transport, so they link WavelengthFabric::WavelengthFabric.
file(GLOB WF_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(source IN LISTS WF_EXAMPLE_SOURCES)
  get_filename_component(name ${source} NAME_WE)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE WavelengthFabric::WavelengthFabric)
  target_compile_options(${name} PRIVATE ${WF_WARN_FLAGS})
  # Collect the examples next to the libraries so that a build tree can be
  # exercised from a single directory.
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
