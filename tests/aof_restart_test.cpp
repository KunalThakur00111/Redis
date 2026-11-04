// Check that the optional append-only log restores data after a restart.
#include "test_support.h"
#include "../src/persistence/snapshot.h"
#include "../src/storage/database.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
            server.interrupt();
        }

        auto complete_size = std::filesystem::file_size(aof_path);
        const std::string snapshot_path = aof_path + ".snapshot";
        std::string snapshot_bytes;
        {
            std::ifstream snapshot_file(snapshot_path, std::ios::binary);
            TEST_CHECK(static_cast<bool>(snapshot_file));
            snapshot_bytes.assign(std::istreambuf_iterator<char>{snapshot_file},
                                  std::istreambuf_iterator<char>{});
            TEST_CHECK(!snapshot_file.bad());
            storage::Database snapshot_database;
            std::uint64_t snapshot_offset = 0;
            std::string snapshot_error;
            TEST_CHECK(persistence::decode_snapshot(snapshot_bytes, snapshot_database,
                                                    snapshot_offset, snapshot_error));
            TEST_CHECK(snapshot_offset == complete_size);
            TEST_CHECK(std::get<std::string>(snapshot_database.values.at("text").value) ==
                       "a\0b"s);
            TEST_CHECK(snapshot_database.values.count("second") == 0);
            TEST_CHECK(snapshot_database.values.count("items") == 1);
            TEST_CHECK(snapshot_database.values.count("profile") == 1);
            TEST_CHECK(snapshot_database.values.count("members") == 1);
        }
        {
            storage::Database mismatched_database;
            std::uint64_t offset = 0;
            std::string error;
            TEST_CHECK(persistence::decode_snapshot(snapshot_bytes, mismatched_database,
                                                    offset, error));
            mismatched_database.values.erase("text");
            const std::string mismatched = persistence::encode_snapshot(
                mismatched_database, offset);
            TEST_CHECK(persistence::save_snapshot_file(snapshot_path, mismatched, error));
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "text"}) ==
                       "$3\r\na\0b\r\n"s);
            TEST_CHECK(persistence::save_snapshot_file(snapshot_path, snapshot_bytes, error));
        }
        {
            std::ofstream log(aof_path, std::ios::binary | std::ios::app);
            for(int byte = 0; byte < 8; byte++) log.put(byte == 0 ? 20 : 0);
            log.write("partial", 7);
            TEST_CHECK(static_cast<bool>(log));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::filesystem::rename(snapshot_path, snapshot_path + ".saved");

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

        std::filesystem::rename(snapshot_path + ".saved", snapshot_path);
        {
            std::fstream log(aof_path, std::ios::binary | std::ios::in | std::ios::out);
            TEST_CHECK(static_cast<bool>(log));
            for(int byte = 0; byte < 8; byte++) log.put(static_cast<char>(0xff));
            TEST_CHECK(static_cast<bool>(log));
        }

        {
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "counter"}) ==
                       "$1\r\n1\r\n");
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "text"}) ==
                       "$3\r\na\0b\r\n"s);
        }
        std::cout << "AOF restart test passed\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
