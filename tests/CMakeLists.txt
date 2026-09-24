# Test executables are discovered from tests/test_*.cpp. Every test runs to
# natural completion: no CTest TIMEOUT property is set anywhere, and a test that
# hangs is treated as a defect in the runtime rather than something to kill.
file(GLOB WF_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

set(WF_TEST_WARN)
if(MSVC)
  set(WF_TEST_WARN /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
  set(WF_TEST_WARN -Wall -Wextra -Werror -Wpedantic)
endif()

enable_testing()

foreach(source IN LISTS WF_TEST_SOURCES)
  get_filename_component(name ${source} NAME_WE)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE WavelengthFabric::WavelengthFabric)
  target_compile_options(${name} PRIVATE ${WF_TEST_WARN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# Proofs that need real processes and the reference transport link the transport
# library as well. They are listed explicitly because they are proofs, not unit
# tests.
if(WAVELENGTH_FABRIC_BUILD_DISTRIBUTED)
  foreach(proof IN ITEMS test_transport)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${proof}.cpp)
      add_executable(${proof} ${proof}.cpp)
      target_link_libraries(${proof} PRIVATE WavelengthFabric::Transport)
      target_compile_options(${proof} PRIVATE ${WF_TEST_WARN})
      target_include_directories(${proof} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      add_test(NAME ${proof} COMMAND ${proof})
    endif()
  endforeach()
endif()
