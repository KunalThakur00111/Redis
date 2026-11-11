// Compare raw RESP2 replies for the admin commands (TYPE, KEYS, SCAN, RENAME,
// FLUSHDB/FLUSHALL, SELECT, COMMAND, CONFIG, INFO) with Redis Open Source 8.2.9.
// Both servers receive the same commands in the same order on one connection
// each. Every mismatch is printed, then the test fails if there was any.
#include "differential_support.h"

using differential::Case;
using differential::Compare;

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 3);
        const std::vector<Case> cases{
            {{"FLUSHALL"}},
            {{"SET", "s", "v"}}, {{"RPUSH", "l", "a"}}, {{"HSET", "h", "f", "v"}},
            {{"SADD", "z", "m"}},

            {{"TYPE", "s"}}, {{"type", "l"}}, {{"TYPE", "h"}}, {{"TYPE", "z"}},
            {{"TYPE", "missing"}}, {{"TYPE"}}, {{"TYPE", "a", "b"}},

            {{"KEYS", "*"}, Compare::sorted}, {{"KEYS", "[hl]"}, Compare::sorted},
            {{"KEYS", "nothing*"}}, {{"KEYS", "?"}, Compare::sorted}, {{"KEYS"}},
            {{"KEYS", "[^s]"}, Compare::sorted}, {{"KEYS", "\\s"}},

            {{"SCAN"}}, {{"SCAN", "abc"}}, {{"SCAN", "-1"}}, {{"SCAN", "+0"}, Compare::scan},
            {{"SCAN", "0", "COUNT", "0"}}, {{"SCAN", "0", "COUNT", "-1"}},
            {{"SCAN", "0", "COUNT", "x"}}, {{"SCAN", "0", "MATCH"}},
            {{"SCAN", "0", "BOGUS", "1"}}, {{"SCAN", "+"}}, {{"SCAN", "++0"}},
            {{"SCAN", "0", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "MATCH", "[hs]", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "string", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "LIST", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "bogus", "COUNT", "100"}, Compare::scan},

            {{"RENAME", "missing", "x"}}, {{"RENAME", "s", "s"}}, {{"RENAME", "s", "t"}},
            {{"GET", "t"}}, {{"EXISTS", "s"}}, {{"RENAMENX", "t", "l"}},
            {{"RENAMENX", "t", "t"}}, {{"RENAMENX", "missing", "x"}},
            {{"RENAMENX", "t", "u"}}, {{"RENAME", "u", "l"}}, {{"TYPE", "l"}},
            {{"RENAME", "a"}}, {{"RENAMENX", "a", "b", "c"}},
            {{"SET", "e", "v", "EX", "1000"}}, {{"RENAME", "e", "e2"}}, {{"TTL", "e2"}},

            {{"FLUSHALL", "now"}}, {{"FLUSHDB", "a", "b"}}, {{"FLUSHDB", "SYNC"}},
            {{"DBSIZE"}}, {{"SET", "k", "v"}}, {{"FLUSHALL", "async"}}, {{"DBSIZE"}},

            {{"SELECT", "0"}}, {{"SELECT", "x"}}, {{"SELECT", "-1"}}, {{"SELECT", "16"}},
            {{"SELECT"}}, {{"SELECT", "0", "1"}},

            {{"COMMAND", "bad\r\nx"}}, {{"COMMAND", "COUNT", "extra"}},
            {{"COMMAND", "LIST", "extra"}}, {{"COMMAND", "LIST", "FILTERBY"}},

            {{"CONFIG"}}, {{"CONFIG", "GET"}}, {{"CONFIG", "GET", "nope"}},
            {{"CONFIG", "GET", "appendonly"}}, {{"CONFIG", "GET", "APPENDONLY"}},
            {{"CONFIG", "GET", "maxmemory"}}, {{"CONFIG", "BAD"}},
            {{"CONFIG", "GET", "AppendOnly"}}, {{"CONFIG", "GET", "APPENDONL?"}},
            {{"CONFIG", "GET", "appendonly", "appendonly"}},

            {{"INFO", "nosuchsection"}},
        };
        return differential::run(argv[1], argv[2], cases, "admin command");
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
