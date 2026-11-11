#pragma once

#include <cstdint>
#include <string>

namespace net {
    // Off by default: a snapshot currently runs under the database lock, so
    // every client waits while it is written.
    constexpr std::uint32_t default_save_seconds = 0;
    constexpr std::uint64_t default_rewrite_min_size = 64ULL * 1024 * 1024;

    // With an AOF, a snapshot is saved every save_seconds if anything was
    // written, and the AOF is rewritten once it reaches rewrite_min_size bytes
    // and has doubled since startup or the last rewrite. 0 disables either one.
    int run_server(std::uint16_t port, const std::string& aof_path = "",
                   bool sync_every_second = true, int io_threads = 4,
                   std::uint32_t save_seconds = default_save_seconds,
                   std::uint64_t rewrite_min_size = default_rewrite_min_size);
}
