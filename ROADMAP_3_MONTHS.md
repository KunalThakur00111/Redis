# Three-month Redis server showcase roadmap

Updated 2026-10-18. Target review: mid-January 2027. This is a plan, not a
claim that future features already work. The target is a substantial,
explainable Redis-compatible server with tested behavior for a defined subset
of Redis Open Source 8.2.9. More available time can pull work forward; passing
the checks below matters more than following exact week boundaries.

## Starting point

- Linux C++17 server with one level-triggered epoll loop and one shared database.
- RESP2 arrays of bulk strings, per-client buffers, pipelining, and bounded
  input/output. Strings, lists (`LPUSH`, `LRANGE`), key expiry, and selected key
  commands are implemented. See [COMPATIBILITY.md](COMPATIBILITY.md) for the
  command inventory; implementation does not imply complete Redis semantics.
- Parser and TCP tests run with CTest. A local Redis 8.2.9 reference can run
  focused comparisons. No persistence, worker threads, replication, or cluster
  behavior is implemented yet.

## Three-month deliverable

Build one reliable path through each major layer for the **supported subset**:

1. RESP2 clients can pipeline supported commands, receive ordered replies, and
   encounter tested behavior for malformed requests and slow clients.
2. A typed keyspace supports strings, lists, hashes, and sets, with consistent
   expiry and wrong-type behavior. Sorted sets are a stretch goal after these
   types and their tests are stable.
3. An append-only log restores supported data and expiry after restart. State
   changes have one ordered representation usable by persistence and replicas.
4. Multiple I/O threads can serve independent clients while a controlled
   execution boundary preserves command order and shared-data safety. Parallel
   command execution is considered only after benchmarks identify a bottleneck.
5. A read-only replica can perform initial synchronization, receive ordered
   writes, and reconnect using a full resynchronization if necessary.
6. Multiple primary nodes can serve statically assigned hash slots. Clients
   receive `MOVED` for keys owned elsewhere, and multi-key commands reject
   cross-slot requests where required.
7. A reproducible test and demo show these guarantees, measured throughput and
   latency, and documented limitations. Current tests do not yet prove them.

"Reliable" here means repeatable tests for the listed behaviors under normal
operation, disconnects, and restart. It does not mean production-grade Redis
compatibility or availability. Redis replication also specifies partial
resynchronization, and Redis Cluster includes node discovery, resharding, and
failover; those are outside this three-month commitment.

## Milestones and exit checks

| Window | Build | Exit check |
| --- | --- | --- |
| Weeks 1–2 | Expand the differential RESP2 test harness; establish command metadata and typed keyspace rules; finish the core list operations. | The same request corpus runs against our server and Redis 8.2.9; mismatches are recorded, not hidden. |
| Weeks 3–4 | Add hashes and sets with common operations; make key, expiry, and wrong-type behavior consistent across types. | Socket tests cover multi-client use, key replacement, expiry, and wrong types for each supported type. |
| Weeks 5–6 | Add append-only persistence and startup replay. Define logging order, durability policy, and handling of an incomplete final record. | Write data, stop/restart, and recover it; inject an interrupted write and document the recovery result. |
| Weeks 7–8 | Add I/O worker threads with owned connections and bounded handoff to command execution. Benchmark before considering sharded execution. | Concurrent clients retain reply order; stress tests and ThreadSanitizer find no known data races; benchmark results include workload and latency, not only throughput. |
| Weeks 9–10 | Add primary–replica full sync and ordered live writes, then reconnect through full resync. | Two processes converge after initial sync, ongoing writes, disconnect, and reconnect; replicas reject writes. |
| Week 11 | Add static slot ownership, Redis slot hashing and hash tags, `MOVED`, and same-slot validation for supported multi-key commands. | A multi-process demo routes keys to their owners and proves a cross-slot request is rejected. |
| Week 12 and remaining buffer | Fix integration failures, add basic operational controls appropriate to the current network exposure, rerun comparison/stress/restart tests, document measured performance and limits. | A fresh clone builds; one script or documented sequence runs the demo; the README and interview guide match the actual code. |

These windows are aggressive. Persistence, concurrency, and distributed state
are dependency gates: if one is not stable, we spend the next window fixing it
before claiming a later layer works. We cut extra commands and polish before
cutting tests for core guarantees. If more time is available, pull the next
gate forward; do not label unverified behavior reliable.

## Candidate extensions after the gates

- Sorted sets, `MULTI`/`EXEC`/`WATCH`, Pub/Sub, RESP3, and broader command coverage.
- Snapshotting, AOF rewriting, memory limits and eviction policies.
- Parallel command execution with sharding and coordinated multi-key operations.
- Partial replica resynchronization, automatic failover, live slot migration,
  node gossip, ACLs, scripting, and full Redis Cluster interoperability.

## Working and interview-preparation loop

For each milestone, first state the behavior and failure cases. Codex can write
the implementation, tests, and documentation after explaining the change;
Kunal reviews the code and asks for a full explanation of unfamiliar C++ or
Linux calls. Run build, CTest, focused Redis comparisons, and the milestone's
failure tests. Record what passed, what remains unsupported, and one measured
result where performance is relevant. Update [INTERVIEW_PREP.md](INTERVIEW_PREP.md)
with answers grounded in the code that actually shipped.

Official references: [Redis replication](https://redis.io/docs/latest/operate/oss_and_stack/management/replication/)
and [Redis Cluster specification](https://redis.io/docs/latest/operate/oss_and_stack/reference/cluster-spec/).
