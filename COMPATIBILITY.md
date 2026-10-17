# Redis compatibility checklist

- Target release: Redis Open Source 8.2.9
- Protocol target: RESP2 first

"Implemented" means the current server has a command handler. It does not mean
the command matches every behavior of the target release. "Compared with Redis"
means its replies and edge cases have been checked against that exact release.

| Command | Implemented | Compared with Redis | Notes |
| --- | --- | --- | --- |
| PING | Yes | No | |
| ECHO | Yes | No | |
| SET | Yes | No | Supports plain SET, NX, XX, EX, PX, EXAT, and PXAT; other options remain. |
| MSET | Yes | No | |
| GET | Yes | No | |
| DEL | Yes | No | |
| EXISTS | Yes | No | |
| DBSIZE | Yes | No | May temporarily count expired keys until an active sweep removes them. |
| MGET | Yes | No | |
| EXPIRE | Yes | No | |
| PTTL | Yes | No | |
| TTL | Yes | No | |
| PERSIST | Yes | No | |
| PEXPIRE | Yes | No | |
| EXPIREAT | Yes | No | |
| PEXPIREAT | Yes | No | |
| PEXPIRETIME | Yes | No | |
| EXPIRETIME | Yes | No | |
| GETDEL | Yes | No | |
| INCR | Yes | No | |
| DECR | Yes | No | |
| INCRBY | Yes | No | |
| DECRBY | Yes | No | |
