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
        } else {
            break;
        }

        if(i + 2 == argc) return net::run_server(port, aof_path);
    }

    if(argc == 1) return net::run_server(port, aof_path);
    std::cerr << "Usage: " << argv[0]
              << " [--port 1-65535] [--aof path]\n";
    return 1;
}