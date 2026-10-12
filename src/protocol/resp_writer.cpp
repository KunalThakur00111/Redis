#include "resp_writer.h"

namespace protocol {

std::string encode_bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string encode_integer(std::int64_t value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string encode_array(const std::vector<std::string>& encoded_elements) {
    std::string reply = "*" + std::to_string(encoded_elements.size()) + "\r\n";
    for (const std::string& element : encoded_elements) {
        reply += element;
    }
    return reply;
}

}  // namespace protocol
