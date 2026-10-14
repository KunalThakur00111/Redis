#include "net/server.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>

int main(int argc, char* argv[]) {
    if (argc == 1) {
        return net::run_server(6379);
    }

    if (argc == 3 && std::string_view(argv[1]) == "--port") {
        std::string_view text(argv[2]);
        unsigned int requested_port = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                            requested_port);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
            requested_port >= 1 && requested_port <= 65535) {
            return net::run_server(static_cast<std::uint16_t>(requested_port));
        }
    }

    std::cerr << "Usage: " << argv[0] << " [--port 1-65535]\n";
    return 1;
}
