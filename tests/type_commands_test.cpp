// Check the newer string, list, hash and set commands over TCP: properties of
// the random ones (SPOP, SRANDMEMBER), the APPEND size limit, and that every
// write among them is replayed from the AOF after a crash.
#include "test_support.h"

#include <algorithm>
#include <iostream>
#include <set>

using namespace std::string_literals;

namespace {

// Elements of a flat array of bulk strings, in reply order.
std::vector<std::string> array_items(const std::string& reply) {
    TEST_CHECK(!reply.empty() && reply[0] == '*');
    std::size_t end = reply.find("\r\n");
    const int count = std::stoi(reply.substr(1, end - 1));
    std::size_t position = end + 2;
    std::vector<std::string> items;
    for(int i = 0; i < count; i++){
        TEST_CHECK(reply[position] == '$');
        end = reply.find("\r\n", position);
        const std::size_t length = std::stoul(reply.substr(position + 1, end - position - 1));
        items.push_back(reply.substr(end + 2, length));
        position = end + 2 + length + 2;
    }
    TEST_CHECK(position == reply.size());
    return items;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        const std::string binary = argv[1];
        const std::vector<std::string> extra(argv + 2, argv + argc);

        {
            test::Server server(binary, extra);
            auto run = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
            const std::set<std::string> members{"a", "b", "c", "d", "e"};
            TEST_CHECK(run({"SADD", "s", "a", "b", "c", "d", "e"}) == ":5\r\n");

            // SRANDMEMBER: a positive count gives distinct members, a negative one
            // may repeat them, and neither changes the set.
            for(int round = 0; round < 20; round++){
                const auto three = array_items(run({"SRANDMEMBER", "s", "3"}));
                TEST_CHECK(three.size() == 3);
                TEST_CHECK(std::set<std::string>(three.begin(), three.end()).size() == 3);
                for(const auto& item : three) TEST_CHECK(members.count(item) == 1);
            }
            const auto repeated = array_items(run({"SRANDMEMBER", "s", "-50"}));
            TEST_CHECK(repeated.size() == 50);
            for(const auto& item : repeated) TEST_CHECK(members.count(item) == 1);
            // 50 picks must repeat some member; check they are not all the same one.
            TEST_CHECK(std::set<std::string>(repeated.begin(), repeated.end()).size() >= 2);
            TEST_CHECK(run({"SRANDMEMBER", "s", "-2000000"}) ==
                       "-ERR count is too large for one reply\r\n");
            TEST_CHECK(run({"SCARD", "s"}) == ":5\r\n");

            // SPOP removes exactly what it returns.
            const auto popped = array_items(run({"SPOP", "s", "2"}));
            TEST_CHECK(popped.size() == 2 && popped[0] != popped[1]);
            for(const auto& item : popped){
                TEST_CHECK(run({"SISMEMBER", "s", item}) == ":0\r\n");
            }
            TEST_CHECK(run({"SCARD", "s"}) == ":3\r\n");
            const std::string single = run({"SPOP", "s"});
            TEST_CHECK(single.rfind("$1\r\n", 0) == 0);
            TEST_CHECK(run({"SCARD", "s"}) == ":2\r\n");

            // Every member is reachable by SRANDMEMBER without a count.
            {
                std::string pipeline;
                for(int i = 0; i < 400; i++) pipeline += test::request({"SRANDMEMBER", "s"});
                const std::string replies = test::exchange(server.port(), pipeline);
                for(const auto& member : {"a", "b", "c", "d", "e"}){
                    if(run({"SISMEMBER", "s", member}) == ":1\r\n"){
                        TEST_CHECK(replies.find("$1\r\n" + std::string(member) + "\r\n") !=
                                   std::string::npos);
                    }
                }
            }

            // Popping a large set one member at a time returns each member once
            // and empties it. The set's buckets are shrunk along the way.
            {
                constexpr int size = 5000;
                std::string fill;
                for(int i = 0; i < size; i++) fill += test::request({"SADD", "big-set", "m" + std::to_string(i)});
                test::exchange(server.port(), fill);
                TEST_CHECK(run({"SCARD", "big-set"}) == ":" + std::to_string(size) + "\r\n");
                std::string pops;
                for(int i = 0; i < size; i++) pops += test::request({"SPOP", "big-set"});
                const std::string replies = test::exchange(server.port(), pops);
                std::set<std::string> seen;
                std::size_t position = 0;
                while(position < replies.size()){
                    TEST_CHECK(replies[position] == '$');
                    const std::size_t end = replies.find("\r\n", position);
                    const std::size_t length = std::stoul(replies.substr(position + 1, end - position - 1));
                    TEST_CHECK(seen.insert(replies.substr(end + 2, length)).second);
                    position = end + 2 + length + 2;
                }
                TEST_CHECK(seen.size() == static_cast<std::size_t>(size));
                TEST_CHECK(run({"EXISTS", "big-set"}) == ":0\r\n");
            }

            // APPEND stops at the 64 KiB string limit.
            const std::string half(32 * 1024, 'x');
            TEST_CHECK(run({"APPEND", "big", half}) == ":32768\r\n");
            TEST_CHECK(run({"APPEND", "big", half}) == ":65536\r\n");
            TEST_CHECK(run({"APPEND", "big", "y"}) ==
                       "-ERR string exceeds maximum allowed size (proto-max-bulk-len)\r\n");
            TEST_CHECK(run({"STRLEN", "big"}) == ":65536\r\n");
        }

        // Every new write is logged and replayed. SIGTERM is not handled, so the
        // restart replays the AOF without a shutdown snapshot.
        test::TemporaryDirectory directory;
        std::vector<std::string> options{"--aof", directory.path() + "/data.aof"};
        options.insert(options.end(), extra.begin(), extra.end());
        {
            test::Server server(binary, options);
            auto run = [&](std::initializer_list<std::string_view> args){
                return test::exchange_request(server.port(), args);
            };
            TEST_CHECK(run({"APPEND", "str", "ab"}) == ":2\r\n");
            TEST_CHECK(run({"APPEND", "str", "c\0d"s}) == ":5\r\n");
            TEST_CHECK(run({"SETNX", "nx", "first"}) == ":1\r\n");
            TEST_CHECK(run({"SETNX", "nx", "second"}) == ":0\r\n");
            TEST_CHECK(run({"SET", "gs", "old", "EX", "1000"}) == "+OK\r\n");
            TEST_CHECK(run({"GETSET", "gs", "new"}) == "$3\r\nold\r\n");
            TEST_CHECK(run({"SET", "fl", "1.5", "EX", "1000"}) == "+OK\r\n");
            TEST_CHECK(run({"INCRBYFLOAT", "fl", "0.25"}) == "$4\r\n1.75\r\n");

            TEST_CHECK(run({"RPUSH", "l", "a", "b", "a", "c", "a"}) == ":5\r\n");
            TEST_CHECK(run({"LSET", "l", "1", "B"}) == "+OK\r\n");
            TEST_CHECK(run({"LREM", "l", "-1", "a"}) == ":1\r\n");
            TEST_CHECK(run({"LINSERT", "l", "AFTER", "c", "d"}) == ":5\r\n");
            TEST_CHECK(run({"LTRIM", "l", "1", "-1"}) == "+OK\r\n");
            TEST_CHECK(run({"RPUSH", "gone", "x"}) == ":1\r\n");
            TEST_CHECK(run({"LREM", "gone", "0", "x"}) == ":1\r\n");

            TEST_CHECK(run({"HMSET", "h", "a", "1", "b", "2"}) == "+OK\r\n");
            TEST_CHECK(run({"HSETNX", "h", "c", "3"}) == ":1\r\n");
            TEST_CHECK(run({"HINCRBY", "h", "a", "41"}) == ":42\r\n");
            TEST_CHECK(run({"HINCRBYFLOAT", "h", "b", "0.5"}) == "$3\r\n2.5\r\n");

            TEST_CHECK(run({"SADD", "x", "1", "2", "3"}) == ":3\r\n");
            TEST_CHECK(run({"SADD", "y", "2", "3", "4"}) == ":3\r\n");
            TEST_CHECK(run({"SINTERSTORE", "both", "x", "y"}) == ":2\r\n");
            TEST_CHECK(run({"SUNIONSTORE", "either", "x", "y"}) == ":4\r\n");
            TEST_CHECK(run({"SDIFFSTORE", "only", "x", "y"}) == ":1\r\n");
            TEST_CHECK(run({"SMOVE", "x", "y", "1"}) == ":1\r\n");
            TEST_CHECK(run({"SADD", "pop", "solo"}) == ":1\r\n");
            TEST_CHECK(run({"SPOP", "pop"}) == "$4\r\nsolo\r\n");
            server.stop();
        }

        test::Server server(binary, options);
        auto run = [&](std::initializer_list<std::string_view> args){
            return test::exchange_request(server.port(), args);
        };
        TEST_CHECK(run({"GET", "str"}) == "$5\r\nabc\0d\r\n"s);
        TEST_CHECK(run({"GET", "nx"}) == "$5\r\nfirst\r\n");
        TEST_CHECK(run({"GET", "gs"}) == "$3\r\nnew\r\n");
        TEST_CHECK(run({"TTL", "gs"}) == ":-1\r\n");// GETSET cleared the expiry
        TEST_CHECK(run({"GET", "fl"}) == "$4\r\n1.75\r\n");
        const std::string float_ttl = run({"TTL", "fl"});// INCRBYFLOAT kept it
        TEST_CHECK(float_ttl != ":-1\r\n" && float_ttl != ":-2\r\n");

        TEST_CHECK(array_items(run({"LRANGE", "l", "0", "-1"})) ==
                   (std::vector<std::string>{"B", "a", "c", "d"}));
        TEST_CHECK(run({"EXISTS", "gone"}) == ":0\r\n");

        TEST_CHECK(run({"HMGET", "h", "a", "b", "c"}) ==
                   "*3\r\n$2\r\n42\r\n$3\r\n2.5\r\n$1\r\n3\r\n");

        auto sorted = [&](std::initializer_list<std::string_view> args){
            auto items = array_items(run(args));
            std::sort(items.begin(), items.end());
            return items;
        };
        TEST_CHECK(sorted({"SMEMBERS", "both"}) == (std::vector<std::string>{"2", "3"}));
        TEST_CHECK(sorted({"SMEMBERS", "either"}) ==
                   (std::vector<std::string>{"1", "2", "3", "4"}));
        TEST_CHECK(sorted({"SMEMBERS", "only"}) == (std::vector<std::string>{"1"}));
        TEST_CHECK(sorted({"SMEMBERS", "x"}) == (std::vector<std::string>{"2", "3"}));
        TEST_CHECK(sorted({"SMEMBERS", "y"}) == (std::vector<std::string>{"1", "2", "3", "4"}));
        TEST_CHECK(run({"EXISTS", "pop"}) == ":0\r\n");
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
