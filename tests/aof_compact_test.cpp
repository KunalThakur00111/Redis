// Check that writes to large keys add small AOF records, that keys with an
// expiry still cannot come back after it passes, and that SPOP replays as the
// members it actually removed.
#include "test_support.h"

#include <filesystem>
#include <iostream>

namespace {

// Fill a key with count members using commands of at most 500 members each.
void fill(std::uint16_t port, const std::string& command, const std::string& key, int count,
          bool with_scores) {
    std::string pipeline;
    for(int start = 0; start < count; start += 500){
        std::vector<std::string> arguments{command, key};
        for(int i = start; i < start + 500 && i < count; i++){
            if(with_scores) arguments.push_back(std::to_string(i));
            arguments.push_back("member-" + std::to_string(i));
            if(command == "HSET") arguments.push_back("value");
        }
        pipeline += "*" + std::to_string(arguments.size()) + "\r\n";
        for(const auto& argument : arguments){
            pipeline += "$" + std::to_string(argument.size()) + "\r\n" + argument + "\r\n";
        }
    }
    test::exchange(port, pipeline);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        const std::string binary = argv[1];
        test::TemporaryDirectory directory;
        const std::string aof_path = directory.path() + "/data.aof";
        std::vector<std::string> options{"--aof", aof_path, "--save-seconds", "0",
                                          "--aof-rewrite-min-size", "0"};
        options.insert(options.end(), argv + 2, argv + argc);

        std::string members_before;
        std::vector<std::string> state_before;// replies that must be the same after replay
        const std::vector<std::vector<std::string>> state_queries{
            {"ZCARD", "z"}, {"ZRANGE", "z", "0", "199", "WITHSCORES"},
            {"ZRANGE", "z", "-200", "-1", "WITHSCORES"}, {"HLEN", "h"}, {"LLEN", "l"},
            {"LRANGE", "l", "0", "99"}, {"LRANGE", "l", "-100", "-1"},
        };
        auto query = [](std::uint16_t port, const std::vector<std::string>& arguments){
            std::string payload = "*" + std::to_string(arguments.size()) + "\r\n";
            for(const auto& argument : arguments){
                payload += "$" + std::to_string(argument.size()) + "\r\n" + argument + "\r\n";
            }
            return test::exchange(port, payload);
        };
        {
            test::Server server(binary, options);
            auto run = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
            constexpr int size = 20000;
            fill(server.port(), "ZADD", "z", size, true);
            fill(server.port(), "SADD", "s", size, false);
            fill(server.port(), "HSET", "h", size, false);
            fill(server.port(), "RPUSH", "l", size, false);
            TEST_CHECK(run({"ZCARD", "z"}) == ":20000\r\n");

            // Each write to a 20,000-member key adds a record about the size of
            // the command, not of the key (which would be about a megabyte).
            auto growth = [&](std::initializer_list<std::string_view> args, const std::string& expected){
                const auto before = std::filesystem::file_size(aof_path);
                const std::string reply = run(args);
                if(reply != expected) std::cerr << "unexpected reply to " << *args.begin()
                                                << ": " << reply << '\n';
                TEST_CHECK(reply == expected);
                return std::filesystem::file_size(aof_path) - before;
            };
            TEST_CHECK(growth({"ZINCRBY", "z", "5", "member-7"}, "$2\r\n12\r\n") < 200);
            TEST_CHECK(growth({"ZADD", "z", "GT", "CH", "1e6", "member-8"}, ":1\r\n") < 200);
            TEST_CHECK(growth({"ZREM", "z", "member-9"}, ":1\r\n") < 200);
            TEST_CHECK(growth({"ZPOPMAX", "z"}, "*2\r\n$8\r\nmember-8\r\n$7\r\n1000000\r\n") < 200);
            TEST_CHECK(growth({"ZREMRANGEBYSCORE", "z", "100", "(110"}, ":10\r\n") < 200);
            TEST_CHECK(growth({"SADD", "s", "extra"}, ":1\r\n") < 200);
            TEST_CHECK(growth({"HINCRBY", "h", "counter", "3"}, ":3\r\n") < 200);
            TEST_CHECK(growth({"LSET", "l", "5", "changed"}, "+OK\r\n") < 200);
            TEST_CHECK(growth({"RPOP", "l"}, "$12\r\nmember-19999\r\n") < 200);
            TEST_CHECK(growth({"EXPIRE", "z", "100000"}, ":1\r\n") < 200);
            // z had an expiry before PERSIST, so its whole state is logged: a
            // record of just "PERSIST z" after "PEXPIREAT z <deadline>" would lose
            // z on any replay after that deadline.
            TEST_CHECK(growth({"PERSIST", "z"}, ":1\r\n") > 100000);
            const std::string spop = run({"SPOP", "s", "3"});
            TEST_CHECK(spop.rfind("*3\r\n", 0) == 0);
            members_before = run({"SCARD", "s"});

            // A key with an expiry still logs its whole state, so a write cannot
            // bring it back without that expiry.
            TEST_CHECK(run({"EXPIRE", "s", "100000"}) == ":1\r\n");
            TEST_CHECK(growth({"SADD", "s", "with-ttl"}, ":1\r\n") > 100000);
            TEST_CHECK(run({"PERSIST", "s"}) == ":1\r\n");

            TEST_CHECK(run({"ZADD", "short", "1", "a"}) == ":1\r\n");
            TEST_CHECK(run({"PEXPIRE", "short", "300"}) == ":1\r\n");
            TEST_CHECK(run({"ZADD", "short", "2", "b"}) == ":1\r\n");
            TEST_CHECK(run({"RPUSH", "short-list", "a"}) == ":1\r\n");
            TEST_CHECK(run({"PEXPIRE", "short-list", "300"}) == ":1\r\n");
            TEST_CHECK(run({"RPUSH", "short-list", "b"}) == ":2\r\n");

            // Keys whose short expiry was later extended or removed must survive
            // a replay that happens after the original deadline.
            TEST_CHECK(run({"RPUSH", "extended", "a", "b"}) == ":2\r\n");
            TEST_CHECK(run({"PEXPIRE", "extended", "200"}) == ":1\r\n");
            TEST_CHECK(run({"PEXPIRE", "extended", "600000"}) == ":1\r\n");
            TEST_CHECK(run({"ZADD", "persisted", "1", "a"}) == ":1\r\n");
            TEST_CHECK(run({"PEXPIRE", "persisted", "200"}) == ":1\r\n");
            TEST_CHECK(run({"PERSIST", "persisted"}) == ":1\r\n");
            TEST_CHECK(run({"SET", "string-ttl", "v", "PX", "200"}) == "+OK\r\n");
            TEST_CHECK(run({"PERSIST", "string-ttl"}) == ":1\r\n");
            for(const auto& arguments : state_queries) state_before.push_back(query(server.port(), arguments));
            server.stop();// SIGTERM is not handled: restart replays the AOF only
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        test::Server server(binary, options);
        auto run = [&](std::initializer_list<std::string_view> args){
            return test::exchange_request(server.port(), args);
        };
        TEST_CHECK(run({"ZSCORE", "z", "member-7"}) == "$2\r\n12\r\n");
        TEST_CHECK(run({"ZSCORE", "z", "member-8"}) == "$-1\r\n");
        TEST_CHECK(run({"ZSCORE", "z", "member-9"}) == "$-1\r\n");
        TEST_CHECK(run({"ZCOUNT", "z", "100", "(110"}) == ":0\r\n");
        TEST_CHECK(run({"ZCARD", "z"}) == ":19988\r\n");// 20,000 - ZREM - ZPOPMAX - 10 in range
        for(std::size_t i = 0; i < state_queries.size(); i++){
            TEST_CHECK(query(server.port(), state_queries[i]) == state_before[i]);
        }
        TEST_CHECK(run({"TTL", "z"}) == ":-1\r\n");
        TEST_CHECK(run({"SCARD", "s"}) == ":" + std::to_string(std::stoi(members_before.substr(1)) + 1) + "\r\n");
        TEST_CHECK(run({"SISMEMBER", "s", "with-ttl"}) == ":1\r\n");
        TEST_CHECK(run({"TTL", "s"}) == ":-1\r\n");
        TEST_CHECK(run({"HGET", "h", "counter"}) == "$1\r\n3\r\n");
        TEST_CHECK(run({"LINDEX", "l", "5"}) == "$7\r\nchanged\r\n");
        TEST_CHECK(run({"LLEN", "l"}) == ":19999\r\n");
        TEST_CHECK(run({"EXISTS", "short", "short-list"}) == ":0\r\n");// expired, not revived
        TEST_CHECK(run({"LRANGE", "extended", "0", "-1"}) == "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
        const std::string extended_ttl = run({"TTL", "extended"});
        TEST_CHECK(extended_ttl != ":-1\r\n" && extended_ttl != ":-2\r\n");
        TEST_CHECK(run({"ZSCORE", "persisted", "a"}) == "$1\r\n1\r\n");
        TEST_CHECK(run({"TTL", "persisted"}) == ":-1\r\n");
        TEST_CHECK(run({"GET", "string-ttl"}) == "$1\r\nv\r\n");
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
