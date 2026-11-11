// Compare raw RESP2 replies for the string, list, hash and set commands with
// Redis Open Source 8.2.9, including their error replies and the order in which
// arguments and key types are checked.
#include "differential_support.h"

using differential::Case;
using differential::Compare;

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 3);
        const std::vector<Case> cases{
            {{"FLUSHALL"}},
            {{"RPUSH", "list", "a", "b", "c", "b", "a", "b"}},
            {{"HSET", "hash", "f", "v", "n", "10", "x", "1.5"}},
            {{"SADD", "set", "a", "b", "c"}},
            {{"SET", "str", "hello"}},

            // APPEND, STRLEN, SETNX, GETSET
            {{"APPEND", "str", " world"}}, {{"GET", "str"}}, {{"APPEND", "new", "abc"}},
            {{"APPEND", "list", "x"}}, {{"APPEND", "str"}},
            {{"STRLEN", "str"}}, {{"STRLEN", "missing"}}, {{"STRLEN", "list"}}, {{"STRLEN"}},
            {{"SETNX", "str", "x"}}, {{"SETNX", "fresh", "x"}}, {{"GET", "fresh"}},
            {{"SETNX", "a"}},
            {{"SET", "ttl", "v", "EX", "1000"}}, {{"GETSET", "ttl", "w"}}, {{"TTL", "ttl"}},
            {{"GETSET", "missing2", "v"}}, {{"GETSET", "list", "v"}}, {{"GETSET", "a"}},

            // INCRBYFLOAT
            {{"SET", "f", "10.50"}}, {{"INCRBYFLOAT", "f", "0.1"}},
            {{"INCRBYFLOAT", "f", "-5"}}, {{"INCRBYFLOAT", "f", "5.0e3"}},
            {{"INCRBYFLOAT", "nf", "3"}}, {{"INCRBYFLOAT", "nf", "-3"}},
            {{"INCRBYFLOAT", "f", "abc"}}, {{"INCRBYFLOAT", "f", " 1"}},
            {{"INCRBYFLOAT", "f", "1 "}}, {{"INCRBYFLOAT", "f", "nan"}},
            {{"INCRBYFLOAT", "f", "inf"}}, {{"INCRBYFLOAT", "str", "1"}},
            {{"INCRBYFLOAT", "list", "1"}}, {{"INCRBYFLOAT", "f"}},
            {{"SET", "big", "1e308"}}, {{"INCRBYFLOAT", "big", "1e308"}},
            {{"SET", "ttlf", "1", "EX", "1000"}}, {{"INCRBYFLOAT", "ttlf", "1"}},
            {{"TTL", "ttlf"}, Compare::shape},

            // LSET
            {{"LSET", "list", "0", "A"}}, {{"LSET", "list", "-1", "Z"}},
            {{"LSET", "list", "99", "x"}}, {{"LSET", "list", "x", "y"}},
            {{"LSET", "missing", "0", "x"}}, {{"LSET", "missing", "x", "x"}},
            {{"LSET", "str", "0", "x"}}, {{"LSET", "str", "x", "x"}}, {{"LSET", "list", "0"}},
            {{"LRANGE", "list", "0", "-1"}},

            // LREM
            {{"RPUSH", "r", "a", "b", "a", "c", "a", "b", "a"}},
            {{"LREM", "r", "2", "a"}}, {{"LRANGE", "r", "0", "-1"}},
            {{"LREM", "r", "-1", "a"}}, {{"LRANGE", "r", "0", "-1"}},
            {{"LREM", "r", "0", "b"}}, {{"LRANGE", "r", "0", "-1"}},
            {{"LREM", "r", "0", "nothing"}}, {{"LREM", "missing", "0", "a"}},
            {{"LREM", "r", "x", "a"}}, {{"LREM", "missing", "x", "a"}},
            {{"LREM", "str", "0", "a"}}, {{"LREM", "str", "x", "a"}},
            {{"LREM", "r", "0", "a"}}, {{"LREM", "r", "0", "c"}}, {{"EXISTS", "r"}},

            // LTRIM
            {{"RPUSH", "t", "0", "1", "2", "3", "4", "5"}},
            {{"LTRIM", "t", "1", "-2"}}, {{"LRANGE", "t", "0", "-1"}},
            {{"LTRIM", "t", "-100", "100"}}, {{"LRANGE", "t", "0", "-1"}},
            {{"LTRIM", "t", "2", "1"}}, {{"EXISTS", "t"}},
            {{"LTRIM", "missing", "0", "1"}}, {{"LTRIM", "str", "0", "1"}},
            {{"LTRIM", "str", "x", "1"}}, {{"LTRIM", "list", "0", "y"}},

            // LINSERT
            {{"RPUSH", "ins", "a", "c"}},
            {{"LINSERT", "ins", "BEFORE", "c", "b"}}, {{"LINSERT", "ins", "after", "c", "d"}},
            {{"LRANGE", "ins", "0", "-1"}}, {{"LINSERT", "ins", "BEFORE", "zz", "x"}},
            {{"LINSERT", "missing", "BEFORE", "a", "x"}}, {{"LINSERT", "ins", "MIDDLE", "a", "x"}},
            {{"LINSERT", "missing", "MIDDLE", "a", "x"}}, {{"LINSERT", "str", "BEFORE", "a", "x"}},
            {{"LINSERT", "str", "MIDDLE", "a", "x"}}, {{"LINSERT", "ins", "BEFORE", "a"}},

            // HMSET, HSETNX, HGETALL, HKEYS, HVALS, HMGET, HEXISTS, HSTRLEN
            {{"HMSET", "h2", "a", "1", "b", "2"}}, {{"HMSET", "h2", "a"}},
            {{"HMSET", "str", "a", "1"}},
            {{"HGETALL", "h2"}, Compare::pairs}, {{"HGETALL", "missing"}},
            {{"HGETALL", "str"}}, {{"HGETALL"}},
            {{"HKEYS", "h2"}, Compare::sorted}, {{"HVALS", "h2"}, Compare::sorted},
            {{"HKEYS", "missing"}}, {{"HVALS", "str"}},
            {{"HSETNX", "h2", "a", "9"}}, {{"HSETNX", "h2", "c", "3"}}, {{"HSETNX", "h3", "a", "1"}},
            {{"HSETNX", "str", "a", "1"}}, {{"HGET", "h2", "a"}},
            {{"HMGET", "h2", "a", "nope", "c"}}, {{"HMGET", "missing", "a", "b"}},
            {{"HMGET", "str", "a"}}, {{"HMGET", "h2"}},
            {{"HEXISTS", "h2", "a"}}, {{"HEXISTS", "h2", "z"}}, {{"HEXISTS", "missing", "a"}},
            {{"HEXISTS", "str", "a"}},
            {{"HSTRLEN", "h2", "a"}}, {{"HSTRLEN", "h2", "z"}}, {{"HSTRLEN", "str", "a"}},

            // HINCRBY, HINCRBYFLOAT
            {{"HINCRBY", "hash", "n", "5"}}, {{"HINCRBY", "hash", "n", "-20"}},
            {{"HINCRBY", "hash", "new", "3"}}, {{"HINCRBY", "hnew", "f", "1"}},
            {{"HINCRBY", "hash", "f", "1"}}, {{"HINCRBY", "hash", "n", "x"}},
            {{"HINCRBY", "str", "n", "1"}}, {{"HINCRBY", "str", "n", "x"}},
            {{"HSET", "hash", "max", "9223372036854775807"}},
            {{"HINCRBY", "hash", "max", "1"}}, {{"HINCRBY", "hash", "n", "+1"}},
            {{"HINCRBYFLOAT", "hash", "x", "0.25"}}, {{"HINCRBYFLOAT", "hash", "nx", "1e2"}},
            {{"HINCRBYFLOAT", "hash", "f", "1"}}, {{"HINCRBYFLOAT", "hash", "x", "y"}},
            {{"HINCRBYFLOAT", "str", "x", "1"}}, {{"HINCRBYFLOAT", "str", "x", "y"}},
            {{"HINCRBYFLOAT", "hash", "x", "inf"}}, {{"HINCRBYFLOAT", "str", "x", "-inf"}},
            {{"HSET", "hash", "huge", "1e4900"}}, {{"HINCRBYFLOAT", "hash", "huge", "1e4900"}},

            // SMISMEMBER, SINTER, SUNION, SDIFF and their STORE forms
            {{"SADD", "s2", "b", "c", "d"}}, {{"SADD", "s3", "c", "x"}},
            {{"SMISMEMBER", "set", "a", "z", "c"}}, {{"SMISMEMBER", "missing", "a"}},
            {{"SMISMEMBER", "str", "a"}}, {{"SMISMEMBER", "set"}},
            {{"SINTER", "set", "s2"}, Compare::sorted}, {{"SINTER", "set", "s2", "s3"}},
            {{"SINTER", "set", "missing"}}, {{"SINTER", "missing", "str"}},
            {{"SUNION", "set", "s2", "missing"}, Compare::sorted}, {{"SUNION", "set", "str"}},
            {{"SDIFF", "set", "s2"}, Compare::sorted}, {{"SDIFF", "missing", "set"}},
            {{"SDIFF", "set", "str"}}, {{"SINTER"}},
            {{"SINTERSTORE", "dest", "set", "s2"}}, {{"SMEMBERS", "dest"}, Compare::sorted},
            {{"SUNIONSTORE", "dest", "set", "s2"}}, {{"SCARD", "dest"}},
            {{"SDIFFSTORE", "dest", "set", "set"}}, {{"EXISTS", "dest"}},
            {{"SINTERSTORE", "str", "set", "s2"}}, {{"TYPE", "str"}},
            {{"SUNIONSTORE", "set", "set", "s3"}}, {{"SCARD", "set"}},
            {{"SINTERSTORE", "dest", "set", "list"}}, {{"SINTERSTORE", "dest"}},

            // SPOP, SRANDMEMBER (random, so only deterministic cases are exact)
            {{"SADD", "one", "only"}}, {{"SPOP", "one"}}, {{"EXISTS", "one"}},
            {{"SPOP", "missing"}}, {{"SPOP", "missing", "2"}}, {{"SPOP", "list"}},
            {{"SPOP", "set", "-1"}}, {{"SPOP", "set", "x"}}, {{"SPOP", "missing", "-1"}},
            {{"SPOP", "set", "1", "2"}}, {{"SPOP"}}, {{"SPOP", "set", "0"}},
            {{"SPOP", "s3", "1"}, Compare::shape},
            {{"SADD", "two", "p", "q"}}, {{"SPOP", "two", "5"}, Compare::sorted},
            {{"EXISTS", "two"}},
            {{"SADD", "three", "p", "q", "r"}},
            {{"SRANDMEMBER", "three", "10"}, Compare::sorted},
            {{"SRANDMEMBER", "three", "0"}}, {{"SRANDMEMBER", "three"}, Compare::shape},
            {{"SRANDMEMBER", "missing"}}, {{"SRANDMEMBER", "missing", "3"}},
            {{"SRANDMEMBER", "missing", "-3"}}, {{"SRANDMEMBER", "list"}},
            {{"SRANDMEMBER", "three", "x"}}, {{"SRANDMEMBER", "three", "1", "2"}},
            {{"SRANDMEMBER", "one", "-3"}}, {{"SCARD", "three"}},

            // SMOVE
            {{"SADD", "from", "m", "n"}}, {{"SMOVE", "from", "to", "m"}},
            {{"SMEMBERS", "to"}}, {{"SMOVE", "from", "to", "zz"}},
            {{"SMOVE", "missing", "to", "m"}}, {{"SMOVE", "from", "from", "n"}},
            {{"SMOVE", "from", "str", "n"}}, {{"SMOVE", "str", "to", "n"}},
            {{"SMOVE", "from", "to", "n"}}, {{"EXISTS", "from"}}, {{"SCARD", "to"}},
            {{"SMOVE", "from", "to"}},
        };
        return differential::run(argv[1], argv[2], cases, "type command");
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
