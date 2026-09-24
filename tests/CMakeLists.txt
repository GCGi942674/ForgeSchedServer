# External mirror subset: these are the existing tests, not the missing
# internal ForgeSched acceptance suite.
message(STATUS "External mirror: building available tests only")
foreach(name test_logging test_thread_pool test_EchoClient test_EchoServer)
  add_executable(${name} ${name}.cpp)
  if(name MATCHES "Echo")
    target_sources(${name} PRIVATE ../client/src/EchoClient.cpp)
    target_include_directories(${name} PRIVATE ../client/include)
  endif()
  target_link_libraries(${name} PRIVATE forgesched_server_core)
  target_compile_options(${name} PRIVATE -UNDEBUG)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 45)
endforeach()

# External regressions for the currently synchronized development stage.
foreach(name test_scheduler test_protocol test_worker_network test_server_process)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE forgesched_server_core)
  add_test(NAME ${name} COMMAND ${name} $<TARGET_FILE:server>)
  set_tests_properties(${name} PROPERTIES TIMEOUT 45)
endforeach()
add_dependencies(test_server_process server)
