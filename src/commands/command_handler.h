#pragma once

#include <string>
#include <vector>

namespace commands {
    std::string execute_command(
        const std::vector<std::string>& arguments
    );
}