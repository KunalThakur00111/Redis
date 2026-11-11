// Check SAVE, BGREWRITEAOF, periodic snapshots, and automatic AOF rewrites.
#include "test_support.h"
#include "../src/persistence/snapshot.h"
#include "../src/storage/database.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace std::string_literals;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    TEST_CHECK(static_cast<bool>(file));
    return std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
}

// The snapshot must decode and point at the current end of the AOF, or inside
// it when later writes may have been logged.
void check_snapshot_matches_aof(const std::string& aof_path, bool exact = true) {
    storage::Database database;
    std::uint64_t offset = 0;
    std::string error;
    TEST_CHECK(persistence::decode_snapshot(read_file(aof_path + ".snapshot"), database,
                                            offset, error));
    const auto size = std::filesystem::file_size(aof_path);
    TEST_CHECK(exact ? offset == size : offset <= size);
}

template<typename Condition>
bool wait_until(Condition condition, int timeout_ms = 5000) {
    for(int elapsed = 0; elapsed < timeout_ms; elapsed += 20){
        if(condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        const std::string binary = argv[1];
        const std::vector<std::string> extra(argv + 2, argv + argc);
        auto options_for = [&](const std::string& aof_path,
                               std::vector<std::string> options){
            options.insert(options.begin(), {"--aof", aof_path});
            options.insert(options.end(), extra.begin(), extra.end());
            return options;
        };

        // BGREWRITEAOF compacts the log, and the new log restores everything.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            const auto options = options_for(aof_path, {"--save-seconds", "0",
                                                        "--aof-rewrite-min-size", "0"});
            {
                test::Server server(binary, options);
                auto exchange = [&](std::initializer_list<std::string_view> args){
                    return test::exchange_request(server.port(), args);
                };
                std::string pipeline;
                for(int i = 0; i < 300; i++){
                    pipeline += test::request({"INCR", "counter"});
                    pipeline += test::request({"SET", "text", "value-" + std::to_string(i)});
                }
                test::exchange(server.port(), pipeline);
                TEST_CHECK(exchange({"RPUSH", "items", "a", "b", "c"}) == ":3\r\n");
                TEST_CHECK(exchange({"LPOP", "items"}) == "$1\r\na\r\n");
                TEST_CHECK(exchange({"HSET", "profile", "name", "Kunal"}) == ":1\r\n");
                TEST_CHECK(exchange({"SADD", "members", "x", "y"}) == ":2\r\n");
                TEST_CHECK(exchange({"SET", "binary", "a\0b"s, "PX", "600000"}) == "+OK\r\n");
                TEST_CHECK(exchange({"SET", "short", "gone", "PX", "100"}) == "+OK\r\n");
                TEST_CHECK(exchange({"SET", "deleted", "x"}) == "+OK\r\n");
                TEST_CHECK(exchange({"DEL", "deleted"}) == ":1\r\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(150));

                const auto before = std::filesystem::file_size(aof_path);
                TEST_CHECK(exchange({"BGREWRITEAOF"}) ==
                           "+Background append only file rewriting started\r\n");
                const auto after = std::filesystem::file_size(aof_path);
                TEST_CHECK(after < before / 10);
                check_snapshot_matches_aof(aof_path);

                // Writes after the rewrite go to the new log.
                TEST_CHECK(exchange({"SET", "after", "rewrite"}) == "+OK\r\n");
                TEST_CHECK(std::filesystem::file_size(aof_path) > after);

                // No leftover temporary files.
                std::size_t files = 0;
                for(const auto& item : std::filesystem::directory_iterator(directory.path())){
                    (void)item;
                    files++;
                }
                TEST_CHECK(files == 2);
                server.interrupt();
            }

            auto check_restored = [&]{
                test::Server server(binary, options);
                auto exchange = [&](std::initializer_list<std::string_view> args){
                    return test::exchange_request(server.port(), args);
                };
                TEST_CHECK(exchange({"GET", "counter"}) == "$3\r\n300\r\n");
                TEST_CHECK(exchange({"GET", "text"}) == "$9\r\nvalue-299\r\n");
                TEST_CHECK(exchange({"LRANGE", "items", "0", "-1"}) ==
                           "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
                TEST_CHECK(exchange({"HGET", "profile", "name"}) == "$5\r\nKunal\r\n");
                TEST_CHECK(exchange({"SCARD", "members"}) == ":2\r\n");
                TEST_CHECK(exchange({"GET", "binary"}) == "$3\r\na\0b\r\n"s);
                const std::string ttl = exchange({"TTL", "binary"});
                TEST_CHECK(ttl.size() > 1 && ttl[0] == ':' && ttl != ":-1\r\n" &&
                           ttl != ":-2\r\n");
                TEST_CHECK(exchange({"EXISTS", "short", "deleted"}) == ":0\r\n");
                TEST_CHECK(exchange({"GET", "after"}) == "$7\r\nrewrite\r\n");
                TEST_CHECK(exchange({"DBSIZE"}) == ":7\r\n");
                server.interrupt();
            };
            check_restored();

            // The rewritten log is complete without the snapshot.
            std::filesystem::remove(aof_path + ".snapshot");
            check_restored();
        }

        // SAVE writes a snapshot that a crashed server starts from, and writes
        // logged after it are replayed on top.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            const auto options = options_for(aof_path, {"--save-seconds", "0"});
            {
                test::Server server(binary, options);
                auto exchange = [&](std::initializer_list<std::string_view> args){
                    return test::exchange_request(server.port(), args);
                };
                TEST_CHECK(exchange({"SET", "before", "save"}) == "+OK\r\n");
                TEST_CHECK(exchange({"save"}) == "+OK\r\n");
                check_snapshot_matches_aof(aof_path);
                TEST_CHECK(exchange({"SAVE", "extra"}) ==
                           "-ERR wrong number of arguments for 'save' command\r\n");
                TEST_CHECK(exchange({"SET", "after", "save"}) == "+OK\r\n");
                // SIGTERM is not handled, so this skips the shutdown snapshot.
                server.stop();
            }
            test::Server server(binary, options);
            TEST_CHECK(test::exchange_request(server.port(), {"MGET", "before", "after"}) ==
                       "*2\r\n$4\r\nsave\r\n$4\r\nsave\r\n");
        }

        // Startup deletes temporaries left by a crashed rewrite, and nothing else.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            const std::vector<std::string> stale{aof_path + ".rewrite.AbC123",
                                                 aof_path + ".snapshot.tmp.x1y2z3"};
            const std::vector<std::string> kept{aof_path + ".rewrite.toolong7",
                                                directory.path() + "/other.aof.rewrite.AbC123"};
            for(const auto& file : stale) std::ofstream(file) << "partial";
            for(const auto& file : kept) std::ofstream(file) << "keep";
            test::Server server(binary, options_for(aof_path, {}));
            for(const auto& file : stale) TEST_CHECK(!std::filesystem::exists(file));
            for(const auto& file : kept) TEST_CHECK(std::filesystem::exists(file));
        }

        // A periodic snapshot is saved after a write.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            test::Server server(binary, options_for(aof_path, {"--save-seconds", "1"}));
            TEST_CHECK(test::exchange_request(server.port(), {"SET", "k", "v"}) == "+OK\r\n");
            TEST_CHECK(wait_until([&]{
                return std::filesystem::exists(aof_path + ".snapshot");
            }));
            check_snapshot_matches_aof(aof_path);
        }

        // A log over the size threshold is rewritten automatically.
        {
            test::TemporaryDirectory directory;
            const std::string aof_path = directory.path() + "/data.aof";
            test::Server server(binary, options_for(aof_path, {"--save-seconds", "0",
                                                               "--aof-rewrite-min-size",
                                                               "8192"}));
            const std::string value(64, 'v');
            std::string pipeline;
            for(int i = 0; i < 200; i++) pipeline += test::request({"SET", "same", value});
            test::exchange(server.port(), pipeline);
            // The rewrite can start partway through the pipeline, so later SETs
            // may follow it in the new log; that log is still under the threshold.
            TEST_CHECK(wait_until([&]{
                return std::filesystem::exists(aof_path + ".snapshot") &&
                       std::filesystem::file_size(aof_path) < 8192;
            }));
            check_snapshot_matches_aof(aof_path, false);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "same"}) ==
                       "$64\r\n" + value + "\r\n");
        }

        // Without --aof there is nothing to save. AOF-only options are left out.
        {
            std::vector<std::string> options;
            for(std::size_t i = 0; i + 1 < extra.size(); i += 2){
                if(extra[i] == "--io-threads") options.insert(options.end(), {extra[i], extra[i + 1]});
            }
            test::Server server(binary, options);
            TEST_CHECK(test::exchange_request(server.port(), {"BGREWRITEAOF"}) ==
                       "-ERR persistence is disabled; start the server with --aof\r\n");
        }
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
