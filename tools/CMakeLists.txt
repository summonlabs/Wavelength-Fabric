# wfcli: the inspection and control utility.
#
# It links the reference transport because the "server" mode drives a runtime
# that is owned by another process through the framed loopback TCP client.
# "local" mode uses the core library only, transitively through the transport.
add_executable(wfcli wfcli.cpp)
target_link_libraries(wfcli PRIVATE WavelengthFabric::Transport)
target_compile_options(wfcli PRIVATE ${WF_WARN_FLAGS})

# Collect the executables next to the libraries so that a build tree can be
# exercised from a single directory (${CMAKE_BINARY_DIR}/Release for a
# multi-configuration generator).
set_target_properties(wfcli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
