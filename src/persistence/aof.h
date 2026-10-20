#pragma once

#include <chrono>
#include <condition_variable>// Condition variables are synchronization primitives in C++ that allow
//                              threads to wait for a specific condition to become true before continuing execution
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace storage { struct Database; }

namespace persistence {

enum class SyncPolicy {
    always,
    every_second
};

class Aof {
    private:
        int fd_ = -1;
        SyncPolicy sync_policy_;
        bool dirty_ = false;// dirty_ means record have been written since the last successfull sync
        std::chrono::steady_clock::time_point next_sync_;// stores when the next sync should happen
        std::uint64_t write_generation_ = 0;// a counter increased after each complete AOF record is written. it identifies weather
        //                                     new record arrives during the sync

        std::mutex sync_mutex_;// protect the field shared by event loop and worker
        std::condition_variable sync_cv_;// let the worker sleep and lets a write or shutdown wake it
        std::thread sync_thread_;// the worker thread itself
        bool stop_sync_ = false;// Tells the worker that normal Aof destruction has begun.
        std::string sync_error_;// An error from the worker that the event loop must notice.

        void sync_loop();

    public:
        explicit Aof(SyncPolicy sync_policy = SyncPolicy::every_second)
            : sync_policy_(sync_policy) {}
        ~Aof();
        Aof(const Aof&) = delete;
        Aof& operator=(const Aof&) = delete;

        bool open_and_replay(const std::string& path, storage::Database& database,
                            std::string& error);
        bool append_after_command(const std::vector<std::string>& arguments,
                                const std::string& reply, storage::Database& database,
                                std::string& error);
        bool check_sync_error(std::string& error);
        bool stop_and_sync(std::string& error);

};

}  // namespace persistence
