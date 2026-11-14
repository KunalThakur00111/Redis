// Check TYPE, KEYS, SCAN, RENAME, FLUSHDB/FLUSHALL, SELECT, COMMAND, INFO and
// CONFIG GET over a real TCP connection, and that the writes among them persist.
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

using namespace std::string_literals;

namespace {

// Read one bulk string at position; return its text and move position past it.
std::string read_bulk(const std::string& reply, std::size_t& position) {
    TEST_CHECK(position < reply.size() && reply[position] == '$');
    const std::size_t end = reply.find("\r\n", position);
    TEST_CHECK(end != std::string::npos);
    const std::size_t length = std::stoul(reply.substr(position + 1, end - position - 1));
    position = end + 2;
    TEST_CHECK(position + length + 2 <= reply.size());
    std::string text = reply.substr(position, length);
    position += length;
    TEST_CHECK(reply.substr(position, 2) == "\r\n");
    position += 2;
    return text;
}

// Read an array of bulk strings at position.
std::vector<std::string> read_bulk_array(const std::string& reply, std::size_t& position) {
    TEST_CHECK(position < reply.size() && reply[position] == '*');
    const std::size_t end = reply.find("\r\n", position);
    TEST_CHECK(end != std::string::npos);
    const int count = std::stoi(reply.substr(position + 1, end - position - 1));
    position = end + 2;
    std::vector<std::string> items;
    for(int i = 0; i < count; i++) items.push_back(read_bulk(reply, position));
    return items;
}

std::vector<std::string> sorted_array(const std::string& reply) {
    std::size_t position = 0;
    auto items = read_bulk_array(reply, position);
    TEST_CHECK(position == reply.size());
    std::sort(items.begin(), items.end());
    return items;
}

struct ScanPage {
    std::string cursor;
    std::vector<std::string> keys;
};

ScanPage parse_scan(const std::string& reply) {
    TEST_CHECK(reply.rfind("*2\r\n", 0) == 0);
    std::size_t position = 4;
    ScanPage page;
    page.cursor = read_bulk(reply, position);
    page.keys = read_bulk_array(reply, position);
    TEST_CHECK(position == reply.size());
    return page;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        const std::string binary = argv[1];
        const std::vector<std::string> extra(argv + 2, argv + argc);

        test::Server server(binary, extra);
        auto exchange = [&](std::initializer_list<std::string_view> args){
            return test::exchange_request(server.port(), args);
        };

        // TYPE
        TEST_CHECK(exchange({"SET", "s", "v"}) == "+OK\r\n");
        TEST_CHECK(exchange({"RPUSH", "l", "a"}) == ":1\r\n");
        TEST_CHECK(exchange({"HSET", "h", "f", "v"}) == ":1\r\n");
        TEST_CHECK(exchange({"SADD", "z", "m"}) == ":1\r\n");
        TEST_CHECK(exchange({"TYPE", "s"}) == "+string\r\n");
        TEST_CHECK(exchange({"type", "l"}) == "+list\r\n");
        TEST_CHECK(exchange({"TYPE", "h"}) == "+hash\r\n");
        TEST_CHECK(exchange({"TYPE", "z"}) == "+set\r\n");
        TEST_CHECK(exchange({"TYPE", "missing"}) == "+none\r\n");
        TEST_CHECK(exchange({"TYPE"}) == "-ERR wrong number of arguments for 'type' command\r\n");

        // KEYS
        TEST_CHECK(sorted_array(exchange({"KEYS", "*"})) ==
                   (std::vector<std::string>{"h", "l", "s", "z"}));
        TEST_CHECK(sorted_array(exchange({"KEYS", "[hl]"})) ==
                   (std::vector<std::string>{"h", "l"}));
        TEST_CHECK(exchange({"KEYS", "nothing*"}) == "*0\r\n");
        TEST_CHECK(exchange({"SET", "gone", "x", "PX", "30"}) == "+OK\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        TEST_CHECK(sorted_array(exchange({"KEYS", "g*"})).empty());

        // SCAN errors
        TEST_CHECK(exchange({"SCAN", "abc"}) == "-ERR invalid cursor\r\n");
        TEST_CHECK(exchange({"SCAN", "-1"}) == "-ERR invalid cursor\r\n");
        TEST_CHECK(exchange({"SCAN", "0", "COUNT", "0"}) == "-ERR syntax error\r\n");
        TEST_CHECK(exchange({"SCAN", "0", "COUNT", "x"}) ==
                   "-ERR value is not an integer or out of range\r\n");
        TEST_CHECK(exchange({"SCAN", "0", "MATCH"}) == "-ERR syntax error\r\n");
        TEST_CHECK(exchange({"SCAN", "0", "BOGUS", "1"}) == "-ERR syntax error\r\n");

        // A full SCAN returns every key that exists throughout, even while new
        // keys make the hash table grow (and rehash) between calls. 500 keys
        // fill libstdc++'s 541-bucket table, so the added keys force a rehash.
        TEST_CHECK(exchange({"FLUSHALL"}) == "+OK\r\n");
        std::string pipeline;
        std::set<std::string> original;
        for(int i = 0; i < 500; i++){
            const std::string key = "key:" + std::to_string(i);
            original.insert(key);
            pipeline += test::request({"SET", key, "v"});
        }
        test::exchange(server.port(), pipeline);

        std::set<std::string> seen;
        std::string cursor = "0";
        int calls = 0;
        int added = 0;
        do {
            const auto page = parse_scan(exchange({"SCAN", cursor, "COUNT", "10"}));
            seen.insert(page.keys.begin(), page.keys.end());
            cursor = page.cursor;
            std::string growth;
            for(int i = 0; i < 5; i++){
                growth += test::request({"SET", "new:" + std::to_string(added++), "v"});
            }
            test::exchange(server.port(), growth);
            TEST_CHECK(++calls < 10000);
        } while(cursor != "0");
        for(const auto& key : original) TEST_CHECK(seen.count(key) == 1);

        // MATCH and TYPE filter the keys.
        TEST_CHECK(exchange({"FLUSHDB", "async"}) == "+OK\r\n");
        TEST_CHECK(exchange({"MSET", "user:1", "a", "user:2", "b", "other", "c"}) == "+OK\r\n");
        TEST_CHECK(exchange({"SADD", "user:set", "m"}) == ":1\r\n");
        {
            const auto page = parse_scan(exchange({"SCAN", "0", "MATCH", "user:*",
                                                   "TYPE", "STRING", "COUNT", "100"}));
            TEST_CHECK(page.cursor == "0");
            auto keys = page.keys;
            std::sort(keys.begin(), keys.end());
            TEST_CHECK(keys == (std::vector<std::string>{"user:1", "user:2"}));
        }

        // RENAME and RENAMENX
        TEST_CHECK(exchange({"SET", "src", "value", "EX", "1000"}) == "+OK\r\n");
        TEST_CHECK(exchange({"RENAME", "src", "dst"}) == "+OK\r\n");
        TEST_CHECK(exchange({"EXISTS", "src"}) == ":0\r\n");
        TEST_CHECK(exchange({"GET", "dst"}) == "$5\r\nvalue\r\n");
        const std::string ttl = exchange({"TTL", "dst"});
        TEST_CHECK(ttl != ":-1\r\n" && ttl != ":-2\r\n");// the expiry moved too
        TEST_CHECK(exchange({"RENAME", "missing", "x"}) == "-ERR no such key\r\n");
        TEST_CHECK(exchange({"RENAME", "dst", "dst"}) == "+OK\r\n");
        TEST_CHECK(exchange({"SET", "other2", "o"}) == "+OK\r\n");
        TEST_CHECK(exchange({"RENAMENX", "dst", "other2"}) == ":0\r\n");
        TEST_CHECK(exchange({"RENAMENX", "dst", "fresh"}) == ":1\r\n");
        TEST_CHECK(exchange({"RENAME", "fresh", "other2"}) == "+OK\r\n");// overwrites
        TEST_CHECK(exchange({"GET", "other2"}) == "$5\r\nvalue\r\n");
        TEST_CHECK(exchange({"RENAMENX", "missing", "x"}) == "-ERR no such key\r\n");

        // FLUSHDB / FLUSHALL
        TEST_CHECK(exchange({"FLUSHALL", "now"}) == "-ERR syntax error\r\n");
        TEST_CHECK(exchange({"FLUSHALL", "SYNC"}) == "+OK\r\n");
        TEST_CHECK(exchange({"DBSIZE"}) == ":0\r\n");

        // SELECT
        TEST_CHECK(exchange({"SELECT", "0"}) == "+OK\r\n");
        TEST_CHECK(exchange({"SELECT", "1"}) == "-ERR DB index is out of range\r\n");
        TEST_CHECK(exchange({"SELECT", "x"}) == "-ERR value is not an integer or out of range\r\n");

        // COMMAND
        TEST_CHECK(exchange({"COMMAND", "DOCS"}) == "*0\r\n");
        const std::string command_count = exchange({"COMMAND", "COUNT"});
        TEST_CHECK(command_count.size() > 3 && command_count[0] == ':');
        const auto names = sorted_array(exchange({"COMMAND", "LIST"}));
        TEST_CHECK(":" + std::to_string(names.size()) + "\r\n" == command_count);
        TEST_CHECK(std::find(names.begin(), names.end(), "scan") != names.end());
        TEST_CHECK(exchange({"COMMAND", "bad\r\nx"}) ==
                   "-ERR unknown subcommand 'bad  x'. Try COMMAND HELP.\r\n");

        // INFO
        TEST_CHECK(exchange({"SET", "a", "1", "EX", "100"}) == "+OK\r\n");
        TEST_CHECK(exchange({"SET", "b", "2"}) == "+OK\r\n");
        {
            std::size_t position = 0;
            const std::string keyspace = read_bulk(exchange({"INFO", "keyspace"}), position);
            TEST_CHECK(keyspace == "# Keyspace\r\ndb0:keys=2,expires=1,avg_ttl=0\r\n");
            position = 0;
            const std::string all = read_bulk(exchange({"INFO"}), position);
            TEST_CHECK(all.find("# Server\r\n") == 0);
            TEST_CHECK(all.find("tcp_port:" + std::to_string(server.port()) + "\r\n") !=
                       std::string::npos);
            TEST_CHECK(all.find("aof_enabled:0\r\n") != std::string::npos);
            // io_threads_active is a flag, as in Redis; the count is separate.
            const bool threaded = !(extra.size() >= 2 && extra[0] == "--io-threads" &&
                                    extra[1] == "1");
            TEST_CHECK(all.find(std::string("io_threads_active:") + (threaded ? "1" : "0") +
                                "\r\n") != std::string::npos);
            TEST_CHECK(all.find(std::string("io_threads:") + (threaded ? "4" : "1") + "\r\n") !=
                       std::string::npos);
            TEST_CHECK(exchange({"INFO", "nosuchsection"}) == "$0\r\n\r\n");
        }

        // CONFIG GET
        TEST_CHECK(exchange({"CONFIG", "GET", "appendonly"}) ==
                   "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n");
        TEST_CHECK(exchange({"CONFIG", "GET", "databases", "PORT"}) ==
                   // An exact name is echoed as written, as Redis does.
                   "*4\r\n$4\r\nPORT\r\n$" + std::to_string(std::to_string(server.port()).size()) +
                   "\r\n" + std::to_string(server.port()) + "\r\n$9\r\ndatabases\r\n$1\r\n1\r\n");
        TEST_CHECK(sorted_array(exchange({"CONFIG", "GET", "auto-aof-*"})).size() == 4);
        TEST_CHECK(exchange({"CONFIG", "GET", "nope"}) == "*0\r\n");
        TEST_CHECK(exchange({"CONFIG", "GET"}) ==
                   "-ERR wrong number of arguments for 'config|get' command\r\n");
        TEST_CHECK(exchange({"CONFIG", "SET", "save", ""}) ==
                   "-ERR CONFIG SET is not supported\r\n");

        // RENAME and FLUSHALL are logged in the AOF and replayed after a restart.
        {
            test::TemporaryDirectory directory;
            std::vector<std::string> options{"--aof", directory.path() + "/data.aof"};
            options.insert(options.end(), extra.begin(), extra.end());
            {
                test::Server persistent(binary, options);
                auto run = [&](std::initializer_list<std::string_view> args){
                    return test::exchange_request(persistent.port(), args);
                };
                TEST_CHECK(run({"SET", "before", "flush"}) == "+OK\r\n");
                TEST_CHECK(run({"FLUSHALL"}) == "+OK\r\n");
                TEST_CHECK(run({"SET", "old", "v", "PX", "600000"}) == "+OK\r\n");
                TEST_CHECK(run({"SET", "target", "replaced"}) == "+OK\r\n");
                TEST_CHECK(run({"RENAME", "old", "target"}) == "+OK\r\n");
                TEST_CHECK(run({"CONFIG", "GET", "appendonly"}) ==
                           "*2\r\n$10\r\nappendonly\r\n$3\r\nyes\r\n");
                // SIGTERM is not handled, so restart replays the AOF without a snapshot.
                persistent.stop();
            }
            test::Server persistent(binary, options);
            auto run = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(persistent.port(), args);
            };
            TEST_CHECK(run({"EXISTS", "before", "old"}) == ":0\r\n");
            TEST_CHECK(run({"GET", "target"}) == "$1\r\nv\r\n");
            const std::string restored_ttl = run({"TTL", "target"});
            TEST_CHECK(restored_ttl != ":-1\r\n" && restored_ttl != ":-2\r\n");
            TEST_CHECK(run({"DBSIZE"}) == ":1\r\n");
        }

        // An EXEC is one AOF record: cutting that record short (a crash while
        // writing it) drops the whole transaction, never half of it.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            std::vector<std::string> options{"--aof", aof_path, "--save-seconds", "0"};
            options.insert(options.end(), extra.begin(), extra.end());
            std::uintmax_t before_exec = 0;
            {
                test::Server persistent(binary, options);
                test::Fd connection = test::connect_to(persistent.port());
                auto run = [&](std::initializer_list<std::string_view> args, std::size_t reply_size){
                    test::send_all(connection.get(), test::request(args));
                    return test::read_exact(connection.get(), reply_size);
                };
                TEST_CHECK(run({"SET", "kept", "yes"}, 5) == "+OK\r\n");
                before_exec = std::filesystem::file_size(aof_path);
                TEST_CHECK(run({"MULTI"}, 5) == "+OK\r\n");
                TEST_CHECK(run({"SET", "a", "1"}, 9) == "+QUEUED\r\n");
                TEST_CHECK(run({"SET", "b", "2"}, 9) == "+QUEUED\r\n");
                TEST_CHECK(run({"EXEC"}, 14) == "*2\r\n+OK\r\n+OK\r\n");
                persistent.stop();
            }
            std::filesystem::resize_file(aof_path, std::filesystem::file_size(aof_path) - 1);
            test::Server persistent(binary, options);
            auto run = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(persistent.port(), args);
            };
            TEST_CHECK(std::filesystem::file_size(aof_path) == before_exec);// the cut record was dropped
            TEST_CHECK(run({"GET", "kept"}) == "$3\r\nyes\r\n");
            TEST_CHECK(run({"EXISTS", "a", "b"}) == ":0\r\n");
        }
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
