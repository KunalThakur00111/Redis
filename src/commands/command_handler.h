#pragma once

#include <string>
#include <vector>
#include "../storage/database.h"

namespace commands {
    // True for names this server runs (already uppercase).
    bool is_known_command(const std::string& name);

    std::string execute_command(
        const std::vector<std::string>& arguments,
        storage::Database& database
    );
}
