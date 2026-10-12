#pragma once

#include <string>
#include <unordered_map>

namespace storage {

struct Database {
    std::unordered_map<std::string, std::string> values;
};

}  // namespace storage
