#include "command_handler.h"

#include "../protocol/resp_writer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace commands {
namespace {

std::string uppercase_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return text;
}

std::string wrong_argument_count(const std::string& name) {
    return "-ERR wrong number of arguments for '" + name + "' command\r\n";
}

}  // namespace

std::string execute_command(const std::vector<std::string>& arguments,
                            storage::Database& database) {
    if (arguments.empty()) {
        return "-ERR empty command\r\n";
    }

    const std::string name = uppercase_ascii(arguments[0]);
    const std::size_t count = arguments.size();

    if (name == "PING") {
        if (count == 1) return "+PONG\r\n";
        if (count == 2) return protocol::encode_bulk_string(arguments[1]);
        return wrong_argument_count(name);
    }
    if (name == "ECHO") {
        if (count != 2) return wrong_argument_count(name);
        return protocol::encode_bulk_string(arguments[1]);
    }
    if (name == "SET") {
        if (count != 3) return wrong_argument_count(name);
        database.values[arguments[1]] = arguments[2];
        return "+OK\r\n";
    }
    if (name == "MSET") {
        if (count < 3 || count % 2 == 0) return wrong_argument_count(name);
        // Arguments after MSET alternate between a key and its value.
        for (std::size_t i = 1; i < count; i += 2) {
            database.values[arguments[i]] = arguments[i + 1];
        }
        return "+OK\r\n";
    }
    if (name == "GET") {
        if (count != 2) return wrong_argument_count(name);
        auto found = database.values.find(arguments[1]);
        if (found == database.values.end()) return "$-1\r\n";
        return protocol::encode_bulk_string(found->second);
    }
    if (name == "DEL") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t removed = 0;
        for (std::size_t i = 1; i < count; ++i) {
            removed += static_cast<std::int64_t>(database.values.erase(arguments[i]));
        }
        return protocol::encode_integer(removed);
    }
    if (name == "EXISTS") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t found_count = 0;
        for (std::size_t i = 1; i < count; ++i) {
            if (database.values.find(arguments[i]) != database.values.end()) {
                ++found_count;
            }
        }
        return protocol::encode_integer(found_count);
    }
    if (name == "DBSIZE") {
        if (count != 1) return wrong_argument_count(name);
        return protocol::encode_integer(
            static_cast<std::int64_t>(database.values.size()));
    }
    if (name == "MGET") {
        if (count < 2) return wrong_argument_count(name);
        std::vector<std::string> elements;
        elements.reserve(count - 1);
        for (std::size_t i = 1; i < count; ++i) {
            auto found = database.values.find(arguments[i]);
            elements.push_back(found == database.values.end()
                                   ? "$-1\r\n"
                                   : protocol::encode_bulk_string(found->second));
        }
        return protocol::encode_array(elements);
    }
    return "-ERR unknown command\r\n";
}
}  // namespace commands
