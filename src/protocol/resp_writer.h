#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

std::string encode_bulk_string(const std::string& value);
std::string encode_integer(std::int64_t value);
std::string encode_array(const std::vector<std::string>& encoded_elements);

}  // namespace protocol
