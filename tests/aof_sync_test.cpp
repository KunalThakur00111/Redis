#include "../src/commands/command_handler.h"
#include "../src/persistence/aof.h"
#include "../src/storage/database.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" int __real_fdatasync(int fd);

namespace {

std::thread::id main_thread;
std::atomic<int> main_syncs{0};
std::atomic<int> background_syncs{0};
std::atomic<bool> fail_next_sync{false};
std::atomic<bool> interrupt_next_sync{false};
std::atomic<bool> hold_first_sync{true};
std::atomic<bool> first_sync_entered{false};
std::atomic<bool> release_first_sync{false};

void check(bool condition, const char* message) {
    if(condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

template<class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while(std::chrono::steady_clock::now() < deadline){
        if(predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

extern "C" int __wrap_fdatasync(int fd) {
    const bool fail = fail_next_sync.exchange(false);
    const bool interrupt = interrupt_next_sync.exchange(false);
    if(std::this_thread::get_id() == main_thread){
        ++main_syncs;
    } else {
        ++background_syncs;
        if(hold_first_sync.exchange(false)){
            first_sync_entered = true;
            while(!release_first_sync.load()){
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    if(fail){
        errno = EIO;
        return -1;
    }
    if(interrupt){
        errno = EINTR;
        return -1;
    }
    return __real_fdatasync(fd);
}

int main() {
    main_thread = std::this_thread::get_id();
    char path[] = "/tmp/redis-aof-sync-XXXXXX";
    const int fd = mkstemp(path);
    check(fd >= 0, "failed to create temporary AOF");
    close(fd);

    {
        storage::Database database;
        persistence::Aof aof(persistence::SyncPolicy::every_second);
        std::string error;
        check(aof.open_and_replay(path, database, error), "failed to open AOF");

        const std::vector<std::string> first = {"SET", "first", "one"};
        const std::string first_reply = commands::execute_command(first, database);
        check(first_reply == "+OK\r\n", "first SET failed");
        check(aof.append_after_command(first, first_reply, database, error),
              "failed to append first record");
        check(main_syncs.load() == 0, "first sync ran on main thread");
        check(wait_until([]{ return first_sync_entered.load(); }),
              "background worker did not begin syncing");

        const std::vector<std::string> second = {"SET", "second", "two"};
        const std::string second_reply = commands::execute_command(second, database);
        check(second_reply == "+OK\r\n", "second SET failed");
        check(aof.append_after_command(second, second_reply, database, error),
              "failed to append while sync was in progress");
        check(background_syncs.load() == 1, "second sync started too early");
        release_first_sync = true;
        check(wait_until([]{ return background_syncs.load() >= 2; }),
              "write during sync did not get another sync");

        fail_next_sync = true;
        const std::vector<std::string> third = {"SET", "third", "three"};
        const std::string third_reply = commands::execute_command(third, database);
        check(third_reply == "+OK\r\n", "third SET failed");
        check(aof.append_after_command(third, third_reply, database, error),
              "failed to append third record");
        check(wait_until([&aof]{
            std::string sync_error;
            return !aof.check_sync_error(sync_error) &&
                   sync_error.find("AOF sync failed") != std::string::npos;
        }), "background sync error was not reported");
        check(main_syncs.load() == 0, "sync ran on main thread");
    }

    {
        storage::Database database;
        persistence::Aof aof(persistence::SyncPolicy::every_second);
        std::string error;
        check(aof.open_and_replay(path, database, error), "failed to reopen AOF");
        const int earlier_syncs = background_syncs.load();
        const std::vector<std::string> last = {"SET", "last", "write"};
        const std::string reply = commands::execute_command(last, database);
        check(reply == "+OK\r\n", "last SET failed");
        check(aof.append_after_command(last, reply, database, error),
              "failed to append last record");
        interrupt_next_sync = true;
        check(aof.stop_and_sync(error), "shutdown sync failed");
        check(background_syncs.load() >= earlier_syncs + 2,
              "shutdown did not retry an interrupted sync");
    }

    unlink(path);
}
