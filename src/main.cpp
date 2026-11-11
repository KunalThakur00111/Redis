#include "net/server.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>
#include <string>

int main(int argc, char* argv[]) {
    std::uint16_t port = 6379;
    std::string aof_path;
    bool port_seen = false;
    bool aof_seen = false;
    bool fsync_seen = false;
    bool io_threads_seen = false;
    bool save_seen = false;
    bool rewrite_size_seen = false;
    bool sync_every_second = true;
    int io_threads = 4;
    std::uint32_t save_seconds = net::default_save_seconds;
    std::uint64_t rewrite_min_size = net::default_rewrite_min_size;

    // Parse a whole decimal number; partial text such as "10s" is rejected.
    auto parse_number = [](std::string_view text, auto& value){
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };

    // Accept --port and --aof in either order, once each.
    for(int i = 1; i < argc; i += 2){
        if(i + 1 >= argc) break;
        const std::string_view option(argv[i]);

        if(option == "--port" && !port_seen){
            const std::string_view text(argv[i + 1]);
            unsigned int requested_port = 0;
            const auto result = std::from_chars(
                text.data(), text.data() + text.size(), requested_port);
            if(result.ec != std::errc{} ||
               result.ptr != text.data() + text.size() ||
               requested_port < 1 || requested_port > 65535) break;
            port = static_cast<std::uint16_t>(requested_port);
            port_seen = true;
        } else if(option == "--aof" && !aof_seen && argv[i + 1][0] != '\0'){
            aof_path = argv[i + 1];
            aof_seen = true;
        } else if(option == "--aof-fsync" && !fsync_seen){
            const std::string_view policy(argv[i + 1]);
            if(policy != "always" && policy != "everysec") break;
            sync_every_second = policy == "everysec";
            fsync_seen = true;
        } else if(option == "--io-threads" && !io_threads_seen){
            const std::string_view text(argv[i + 1]);
            int requested_threads = 0;
            const auto result = std::from_chars(
                text.data(), text.data() + text.size(), requested_threads);
            if(result.ec != std::errc{} ||
               result.ptr != text.data() + text.size() ||
               requested_threads < 1 || requested_threads > 16) break;
            io_threads = requested_threads;
            io_threads_seen = true;
        } else if(option == "--save-seconds" && !save_seen){
            if(!parse_number(argv[i + 1], save_seconds)) break;
            save_seen = true;
        } else if(option == "--aof-rewrite-min-size" && !rewrite_size_seen){
            if(!parse_number(argv[i + 1], rewrite_min_size)) break;
            rewrite_size_seen = true;
        } else {
            break;
        }

        if(i + 2 == argc){
            // These options only affect the AOF, so they need --aof.
            if((fsync_seen || save_seen || rewrite_size_seen) && !aof_seen) break;
            return net::run_server(port, aof_path, sync_every_second,
                                   io_threads, save_seconds, rewrite_min_size);
        }
    }

    if(argc == 1) return net::run_server(port, aof_path);
    std::cerr << "Usage: " << argv[0]
              << " [--port 1-65535] [--aof path] [--aof-fsync always|everysec]"
              << " [--io-threads 1-16]\n"
              << "       [--save-seconds N (0 = off)] [--aof-rewrite-min-size BYTES (0 = off)]\n";
    return 1;
}
