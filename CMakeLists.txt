cmake_minimum_required(VERSION 3.16)
project(redis_learning_server LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This server currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(redis-server
    src/main.cpp
    src/net/server.cpp
    src/persistence/aof.cpp
    src/persistence/snapshot.cpp
    src/protocol/resp_parser.cpp
    src/protocol/resp_writer.cpp
    src/commands/command_handler.cpp
)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(redis-server PRIVATE Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(resp-parser-test
        tests/resp_parser_test.cpp
        src/protocol/resp_parser.cpp
    )
    target_compile_options(resp-parser-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME resp-parser COMMAND resp-parser-test)

    add_executable(snapshot-test
        tests/snapshot_test.cpp
        src/persistence/snapshot.cpp
    )
    target_compile_options(snapshot-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME snapshot COMMAND snapshot-test)

    add_executable(aof-sync-test
        tests/aof_sync_test.cpp
        src/persistence/aof.cpp
        src/persistence/snapshot.cpp
        src/protocol/resp_parser.cpp
        src/protocol/resp_writer.cpp
        src/commands/command_handler.cpp
    )
    target_compile_options(aof-sync-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(aof-sync-test PRIVATE Threads::Threads)
    target_link_options(aof-sync-test PRIVATE -Wl,--wrap=fdatasync)
    add_test(NAME aof-sync COMMAND aof-sync-test)

    add_executable(socket-commands-test tests/socket_commands_test.cpp)
    add_executable(aof-restart-test
        tests/aof_restart_test.cpp
        src/persistence/snapshot.cpp
    )
    add_executable(aof-sigint-test tests/aof_sigint_test.cpp)
    add_executable(aof-rewrite-test
        tests/aof_rewrite_test.cpp
        src/persistence/snapshot.cpp
    )
    add_executable(io-threads-test tests/io_threads_test.cpp)
    add_executable(differential-ping-test tests/differential_ping_test.cpp)
    add_executable(io-threads-bench benchmarks/io_threads.cpp)
    add_executable(command-latency-bench benchmarks/command_latency.cpp)
    foreach(target socket-commands-test aof-restart-test aof-sigint-test aof-rewrite-test
                   io-threads-test
                   differential-ping-test io-threads-bench command-latency-bench)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endforeach()

    add_test(NAME socket-commands
        COMMAND socket-commands-test $<TARGET_FILE:redis-server>)
    add_test(NAME socket-commands-single-thread
        COMMAND socket-commands-test $<TARGET_FILE:redis-server> --io-threads 1)
    add_test(NAME aof-restart
        COMMAND aof-restart-test $<TARGET_FILE:redis-server>)
    add_test(NAME aof-restart-everysec
        COMMAND aof-restart-test $<TARGET_FILE:redis-server> --aof-fsync everysec)
    add_test(NAME aof-restart-always
        COMMAND aof-restart-test $<TARGET_FILE:redis-server> --aof-fsync always)
    add_test(NAME aof-sigint
        COMMAND aof-sigint-test $<TARGET_FILE:redis-server>)
    add_test(NAME aof-restart-single-thread
        COMMAND aof-restart-test $<TARGET_FILE:redis-server> --io-threads 1)
    add_test(NAME aof-sigint-single-thread
        COMMAND aof-sigint-test $<TARGET_FILE:redis-server> --io-threads 1)
    add_test(NAME aof-rewrite
        COMMAND aof-rewrite-test $<TARGET_FILE:redis-server>)
    add_test(NAME aof-rewrite-single-thread
        COMMAND aof-rewrite-test $<TARGET_FILE:redis-server> --io-threads 1)
    add_test(NAME aof-rewrite-always
        COMMAND aof-rewrite-test $<TARGET_FILE:redis-server> --aof-fsync always)
    add_test(NAME io-threads
        COMMAND io-threads-test $<TARGET_FILE:redis-server> --io-threads 4)
endif()
