cmake_minimum_required(VERSION 3.20)
project(WavelengthFabric VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(WAVELENGTH_FABRIC_BUILD_TESTS "Build the test suite" ON)
option(WAVELENGTH_FABRIC_BUILD_EXAMPLES "Build examples" ON)
option(WAVELENGTH_FABRIC_BUILD_TOOLS "Build CLI tooling" ON)
option(WAVELENGTH_FABRIC_BUILD_BENCHMARKS "Build benchmarks" ON)
option(WAVELENGTH_FABRIC_BUILD_DISTRIBUTED "Build the reference TCP server and multiprocess proof" ON)
option(WAVELENGTH_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(WAVELENGTH_FABRIC_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WAVELENGTH_FABRIC_WARNINGS_AS_ERRORS)
  if(MSVC)
    set(WF_WARN_FLAGS /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  else()
    set(WF_WARN_FLAGS -Wall -Wextra -Werror -Wpedantic)
  endif()
else()
  if(MSVC)
    set(WF_WARN_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  else()
    set(WF_WARN_FLAGS -Wall -Wextra -Wpedantic)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library: vendor-neutral spectrum allocation, reservation and lifecycle.
# The core is transport-free and does not touch sockets or processes.
# ---------------------------------------------------------------------------
add_library(wavelength_fabric STATIC
  src/codec.cpp
  src/grid.cpp
  src/model.cpp
  src/text.cpp
  src/runtime_core.cpp
  src/runtime_capability.cpp
  src/runtime_candidates.cpp
  src/runtime_alloc.cpp
  src/runtime_lifecycle.cpp
  src/runtime_query.cpp
  src/runtime_persist.cpp
)
add_library(WavelengthFabric::WavelengthFabric ALIAS wavelength_fabric)

target_compile_features(wavelength_fabric PUBLIC cxx_std_20)
target_include_directories(wavelength_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(wavelength_fabric PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME WavelengthFabric
)
target_compile_options(wavelength_fabric PRIVATE ${WF_WARN_FLAGS})

# ---------------------------------------------------------------------------
# Transport library: bounded, checksummed framing over loopback TCP. It is a
# reference deployment mechanism layered on the core, not a core abstraction.
# ---------------------------------------------------------------------------
add_library(wavelength_fabric_transport STATIC
  src/net.cpp
  src/process.cpp
  src/protocol.cpp
)
add_library(WavelengthFabric::Transport ALIAS wavelength_fabric_transport)
target_compile_features(wavelength_fabric_transport PUBLIC cxx_std_20)
target_include_directories(wavelength_fabric_transport
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(wavelength_fabric_transport PUBLIC WavelengthFabric::WavelengthFabric)
set_target_properties(wavelength_fabric_transport PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME Transport
)
target_compile_options(wavelength_fabric_transport PRIVATE ${WF_WARN_FLAGS})
if(WIN32)
  target_link_libraries(wavelength_fabric_transport PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer
# ---------------------------------------------------------------------------
if(WAVELENGTH_FABRIC_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /O1 /Zi)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
install(TARGETS wavelength_fabric wavelength_fabric_transport
  EXPORT WavelengthFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/wavelength_fabric DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT WavelengthFabricTargets
  FILE WavelengthFabricTargets.cmake
  NAMESPACE WavelengthFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavelengthFabric
)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WavelengthFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/WavelengthFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavelengthFabric
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/WavelengthFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/WavelengthFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/WavelengthFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavelengthFabric
)
install(FILES LICENSE NOTICE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/WavelengthFabric
)

if(WAVELENGTH_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(WAVELENGTH_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(WAVELENGTH_FABRIC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(WAVELENGTH_FABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(WAVELENGTH_FABRIC_BUILD_DISTRIBUTED)
  add_subdirectory(distributed)
endif()
