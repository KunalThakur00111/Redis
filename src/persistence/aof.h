#pragma once

#include <chrono>
#include <condition_variable>// Condition variables are synchronization primitives in C++ that allow
//                              threads to wait for a specific condition to become true before continuing execution
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace storage { struct Database; }

namespace persistence {

enum class SyncPolicy {
    always,
    every_second
};

// ok: done (or nothing to do). failed: nothing changed and the server can keep
// running. fatal: the log may be damaged, so the server must stop.
enum class PersistResult {
    ok,
    failed,
    fatal
};

class Aof {
    private:
        int fd_ = -1;
        std::string path_;
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

        // Set by note_before_command() for the next append_after_command().
        bool key_was_stable_ = false;
        std::string stable_key_;

        // These are changed only while the caller holds the database lock.
        std::uint64_t changes_since_save_ = 0;// logged writes since the last snapshot
        std::chrono::steady_clock::time_point last_save_;
        std::uint64_t rewrite_base_size_ = 0;// AOF size at startup or after the last rewrite
        std::chrono::seconds save_interval_{0};// 0 disables periodic snapshots
        std::uint64_t rewrite_min_size_ = 0;// 0 disables automatic rewrites
        std::chrono::steady_clock::time_point next_scheduled_attempt_;// delays a retry after a failure

        void sync_loop();
        bool start_sync_thread(std::string& error);
        std::string snapshot_path() const { return path_ + ".snapshot"; }

    public:
        explicit Aof(SyncPolicy sync_policy = SyncPolicy::every_second)
            : sync_policy_(sync_policy) {}
        ~Aof();
        Aof(const Aof&) = delete;
        Aof& operator=(const Aof&) = delete;

        bool open_and_replay(const std::string& path, storage::Database& database,
                            std::string& error, std::uint64_t snapshot_offset = 0,
                            std::string_view snapshot_checksum = {});
                            
        // Call just before running a command, under the same database lock as
        // append_after_command(). It records whether the command's key is live
        // with no expiry, which is when a short record of the write is safe.
        // Without this call, every write logs the whole affected keys.
        void note_before_command(const std::vector<std::string>& arguments,
                                 const storage::Database& database);

        bool append_after_command(const std::vector<std::string>& arguments,
                                const std::string& reply, storage::Database& database,
                                std::string& error);

        bool current_offset(std::uint64_t& offset, std::string& error) const;
        bool next_snapshot_offset(std::uint64_t& offset, std::string& error) const;

        bool append_snapshot_checkpoint(std::string_view snapshot,
                                        std::uint64_t expected_offset, std::string& error);

        bool enabled() const { return fd_ >= 0; }
        // Logged writes since the last snapshot; read under the database lock.
        std::uint64_t changes_since_save() const { return changes_since_save_; }

        // Configure periodic work done by run_scheduled(). A snapshot is saved
        // every save_interval when at least one write was logged. The AOF is
        // rewritten when it is at least rewrite_min_size bytes and has doubled
        // since startup or the last rewrite.
        void set_schedule(std::chrono::seconds save_interval, std::uint64_t rewrite_min_size);

        // The caller must hold the database lock for all three.
        PersistResult save_snapshot(storage::Database& database, std::string& error);
        PersistResult rewrite(storage::Database& database, std::string& error);
        PersistResult run_scheduled(storage::Database& database, std::string& error);

        bool check_sync_error(std::string& error);
        bool stop_and_sync(std::string& error);

};

}  // namespace persistence
