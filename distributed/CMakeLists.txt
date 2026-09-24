# Reference deployment: a real server process, a real client process, and the
# multiprocess proof that drives them. The proof uses genuine operating-system
# processes and a real loopback TCP transport; threads are not used as a
# substitute for multiprocess evidence.
set(WF_DIST_WARN)
if(MSVC)
  set(WF_DIST_WARN /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
  set(WF_DIST_WARN -Wall -Wextra -Werror -Wpedantic)
endif()

function(wf_dist_exe target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE WavelengthFabric::Transport)
  target_compile_options(${target} PRIVATE ${WF_DIST_WARN})
endfunction()

wf_dist_exe(wavelengthd wavelengthd_main.cpp)
wf_dist_exe(wf_mp_client mp_client_main.cpp)
wf_dist_exe(wf_mp_scenario mp_scenario.cpp)

# No CTest TIMEOUT is set: the proof synchronizes through real transport
# handshakes and readiness checks that fail loudly rather than being killed.
add_test(NAME mp_scenario
  COMMAND wf_mp_scenario $<TARGET_FILE:wavelengthd> $<TARGET_FILE:wf_mp_client>)
