// Compare raw RESP2 replies for the sorted-set commands with Redis Open Source
// 8.2.9: score formatting, every ZADD flag combination, the ZRANGE option
// rules, range parsing, and the order arguments and key types are checked.
#include "differential_support.h"

using differential::Case;
using differential::Compare;

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 3);
        const std::vector<Case> cases{
            {{"FLUSHALL"}},
            {{"SET", "str", "v"}},

            // Score formatting: integers, shortest decimals, exponents, -0, inf.
            {{"ZADD", "fmt", "1", "int", "1.5", "half", "0.1", "tenth", "-0", "negzero",
              "1e20", "big", "1e-5", "small", "0.0001", "smallish", "3.14159", "pi",
              "123456789012345678", "huge-int", "5e18", "over-half-max", "1e16", "e16",
              "1e17", "e17", "100.25", "hundred", "-2.5", "neg", "inf", "posinf",
              "-inf", "neginf", "0.3333333333333333", "third", "1.7976931348623157e308", "max"}},
            {{"ZRANGE", "fmt", "0", "-1", "WITHSCORES"}},
            {{"ZSCORE", "fmt", "negzero"}}, {{"ZSCORE", "fmt", "big"}}, {{"ZSCORE", "fmt", "small"}},
            {{"ZINCRBY", "fmt", "0.2", "tenth"}}, {{"ZINCRBY", "fmt", "1e300", "max"}},
            {{"ZADD", "fmt2", "1e-7", "a", "2.5e-8", "b", "1e-100", "c", "1.5e-300", "d",
              "1e100", "e", "1.2345678901234568e21", "f", "-1e-6", "g", "1234.5e-10", "h",
              "9.87654321e-5", "i", "6e18", "j", "-7.5e25", "k", "0.000001", "l"}},
            {{"ZRANGE", "fmt2", "0", "-1", "WITHSCORES"}},
            {{"ZADD", "parse", "0x10", "hex"}}, {{"ZADD", "parse", "5.", "dot"}},
            {{"ZADD", "parse", ".5", "lead"}}, {{"ZADD", "parse", "infinity", "word"}},
            {{"ZADD", "parse", "+1", "plus"}}, {{"ZADD", "parse", "1_0", "under"}},
            {{"ZADD", "parse", "-inf", "ninf", "+inf", "pinf"}},
            {{"ZRANGE", "parse", "0", "-1", "WITHSCORES"}},
            {{"ZRANGE", "parse", "0x0", "10", "BYSCORE"}}, {{"ZRANGE", "parse", "-0X1", "10", "BYSCORE"}}, {{"ZCOUNT", "parse", "(0x0", "+inf"}}, {{"ZRANGE", "parse", "(5.", "+inf", "BYSCORE"}},
            {{"ZRANGE", "parse", "-infinity", "( 1", "BYSCORE"}}, {{"ZRANGE", "parse", "(", "1", "BYSCORE"}},

            // ZADD basics and errors.
            {{"ZADD", "z", "1", "a", "2", "b", "3", "c"}}, {{"ZADD", "z", "1", "a"}},
            {{"ZADD", "z", "5", "a"}}, {{"ZADD", "z", "CH", "6", "a", "4", "d"}},
            {{"ZADD", "z"}}, {{"ZADD", "z", "1"}}, {{"ZADD", "z", "1", "a", "2"}},
            {{"ZADD", "z", "NX"}}, {{"ZADD", "z", "x", "a"}}, {{"ZADD", "z", "nan", "a"}},
            {{"ZADD", "z", " 1", "a"}}, {{"ZADD", "z", "1 ", "a"}}, {{"ZADD", "z", "1e999", "a"}},
            {{"ZADD", "z", "1", "a", "x", "b"}}, {{"ZSCORE", "z", "a"}},
            {{"ZADD", "str", "1", "a"}}, {{"ZADD", "str", "x", "a"}},
            {{"ZADD", "z", "NX", "XX", "1", "a"}}, {{"ZADD", "z", "GT", "LT", "1", "a"}},
            {{"ZADD", "z", "NX", "GT", "1", "a"}}, {{"ZADD", "z", "NX", "XX", "1"}},
            {{"ZADD", "z", "INCR", "1", "a", "2", "b"}}, {{"ZADD", "z", "NX", "XX", "INCR", "1", "a", "2", "b"}},

            // NX, XX, GT, LT, CH, INCR.
            {{"ZADD", "z", "NX", "100", "a", "7", "e"}}, {{"ZSCORE", "z", "a"}},
            {{"ZADD", "z", "XX", "8", "a", "9", "new"}}, {{"ZSCORE", "z", "new"}},
            {{"ZADD", "z", "XX", "CH", "9", "a"}}, {{"ZADD", "z", "GT", "CH", "1", "a", "10", "b"}},
            {{"ZADD", "z", "LT", "CH", "100", "a", "0", "c"}}, {{"ZADD", "z", "GT", "5", "fresh"}},
            {{"ZADD", "z", "XX", "GT", "CH", "50", "a"}}, {{"ZADD", "z", "INCR", "5", "a"}},
            {{"ZADD", "z", "NX", "INCR", "5", "a"}}, {{"ZADD", "z", "XX", "INCR", "5", "nope"}},
            {{"ZADD", "z", "GT", "INCR", "-1", "a"}}, {{"ZADD", "z", "LT", "INCR", "-1", "a"}},
            {{"ZADD", "missing", "XX", "1", "a"}}, {{"ZADD", "missing", "XX", "INCR", "1", "a"}},
            {{"EXISTS", "missing"}}, {{"zadd", "z", "nx", "ch", "1", "lower"}},
            {{"ZADD", "inc", "inf", "m"}}, {{"ZADD", "inc", "INCR", "-inf", "m"}}, {{"ZSCORE", "inc", "m"}},
            {{"ZINCRBY", "inc", "-inf", "m"}}, {{"ZINCRBY", "inc", "x", "m"}},
            {{"ZINCRBY", "str", "1", "m"}}, {{"ZINCRBY", "inc", "1"}}, {{"ZINCRBY", "newz", "2.5", "m"}},
            {{"ZRANGE", "z", "0", "-1", "WITHSCORES"}},

            // ZSCORE, ZMSCORE, ZCARD, ZREM
            {{"ZSCORE", "z", "nope"}}, {{"ZSCORE", "missing", "a"}}, {{"ZSCORE", "str", "a"}},
            {{"ZSCORE", "z"}}, {{"ZMSCORE", "z", "a", "nope", "b"}}, {{"ZMSCORE", "missing", "a"}},
            {{"ZMSCORE", "str", "a"}}, {{"ZMSCORE", "z"}},
            {{"ZCARD", "z"}}, {{"ZCARD", "missing"}}, {{"ZCARD", "str"}}, {{"ZCARD"}},
            {{"ZADD", "rem", "1", "a", "2", "b"}}, {{"ZREM", "rem", "a", "x", "a"}},
            {{"ZREM", "missing", "a"}}, {{"ZREM", "str", "a"}}, {{"ZREM", "rem"}},
            {{"ZREM", "rem", "b"}}, {{"EXISTS", "rem"}},

            // ZRANK, ZREVRANK
            {{"ZADD", "r", "1", "a", "2", "b", "2", "c", "3", "d"}},
            {{"ZRANK", "r", "a"}}, {{"ZRANK", "r", "c"}}, {{"ZREVRANK", "r", "a"}},
            {{"ZRANK", "r", "nope"}}, {{"ZRANK", "missing", "a"}}, {{"ZRANK", "str", "a"}},
            {{"ZRANK", "r", "b", "WITHSCORE"}}, {{"ZREVRANK", "r", "d", "withscore"}},
            {{"ZRANK", "r", "nope", "WITHSCORE"}}, {{"ZRANK", "missing", "a", "WITHSCORE"}},
            {{"ZRANK", "r", "a", "BAD"}}, {{"ZRANK", "r", "a", "WITHSCORE", "x"}}, {{"ZRANK", "r"}},

            // ZRANGE by rank
            {{"ZRANGE", "r", "0", "-1"}}, {{"ZRANGE", "r", "1", "2", "WITHSCORES"}},
            {{"ZRANGE", "r", "-2", "-1"}}, {{"ZRANGE", "r", "5", "10"}}, {{"ZRANGE", "r", "2", "1"}},
            {{"ZRANGE", "r", "-100", "100"}}, {{"ZRANGE", "r", "0", "-1", "REV"}},
            {{"ZRANGE", "r", "0", "1", "REV", "WITHSCORES"}}, {{"ZREVRANGE", "r", "0", "-1"}},
            {{"ZREVRANGE", "r", "1", "1", "WITHSCORES"}}, {{"ZRANGE", "r", "x", "1"}},
            {{"ZRANGE", "missing", "0", "-1"}}, {{"ZRANGE", "str", "0", "-1"}},
            {{"ZRANGE", "str", "x", "1"}}, {{"ZRANGE", "r", "0"}},
            {{"ZRANGE", "r", "0", "1", "LIMIT", "0", "1"}}, {{"ZRANGE", "r", "0", "1", "LIMIT", "0", "-1"}},
            {{"ZRANGE", "r", "0", "1", "BOGUS"}}, {{"ZRANGE", "r", "0", "1", "REV", "REV"}},
            {{"ZRANGE", "r", "0", "1", "LIMIT", "0"}}, {{"ZREVRANGE", "r", "0", "1", "REV"}},

            // ZRANGE BYSCORE and ZRANGEBYSCORE
            {{"ZRANGE", "r", "2", "3", "BYSCORE"}}, {{"ZRANGE", "r", "(1", "(3", "BYSCORE"}},
            {{"ZRANGE", "r", "-inf", "+inf", "BYSCORE", "WITHSCORES"}},
            {{"ZRANGE", "r", "3", "1", "BYSCORE", "REV"}}, {{"ZRANGE", "r", "1", "3", "BYSCORE", "REV"}},
            {{"ZRANGE", "r", "-inf", "inf", "BYSCORE", "LIMIT", "1", "2"}},
            {{"ZRANGE", "r", "-inf", "inf", "BYSCORE", "LIMIT", "1", "-1"}},
            {{"ZRANGE", "r", "-inf", "inf", "BYSCORE", "LIMIT", "-1", "2"}},
            {{"ZRANGE", "r", "-inf", "inf", "BYSCORE", "LIMIT", "10", "2"}},
            {{"ZRANGE", "r", "-inf", "inf", "BYSCORE", "REV", "LIMIT", "1", "2", "WITHSCORES"}},
            {{"ZRANGE", "r", "x", "3", "BYSCORE"}}, {{"ZRANGE", "r", "(x", "3", "BYSCORE"}},
            {{"ZRANGE", "r", "2", "2", "BYSCORE"}}, {{"ZRANGE", "r", "(2", "2", "BYSCORE"}},
            {{"ZRANGE", "r", " 2", "2", "BYSCORE"}}, {{"ZRANGE", "r", "", "2", "BYSCORE"}},
            {{"ZRANGE", "r", "nan", "2", "BYSCORE"}}, {{"ZRANGE", "str", "x", "1", "BYSCORE"}},
            {{"ZRANGE", "r", "0", "1", "BYSCORE", "BYLEX"}}, {{"ZRANGE", "r", "1", "3", "BYSCORE", "LIMIT", "x", "1"}},
            {{"ZRANGEBYSCORE", "r", "1", "2"}}, {{"ZRANGEBYSCORE", "r", "1", "2", "WITHSCORES"}},
            {{"ZRANGEBYSCORE", "r", "-inf", "+inf", "LIMIT", "1", "1"}},
            {{"ZREVRANGEBYSCORE", "r", "3", "2"}}, {{"ZREVRANGEBYSCORE", "r", "+inf", "-inf", "WITHSCORES", "LIMIT", "0", "2"}},
            {{"ZRANGEBYSCORE", "r", "1", "2", "BYSCORE"}}, {{"ZRANGEBYSCORE", "r", "1", "2", "REV"}},
            {{"ZRANGEBYSCORE", "missing", "1", "2"}}, {{"ZRANGEBYSCORE", "r", "1"}},

            // BYLEX
            {{"ZADD", "lex", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e"}},
            {{"ZRANGE", "lex", "[b", "[d", "BYLEX"}}, {{"ZRANGE", "lex", "(b", "(d", "BYLEX"}},
            {{"ZRANGE", "lex", "-", "+", "BYLEX"}}, {{"ZRANGE", "lex", "-", "-", "BYLEX"}},
            {{"ZRANGE", "lex", "+", "-", "BYLEX"}}, {{"ZRANGE", "lex", "+", "-", "BYLEX", "REV"}},
            {{"ZRANGE", "lex", "[d", "[b", "BYLEX", "REV"}}, {{"ZRANGE", "lex", "-", "+", "BYLEX", "LIMIT", "1", "2"}},
            {{"ZRANGE", "lex", "b", "d", "BYLEX"}}, {{"ZRANGE", "lex", "[b", "++", "BYLEX"}},
            {{"ZRANGE", "lex", "[b", "[d", "BYLEX", "WITHSCORES"}}, {{"ZRANGE", "lex", "[", "[c", "BYLEX"}},
            {{"ZRANGEBYLEX", "lex", "[bb", "+"}}, {{"ZREVRANGEBYLEX", "lex", "+", "[c"}},
            {{"ZRANGEBYLEX", "lex", "-", "+", "LIMIT", "2", "10"}}, {{"ZRANGEBYLEX", "lex", "-", "+", "WITHSCORES"}},
            {{"ZRANGEBYLEX", "str", "-", "+"}}, {{"ZRANGEBYLEX", "missing", "x", "+"}},

            // ZCOUNT, ZLEXCOUNT
            {{"ZCOUNT", "r", "2", "2"}}, {{"ZCOUNT", "r", "-inf", "+inf"}}, {{"ZCOUNT", "r", "(1", "3"}},
            {{"ZCOUNT", "r", "5", "1"}}, {{"ZCOUNT", "r", "x", "1"}}, {{"ZCOUNT", "missing", "0", "1"}},
            {{"ZCOUNT", "str", "0", "1"}}, {{"ZCOUNT", "str", "x", "1"}}, {{"ZCOUNT", "r", "0"}},
            {{"ZLEXCOUNT", "lex", "-", "+"}}, {{"ZLEXCOUNT", "lex", "(a", "[c"}},
            {{"ZLEXCOUNT", "lex", "x", "+"}}, {{"ZLEXCOUNT", "missing", "-", "+"}},

            // ZPOPMIN, ZPOPMAX
            {{"ZADD", "p", "1", "a", "2", "b", "3", "c", "4", "d"}},
            {{"ZPOPMIN", "p"}}, {{"ZPOPMAX", "p"}}, {{"ZPOPMIN", "p", "0"}}, {{"ZPOPMIN", "p", "5"}},
            {{"EXISTS", "p"}}, {{"ZPOPMIN", "missing"}}, {{"ZPOPMAX", "missing", "3"}},
            {{"ZPOPMIN", "str"}}, {{"ZPOPMIN", "p", "-1"}}, {{"ZPOPMIN", "p", "x"}},
            {{"ZPOPMIN", "str", "-1"}}, {{"ZPOPMIN", "p", "1", "2"}}, {{"ZPOPMIN"}},

            // ZREMRANGEBY*
            {{"ZADD", "rr", "1", "a", "2", "b", "3", "c", "4", "d", "5", "e"}},
            {{"ZREMRANGEBYRANK", "rr", "0", "1"}}, {{"ZRANGE", "rr", "0", "-1"}},
            {{"ZREMRANGEBYSCORE", "rr", "(3", "4"}}, {{"ZRANGE", "rr", "0", "-1"}},
            {{"ZADD", "rr", "0", "x", "0", "y"}}, {{"ZREMRANGEBYLEX", "rr", "[x", "[y"}},
            {{"ZRANGE", "rr", "0", "-1", "WITHSCORES"}},
            {{"ZREMRANGEBYRANK", "rr", "-1", "-1"}}, {{"ZREMRANGEBYRANK", "rr", "0", "-1"}},
            {{"EXISTS", "rr"}}, {{"ZREMRANGEBYRANK", "missing", "0", "1"}},
            {{"ZREMRANGEBYRANK", "str", "0", "1"}}, {{"ZREMRANGEBYRANK", "r", "x", "1"}},
            {{"ZREMRANGEBYSCORE", "r", "x", "1"}}, {{"ZREMRANGEBYLEX", "r", "x", "+"}},
            {{"ZREMRANGEBYSCORE", "r", "10", "20"}}, {{"ZREMRANGEBYRANK", "r", "0"}},

            // TYPE and SCAN TYPE see the new type.
            {{"TYPE", "r"}}, {{"SCAN", "0", "TYPE", "zset", "COUNT", "100"}, Compare::scan},
        };
        return differential::run(argv[1], argv[2], cases, "sorted set");
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
