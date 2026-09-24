# Standalone consumer project for an installed WavelengthFabric package.
#
# This directory is deliberately not added by the repository root
# CMakeLists.txt. It is an independent project that is configured on its own
# against an installed prefix, for example:
#
#   cmake -S consumer -B <consumer-build> -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <consumer-build> --config Release
#   ctest --test-dir <consumer-build> -C Release --output-on-failure
#
cmake_minimum_required(VERSION 3.20)

project(WavelengthFabricConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Config-mode lookup of the installed package. The imported targets carry the
# installed include directory and the transitive link requirements, so no
# repository path appears anywhere in this project.
find_package(WavelengthFabric 1.0 REQUIRED CONFIG)

add_executable(wavelength_fabric_consumer main.cpp)

target_compile_features(wavelength_fabric_consumer PRIVATE cxx_std_20)

target_link_libraries(wavelength_fabric_consumer
  PRIVATE
    WavelengthFabric::WavelengthFabric
    WavelengthFabric::Transport
)

if(MSVC)
  target_compile_options(wavelength_fabric_consumer PRIVATE
    /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
  target_compile_options(wavelength_fabric_consumer PRIVATE
    -Wall -Wextra -Werror -Wpedantic)
endif()

add_test(NAME wavelength_fabric_consumer COMMAND wavelength_fabric_consumer)
