// Small end-to-end test of the server over a real TCP connection.
#include "test_support.h"

#include <algorithm>
#include <iostream>

using namespace std::string_literals;

namespace {
bool positive_ttl(const std::string& reply) {
    return reply.size() > 3 && reply.front() == ':' &&
           reply.substr(reply.size() - 2) == "\r\n" &&
           std::stoll(reply.substr(1)) > 0;
}

std::vector<std::string> sorted_members(const std::string& reply) {
    TEST_CHECK(!reply.empty() && reply.front() == '*');
    std::size_t end = reply.find("\r\n");
    TEST_CHECK(end != std::string::npos);
    int count = std::stoi(reply.substr(1, end - 1));
    TEST_CHECK(count >= 0);
    std::size_t position = end + 2;
    std::vector<std::string> members;
    for(int i = 0; i < count; i++){
        TEST_CHECK(position < reply.size() && reply[position] == '$');
        end = reply.find("\r\n", position);
        TEST_CHECK(end != std::string::npos);
        int length = std::stoi(reply.substr(position + 1, end - position - 1));
        TEST_CHECK(length >= 0);
        position = end + 2;
        TEST_CHECK(position + static_cast<std::size_t>(length) + 2 <= reply.size());
        members.push_back(reply.substr(position, static_cast<std::size_t>(length)));
        position += static_cast<std::size_t>(length);
        TEST_CHECK(reply.substr(position, 2) == "\r\n");
        position += 2;
    }
    TEST_CHECK(position == reply.size());
    std::sort(members.begin(), members.end());
    return members;
}

std::size_t count_lines(const std::string& reply) {
    std::size_t count = 0;
    for(std::size_t position = 0; (position = reply.find("\r\n", position)) != std::string::npos; position += 2)
        count++;
    return count;
}
}

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        std::vector<std::string> options(argv + 2, argv + argc);
        test::Server server(argv[1], options);
        std::uint16_t port = server.port();
        auto exchange = [&](std::initializer_list<std::string_view> arguments){
            return test::exchange_request(port, arguments);
        };

        TEST_CHECK(exchange({"PING"}) == "+PONG\r\n");
        TEST_CHECK(exchange({"ping", "a\000b"s}) == "$3\r\na\000b\r\n"s);
        std::string echo = test::request({"ECHO", "a\r\nb"});
        {
            test::Fd connection = test::connect_to(port);
            test::send_all(connection.get(), std::string_view(echo).substr(0, 7));
            test::send_all(connection.get(), std::string_view(echo).substr(7));
            TEST_CHECK(shutdown(connection.get(), SHUT_WR) == 0);
            TEST_CHECK(test::read_to_eof(connection.get()) == "$4\r\na\r\nb\r\n");
        }

        // One client's incomplete request must not block another ready client.
        {
            test::Fd first = test::connect_to(port);
            test::Fd second = test::connect_to(port);
            std::string ping = test::request({"PING"});
            test::send_all(first.get(), std::string_view(ping).substr(0, 5));
            test::send_all(second.get(), ping);
            TEST_CHECK(test::read_exact(second.get(), 7) == "+PONG\r\n");
            test::send_all(first.get(), std::string_view(ping).substr(5));
            TEST_CHECK(test::read_exact(first.get(), 7) == "+PONG\r\n");
        }

        std::string pipeline = test::request({"SET", "key", "first"}) +
                               test::request({"SET", "empty", ""}) +
                               test::request({"SET", "key", "second"}) +
                               test::request({"DBSIZE"});
        TEST_CHECK(test::exchange(port, pipeline) == "+OK\r\n+OK\r\n+OK\r\n:2\r\n");

        const std::string malformed = "*1\r\n$x\r\nPING\r\n";
        for(const auto& entry : std::vector<std::pair<std::string, std::string>>{
                {malformed, "-ERR Protocol error: "},
                {test::request({"PING"}) + malformed, "+PONG\r\n-ERR Protocol error: "}}){
            test::Fd connection = test::connect_to(port);
            test::send_all(connection.get(), entry.first);
            std::string reply = test::read_to_eof(connection.get());
            TEST_CHECK(reply.find(entry.second) == 0);
            TEST_CHECK(reply.size() >= 2 && reply.substr(reply.size() - 2) == "\r\n");
            TEST_CHECK(count_lines(reply) == (entry.first == malformed ? 1U : 2U));
        }

        const std::string expected = "*4\r\n$6\r\nsecond\r\n$-1\r\n$0\r\n\r\n$6\r\nsecond\r\n";
        const std::string large(65536, 'x');
        const std::string minimum = "-9223372036854775808";
        TEST_CHECK(exchange({"mget"s, "key"s, "missing"s, "empty"s, "key"s}) == expected);
        TEST_CHECK(exchange({"DBSIZE"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "key"s, "key"s, "missing"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"DEL"s, "key"s, "key"s, "missing"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"GET"s, "key"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"MGET"s}) == "-ERR wrong number of arguments for 'mget' command\r\n"s);
        TEST_CHECK(exchange({"MSET"s, "first"s, "one"s, "second"s, "a\000b"s, "first"s, "updated"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"MGET"s, "first"s, "second"s}) == "*2\r\n$7\r\nupdated\r\n$3\r\na\000b\r\n"s);
        TEST_CHECK(exchange({"MSET"s, "orphan"s}) == "-ERR wrong number of arguments for 'mset' command\r\n"s);
        TEST_CHECK(exchange({"MSET"s, "untouched"s, "value"s, "orphan"s}) == "-ERR wrong number of arguments for 'mset' command\r\n"s);
        TEST_CHECK(exchange({"GET"s, "untouched"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"GET"s, "orphan"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"SET"s, "large"s, large}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"GET"s, "large"s}) == (("$65536\r\n"s + large) + "\r\n"s));
        TEST_CHECK(exchange({"INCR"s, "counter"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "counter"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "missing-counter"s}) == ":-1\r\n"s);
        TEST_CHECK(exchange({"DECR"s}) == "-ERR wrong number of arguments for 'decr' command\r\n"s);
        TEST_CHECK(exchange({"SET"s, "signed"s, "+7"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCR"s, "signed"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "signed"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "signed"s}) == "$2\r\n+7\r\n"s);
        TEST_CHECK(exchange({"SET"s, "negative"s, "-7"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCR"s, "negative"s}) == ":-6\r\n"s);
        TEST_CHECK(exchange({"SET"s, "leading-zero"s, "01"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCR"s, "leading-zero"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "leading-zero"s}) == "$2\r\n01\r\n"s);
        TEST_CHECK(exchange({"SET"s, "negative-zero"s, "-0"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "negative-zero"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "negative-zero"s}) == "$2\r\n-0\r\n"s);
        TEST_CHECK(exchange({"SET"s, "minimum"s, minimum}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "minimum"s}) == "-ERR increment or decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "minimum"s}) == "$20\r\n-9223372036854775808\r\n"s);
        TEST_CHECK(exchange({"SET"s, "ttl-counter"s, "5"s, "PX"s, "60000"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECR"s, "ttl-counter"s}) == ":4\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "ttl-counter"})));
        TEST_CHECK(exchange({"INCRBY"s, "incrby-missing"s, "5"s}) == ":5\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-missing"s, "-2"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"GET"s, "incrby-missing"s}) == "$1\r\n3\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-negative"s, "-2"s}) == ":-2\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-missing"s, "+7"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "incrby-missing"s}) == "$1\r\n3\r\n"s);
        TEST_CHECK(exchange({"SET"s, "incrby-bad"s, "abc"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-bad"s, "1"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "incrby-bad"s}) == "$3\r\nabc\r\n"s);
        TEST_CHECK(exchange({"SET"s, "incrby-max"s, "9223372036854775807"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-max"s, "1"s}) == "-ERR increment or decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "incrby-max"s}) == "$19\r\n9223372036854775807\r\n"s);
        TEST_CHECK(exchange({"SET"s, "incrby-min"s, minimum}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-min"s, "-1"s}) == "-ERR increment or decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "incrby-min"s}) == "$20\r\n-9223372036854775808\r\n"s);
        TEST_CHECK(exchange({"SET"s, "incrby-ttl"s, "5"s, "PX"s, "60000"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"INCRBY"s, "incrby-ttl"s, "2"s}) == ":7\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "incrby-ttl"})));
        TEST_CHECK(exchange({"INCRBY"s, "incrby-ttl"s}) == "-ERR wrong number of arguments for 'incrby' command\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-missing"s, "3"s}) == ":-3\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-negative"s, "-3"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-negative"s}) == "$1\r\n3\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-negative"s, "-3"s}) == ":6\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-min-amount"s, minimum}) == "-ERR decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-min-amount"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-negative"s, minimum}) == "-ERR decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-negative"s}) == "$1\r\n6\r\n"s);
        TEST_CHECK(exchange({"SET"s, "decrby-min"s, minimum}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-min"s, "1"s}) == "-ERR increment or decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-min"s}) == "$20\r\n-9223372036854775808\r\n"s);
        TEST_CHECK(exchange({"SET"s, "decrby-max"s, "9223372036854775807"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-max"s, "-1"s}) == "-ERR increment or decrement would overflow\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-max"s}) == "$19\r\n9223372036854775807\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-negative"s, "+7"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"SET"s, "decrby-bad"s, "abc"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-bad"s, "1"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "decrby-bad"s}) == "$3\r\nabc\r\n"s);
        TEST_CHECK(exchange({"SET"s, "decrby-ttl"s, "5"s, "PX"s, "60000"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"DECRBY"s, "decrby-ttl"s, "2"s}) == ":3\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "decrby-ttl"})));
        TEST_CHECK(exchange({"DECRBY"s, "decrby-ttl"s}) == "-ERR wrong number of arguments for 'decrby' command\r\n"s);
        TEST_CHECK(exchange({"LPUSH"s, "list-items"s, "a"s, "b"s, "c"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"LPUSH"s, "list-items"s, "d"s}) == ":4\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "list-items"s, "0"s, "-1"s}) == "*4\r\n$1\r\nd\r\n$1\r\nc\r\n$1\r\nb\r\n$1\r\na\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "list-items"s, "-2"s, "-1"s}) == "*2\r\n$1\r\nb\r\n$1\r\na\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "list-items"s, "10"s, "20"s}) == "*0\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "missing-list"s, "0"s, "-1"s}) == "*0\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "list-items"s, "01"s, "-1"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"GET"s, "list-items"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"MGET"s, "list-items"s, "missing"s}) == "*2\r\n$-1\r\n$-1\r\n"s);
        TEST_CHECK(exchange({"GETDEL"s, "list-items"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "list-items"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"SET"s, "string-value"s, "original"s}) == "+OK\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "string-value"s, "0"s, "-1"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"LPUSH"s, "string-value"s, "x"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"GET"s, "string-value"s}) == "$8\r\noriginal\r\n"s);
        TEST_CHECK(exchange({"LPUSH"s, "list-items"s}) == "-ERR wrong number of arguments for 'lpush' command\r\n"s);
        TEST_CHECK(exchange({"RPUSH"s, "queue"s, "a"s, "b"s, "c"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"LRANGE"s, "queue"s, "0"s, "-1"s}) == "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"s);
        TEST_CHECK(exchange({"LINDEX"s, "queue"s, "0"s}) == "$1\r\na\r\n"s);
        TEST_CHECK(exchange({"LINDEX"s, "queue"s, "-1"s}) == "$1\r\nc\r\n"s);
        TEST_CHECK(exchange({"LINDEX"s, "queue"s, "3"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"LINDEX"s, "missing-list"s, "0"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"LLEN"s, "queue"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"LLEN"s, "missing-list"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "queue"s}) == "$1\r\na\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "queue"s, "0"s}) == "*0\r\n"s);
        TEST_CHECK(exchange({"LLEN"s, "queue"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"RPOP"s, "queue"s, "2"s}) == "*2\r\n$1\r\nc\r\n$1\r\nb\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "queue"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"RPOP"s, "queue"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "queue"s, "2"s}) == "*-1\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "queue"s, "-1"s}) == "-ERR value is out of range, must be positive\r\n"s);
        TEST_CHECK(exchange({"RPOP"s, "queue"s, "bad"s}) == "-ERR value is not an integer or out of range\r\n"s);
        TEST_CHECK(exchange({"RPUSH"s, "string-value"s, "x"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "string-value"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"RPOP"s, "string-value"s, "0"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"LLEN"s, "string-value"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"LINDEX"s, "string-value"s, "0"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"RPUSH"s, "expiring-list"s, "a"s, "b"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"PEXPIRE"s, "expiring-list"s, "60000"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"LPOP"s, "expiring-list"s}) == "$1\r\na\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "expiring-list"})));
        TEST_CHECK(exchange({"RPOP"s, "expiring-list"s}) == "$1\r\nb\r\n"s);
        TEST_CHECK(exchange({"PTTL"s, "expiring-list"s}) == ":-2\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "profile"s, "name"s, "Kunal"s, "place"s, "Delhi"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "profile"s, "name"s, "Sam"s, "age"s, "21"s, "age"s, "22"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "profile"s, "name"s}) == "$3\r\nSam\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "profile"s, "age"s}) == "$2\r\n22\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "profile"s, "missing"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"HLEN"s, "profile"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"HDEL"s, "profile"s, "name"s, "name"s, "missing"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"HLEN"s, "profile"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"HDEL"s, "profile"s, "place"s, "age"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "profile"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "profile"s, "name"s}) == "$-1\r\n"s);
        TEST_CHECK(exchange({"HLEN"s, "profile"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "broken-hash"s, "field"s}) == "-ERR wrong number of arguments for 'hset' command\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "broken-hash"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "string-value"s, "field"s, "value"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "list-items"s, "field"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"HDEL"s, "list-items"s, "field"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"HLEN"s, "list-items"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "binary-hash"s, "f\000"s, "a\000b"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"HGET"s, "binary-hash"s, "f\000"s}) == "$3\r\na\000b\r\n"s);
        TEST_CHECK(exchange({"HDEL"s, "binary-hash"s, "f\000"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "binary-hash"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "expiring-hash"s, "first"s, "a"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"PEXPIRE"s, "expiring-hash"s, "60000"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"HSET"s, "expiring-hash"s, "second"s, "b"s}) == ":1\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "expiring-hash"})));
        TEST_CHECK(exchange({"HDEL"s, "expiring-hash"s, "first"s, "second"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"PTTL"s, "expiring-hash"s}) == ":-2\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "members"s, "alice"s, "bob"s, "alice"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "members"s, "bob"s, "a\000b"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"SCARD"s, "members"s}) == ":3\r\n"s);
        TEST_CHECK(exchange({"SISMEMBER"s, "members"s, "alice"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"SISMEMBER"s, "members"s, "missing"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"SISMEMBER"s, "members"s, "a\000b"s}) == ":1\r\n"s);
        TEST_CHECK(sorted_members(exchange({"SMEMBERS", "members"})) == std::vector<std::string>({"a\000b"s, "alice"s, "bob"s}));
        TEST_CHECK(exchange({"SREM"s, "members"s, "bob"s, "bob"s, "missing"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"SCARD"s, "members"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"SREM"s, "members"s, "alice"s, "a\000b"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "members"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"SCARD"s, "members"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"SISMEMBER"s, "members"s, "alice"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"SMEMBERS"s, "members"s}) == "*0\r\n"s);
        TEST_CHECK(exchange({"SREM"s, "members"s, "alice"s}) == ":0\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "expiring-set"s, "first"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"PEXPIRE"s, "expiring-set"s, "60000"s}) == ":1\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "expiring-set"s, "second"s}) == ":1\r\n"s);
        TEST_CHECK(positive_ttl(exchange({"PTTL", "expiring-set"})));
        TEST_CHECK(exchange({"SREM"s, "expiring-set"s, "first"s, "second"s}) == ":2\r\n"s);
        TEST_CHECK(exchange({"PTTL"s, "expiring-set"s}) == ":-2\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "string-value"s, "x"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"SREM"s, "list-items"s, "x"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"SISMEMBER"s, "list-items"s, "x"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"SCARD"s, "list-items"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"SMEMBERS"s, "list-items"s}) == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"s);
        TEST_CHECK(exchange({"SADD"s, "bad-set"s}) == "-ERR wrong number of arguments for 'sadd' command\r\n"s);
        TEST_CHECK(exchange({"SREM"s, "bad-set"s}) == "-ERR wrong number of arguments for 'srem' command\r\n"s);
        TEST_CHECK(exchange({"EXISTS"s, "bad-set"s}) == ":0\r\n"s);

        std::cout << "Socket command tests passed\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}

