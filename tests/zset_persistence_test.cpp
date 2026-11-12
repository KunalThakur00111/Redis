// Check that sorted sets come back with exact scores and order after each way
// the server restores data: AOF replay after a crash, the shutdown snapshot,
// a snapshot plus later AOF records, and a rewritten AOF without a snapshot.
#include "test_support.h"

#include <filesystem>
#include <iostream>

using namespace std::string_literals;

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        const std::string binary = argv[1];
        test::TemporaryDirectory directory;
        const std::string aof_path = directory.path() + "/data.aof";
        std::vector<std::string> options{"--aof", aof_path, "--save-seconds", "0",
                                          "--aof-rewrite-min-size", "0"};
        options.insert(options.end(), argv + 2, argv + argc);

        auto with = [&](test::Server& server){
            return [&server](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
        };

        // Scores that are easy to get wrong in text: -0, a tenth, huge, tiny,
        // infinities and a binary member.
        std::string full_range;
        {
            test::Server server(binary, options);
            auto run = with(server);
            TEST_CHECK(run({"ZADD", "z", "-inf", "low", "-0", "zero", "0.1", "tenth",
                            "1e300", "huge", "5e-324", "tiny", "inf", "high"}) == ":6\r\n");
            TEST_CHECK(run({"ZADD", "z", "1", "bin\0ary"s}) == ":1\r\n");
            TEST_CHECK(run({"ZINCRBY", "z", "0.2", "tenth"}) ==
                       "$19\r\n0.30000000000000004\r\n");
            TEST_CHECK(run({"ZREM", "z", "huge"}) == ":1\r\n");
            TEST_CHECK(run({"ZADD", "gone", "1", "a"}) == ":1\r\n");
            TEST_CHECK(run({"ZPOPMIN", "gone"}) == "*2\r\n$1\r\na\r\n$1\r\n1\r\n");
            TEST_CHECK(run({"ZADD", "ranged", "1", "a", "2", "b", "3", "c", "4", "d"}) == ":4\r\n");
            TEST_CHECK(run({"ZREMRANGEBYSCORE", "ranged", "2", "3"}) == ":2\r\n");
            TEST_CHECK(run({"ZADD", "ttl", "1", "a"}) == ":1\r\n");
            TEST_CHECK(run({"EXPIRE", "ttl", "1000"}) == ":1\r\n");
            full_range = run({"ZRANGE", "z", "0", "-1", "WITHSCORES"});
            TEST_CHECK(full_range.rfind("*12\r\n", 0) == 0);
            server.stop();// SIGTERM is not handled: no snapshot, so this tests AOF replay
        }

        auto check_restored = [&](test::Server& server){
            auto run = with(server);
            TEST_CHECK(run({"ZRANGE", "z", "0", "-1", "WITHSCORES"}) == full_range);
            TEST_CHECK(run({"ZSCORE", "z", "tenth"}) == "$19\r\n0.30000000000000004\r\n");
            TEST_CHECK(run({"ZSCORE", "z", "tiny"}) == "$6\r\n5e-324\r\n");
            TEST_CHECK(run({"ZRANK", "z", "bin\0ary"s}) == ":4\r\n");
            TEST_CHECK(run({"EXISTS", "gone"}) == ":0\r\n");
            TEST_CHECK(run({"ZRANGE", "ranged", "0", "-1"}) == "*2\r\n$1\r\na\r\n$1\r\nd\r\n");
            TEST_CHECK(run({"TYPE", "z"}) == "+zset\r\n");
            const std::string ttl = run({"TTL", "ttl"});
            TEST_CHECK(ttl != ":-1\r\n" && ttl != ":-2\r\n");
        };

        // 1. AOF replay.
        {
            test::Server server(binary, options);
            check_restored(server);
            server.interrupt();// a clean shutdown writes a snapshot
        }
        TEST_CHECK(std::filesystem::exists(aof_path + ".snapshot"));

        // 2. Snapshot only, then 3. snapshot plus later AOF records.
        {
            test::Server server(binary, options);
            check_restored(server);
            auto run = with(server);
            TEST_CHECK(run({"ZADD", "later", "2.5", "x"}) == ":1\r\n");
            server.stop();
        }
        {
            test::Server server(binary, options);
            check_restored(server);
            TEST_CHECK(with(server)({"ZSCORE", "later", "x"}) == "$3\r\n2.5\r\n");

            // 4. A rewritten AOF restores everything even without the snapshot.
            TEST_CHECK(with(server)({"BGREWRITEAOF"}) ==
                       "+Background append only file rewriting started\r\n");
            server.stop();
        }
        std::filesystem::remove(aof_path + ".snapshot");
        {
            test::Server server(binary, options);
            check_restored(server);
            TEST_CHECK(with(server)({"ZSCORE", "later", "x"}) == "$3\r\n2.5\r\n");
        }
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
