#pragma once

#include <string>
#include <vector>

namespace storage { struct Database; }

namespace persistence {

class Aof {
public:
    Aof() = default;
    ~Aof();
    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    bool open_and_replay(const std::string& path, storage::Database& database,
                         std::string& error);
    bool append_after_command(const std::vector<std::string>& arguments,
                              const std::string& reply, storage::Database& database,
                              std::string& error);

private:
    int fd_ = -1;
};

}  // namespace persistence
