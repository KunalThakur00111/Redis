#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace storage { struct Database; }

namespace persistence {

enum class SyncPolicy {
    always,
    every_second
};

class Aof {
public:
    explicit Aof(SyncPolicy sync_policy = SyncPolicy::always)
        : sync_policy_(sync_policy) {}
    ~Aof();
    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    bool open_and_replay(const std::string& path, storage::Database& database,
                         std::string& error);
    bool append_after_command(const std::vector<std::string>& arguments,
                              const std::string& reply, storage::Database& database,
                              std::string& error);
    bool sync_if_due(std::string& error);

private:
    int fd_ = -1;
    SyncPolicy sync_policy_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point next_sync_;
};

}  // namespace persistence
