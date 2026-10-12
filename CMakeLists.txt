cmake_minimum_required(VERSION 3.16)
project(redis_learning_server LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This server currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(redis-server
    src/main.cpp
    src/net/server.cpp
    src/protocol/resp_parser.cpp
    src/protocol/resp_writer.cpp
    src/commands/command_handler.cpp
)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(resp-parser-test
        tests/resp_parser_test.cpp
        src/protocol/resp_parser.cpp
    )
    target_compile_options(resp-parser-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME resp-parser COMMAND resp-parser-test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME socket-commands
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/socket_commands_test.py
                $<TARGET_FILE:redis-server>)
    endif()
endif()
