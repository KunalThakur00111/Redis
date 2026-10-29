// Check that the optional append-only log restores data after a restart.
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace std::string_literals;

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        test::TemporaryDirectory directory;
        std::string aof_path = directory.path() + "/data.aof";
        std::vector<std::string> options{"--aof", aof_path};
        options.insert(options.end(), argv + 2, argv + argc);

        {
            test::Server server(argv[1], options);
            auto exchange = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
            TEST_CHECK(exchange({"SET", "text", "a\0b"s}) == "+OK\r\n");
            TEST_CHECK(exchange({"MSET", "first", "one", "second", "two"}) == "+OK\r\n");
            TEST_CHECK(exchange({"DEL", "second"}) == ":1\r\n");
            TEST_CHECK(exchange({"RPUSH", "items", "a", "b"}) == ":2\r\n");
            TEST_CHECK(exchange({"HSET", "profile", "name", "Kunal"}) == ":1\r\n");
            TEST_CHECK(exchange({"SADD", "members", "x", "y"}) == ":2\r\n");
            TEST_CHECK(exchange({"SET", "short", "gone", "PX", "150"}) == "+OK\r\n");
            TEST_CHECK(exchange({"PEXPIRE", "items", "60000"}) == ":1\r\n");
        }

        auto complete_size = std::filesystem::file_size(aof_path);
        {
            std::ofstream log(aof_path, std::ios::binary | std::ios::app);
            for(int byte = 0; byte < 8; byte++) log.put(byte == 0 ? 20 : 0);
            log.write("partial", 7);
            TEST_CHECK(static_cast<bool>(log));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        {
            test::Server server(argv[1], options);
            auto exchange = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
            TEST_CHECK(std::filesystem::file_size(aof_path) == complete_size);
            TEST_CHECK(exchange({"GET", "text"}) == "$3\r\na\0b\r\n"s);
            TEST_CHECK(exchange({"GET", "first"}) == "$3\r\none\r\n");
            TEST_CHECK(exchange({"GET", "second"}) == "$-1\r\n");
            TEST_CHECK(exchange({"LRANGE", "items", "0", "-1"}) ==
                       "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
            std::string ttl = exchange({"PTTL", "items"});
            TEST_CHECK(ttl.front() == ':' && std::stoll(ttl.substr(1)) > 0);
            TEST_CHECK(exchange({"HGET", "profile", "name"}) == "$5\r\nKunal\r\n");
            TEST_CHECK(exchange({"SISMEMBER", "members", "x"}) == ":1\r\n");
            TEST_CHECK(exchange({"SISMEMBER", "members", "y"}) == ":1\r\n");
            TEST_CHECK(exchange({"GET", "short"}) == "$-1\r\n");
            TEST_CHECK(exchange({"INCR", "counter"}) == ":1\r\n");
        }

        {
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "counter"}) ==
                       "$1\r\n1\r\n");
        }
        std::cout << "AOF restart test passed\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
