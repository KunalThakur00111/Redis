#include "command_handler.h"

#include "../protocol/resp_writer.h"

#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <charconv>
#include <chrono>
#include <limits>
#include <system_error>

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

bool parse_int64(const std::string& text, std::int64_t& value) {
    const char* first = text.data();
    const char* last = first + text.size();

    if (first == last) return false;
    if (*first == '+') {
        first++;
        if (first == last || *first < '0' || *first > '9') return false;
    }

    const auto result = std::from_chars(first, last, value);// it accepts '-' but not '+' that's why we handle that ourselves

    return result.ec == std::errc{} /*an error code; a default value (std::errc{}) means no conversion error.*/
                        && result.ptr == last; // where parsing stops;
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
        database.values[arguments[1]] = storage::Entry{arguments[2],std::nullopt};//std::nullopt explicitly means no expiry 
        return "+OK\r\n";
    }
    if (name == "MSET") {
        if (count < 3 || count % 2 == 0) return wrong_argument_count(name);
        // Arguments after MSET alternate between a key and its value.
        for (std::size_t i = 1; i < count; i += 2) {
            database.values[arguments[i]] = storage::Entry{arguments[i + 1],std::nullopt};
        }
        return "+OK\r\n";
    }
    if (name == "GET") {
        if (count != 2) return wrong_argument_count(name);
        auto found = database.find_live(arguments[1]);
        if (found == database.values.end()) return "$-1\r\n";
        return protocol::encode_bulk_string(found->second.value);
    }
    if (name == "DEL") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t removed = 0;
        for (std::size_t i = 1; i < count; ++i) {
            auto found = database.find_live(arguments[i]);// expiry already deleted earlier by their own
            if(found!=database.values.end()){
                database.values.erase(found);
                removed++;
            }
        }
        return protocol::encode_integer(removed);
    }
    if (name == "EXISTS") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t found_count = 0;
        for (std::size_t i = 1; i < count; ++i) {
            if (database.find_live(arguments[i]) != database.values.end()) {
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
            auto found = database.find_live(arguments[i]);
            elements.push_back(found == database.values.end()
                                   ? "$-1\r\n"
                                   : protocol::encode_bulk_string(found->second.value));
        }
        return protocol::encode_array(elements);
    }
    if(name == "EXPIRE"){
        if(count != 3) return wrong_argument_count(name);

        std::int64_t seconds=0;
        if(!parse_int64(arguments[2],seconds)){
            return "-ERR value is not an integer or out of range\r\n"; 
        }

        const auto now_ms= std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if(seconds>0 && seconds>(std::numeric_limits<std::int64_t>::max()-now_ms)/1000) {
            return "-ERR invalid expire time in 'expire' command\r\n";
        }
        const std::int64_t deadline_ms = seconds <= 0 ? now_ms : now_ms + seconds * 1000;

        return protocol::encode_integer(
            database.expires_at(arguments[1], deadline_ms) ? 1 : 0);
    }
    return "-ERR unknown command\r\n";
}
}  // namespace commands
