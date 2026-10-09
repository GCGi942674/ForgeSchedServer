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
foreach(name test_scheduler test_protocol test_worker_network test_server_process test_scheduling_driver test_automatic_scheduling test_dispatch_cancellation test_scheduler_exception_safety test_connection_limits)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE forgesched_server_core)
  add_test(NAME ${name} COMMAND ${name} $<TARGET_FILE:server>)
  set_tests_properties(${name} PROPERTIES TIMEOUT 45)
endforeach()
add_dependencies(test_server_process server)
target_link_libraries(test_server_process PRIVATE forgesched_client_api)

add_executable(test_control_plane test_control_plane.cpp)
target_link_libraries(test_control_plane PRIVATE forgesched_server_core forgesched_client_api)
add_test(NAME test_control_plane COMMAND test_control_plane)
set_tests_properties(test_control_plane PROPERTIES TIMEOUT 45)

add_executable(test_client_cli test_client_cli.cpp)
target_link_libraries(test_client_cli PRIVATE forgesched_server_core)
add_dependencies(test_client_cli forgesched_client)
add_test(NAME test_client_cli COMMAND test_client_cli $<TARGET_FILE:forgesched_client>)
set_tests_properties(test_client_cli PROPERTIES TIMEOUT 45)

add_executable(test_client_transport test_client_transport.cpp)
target_link_libraries(test_client_transport PRIVATE forgesched_client_api)
add_test(NAME test_client_transport COMMAND test_client_transport)
set_tests_properties(test_client_transport PROPERTIES TIMEOUT 45)

find_package(Python3 3.6 REQUIRED COMPONENTS Interpreter)
add_executable(test_python_worker test_python_worker.cpp)
target_link_libraries(test_python_worker PRIVATE forgesched_server_core forgesched_client_api)
add_test(NAME test_python_worker COMMAND test_python_worker ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/worker/forge_worker.py)
set_tests_properties(test_python_worker PROPERTIES TIMEOUT 45)

add_executable(test_pjtest_adapter test_pjtest_adapter.cpp)
target_link_libraries(test_pjtest_adapter PRIVATE forgesched_server_core)
add_dependencies(test_pjtest_adapter forgesched_client)
add_test(NAME test_pjtest_adapter COMMAND test_pjtest_adapter
  ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/worker/forge_worker.py
  $<TARGET_FILE:forgesched_client> ${PROJECT_SOURCE_DIR}/tests/fixtures/pjtest/setup.py)
set_tests_properties(test_pjtest_adapter PROPERTIES TIMEOUT 60)

add_executable(test_worker_safety test_worker_safety.cpp)
add_test(NAME test_worker_safety COMMAND test_worker_safety
  ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/fixtures/worker_safety.py)
set_tests_properties(test_worker_safety PROPERTIES TIMEOUT 90)
