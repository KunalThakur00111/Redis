#pragma once

#include <string>
#include <vector>
#include "../storage/database.h"

namespace commands {
    std::string execute_command(
        const std::vector<std::string>& arguments,
        storage::Database& database
    );
}
