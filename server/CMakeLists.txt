# server/CMakeLists.txt

add_library(forgesched_server_core STATIC
    src/Connection.cpp
    src/EchoServer.cpp
    src/EventLoop.cpp
    src/Acceptor.cpp
    src/ServerMetrics.cpp
    src/EchoHandler.cpp
    src/EventLoopThread.cpp
    src/EventLoopThreadPool.cpp
    src/Message.cpp
    src/NetworkWorkerDispatcher.cpp
    src/ProtocolRouter.cpp
    src/Scheduler.cpp
    src/SchedulingCoordinator.cpp
    src/TaskIdGenerator.cpp
    src/TaskService.cpp
    src/WorkerConnectionRegistry.cpp
    src/WorkerManager.cpp
    src/WorkerServer.cpp
    src/WorkerSession.cpp
    src/WorkerSessionHandler.cpp
)

target_include_directories(forgesched_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(forgesched_server_core
    PUBLIC
        common
)
add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE forgesched_server_core)
