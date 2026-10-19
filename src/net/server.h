#pragma once

#include <cstdint>
#include <string>

namespace net {
    int run_server(std::uint16_t port, const std::string& aof_path = "");
}
