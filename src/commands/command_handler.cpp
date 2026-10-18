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
#include <variant>

namespace commands {
namespace {

constexpr const char* wrong_type_error = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
std::string uppercase_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return text;
}

std::string lower_ascii(std::string text){
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

std::string wrong_argument_count(const std::string& name) {
    return "-ERR wrong number of arguments for '" + lower_ascii(name) + "' command\r\n";
}

bool arithmetic_parse(const std::string& text){
    if(text.empty()) return false;
    if(text.size() == 1 && text[0] == '0') return true;
    if((text[0]>='1' && text[0]<='9')) return true;

    if(text.size() == 1 && text[0] == '-') return false;
    if(text.size() > 1 && text[0] == '-' && text[1]>='1' && text[1] <= '9') return true;

    return false;
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
        if (count == 3){
            database.set(arguments[1],arguments[2]); 
            return "+OK\r\n";
        }

        if(count == 4){
            const std::string option = uppercase_ascii(arguments[3]);
            if(option!="NX" && option!="XX"){// "NX" writes only when key don't exits, "XX" writes only when it exits
                return "-ERR syntax error\r\n";
            }
            bool exists = database.find_live(arguments[1])!=database.values.end();
            if((exists && option=="NX") || (!exists && option=="XX")){
                return "$-1\r\n";
            }
            database.set(arguments[1],arguments[2]);
            return "+OK\r\n";
        }

        if(count!=5 && count!=6) return wrong_argument_count(name);

        std::string option = uppercase_ascii(arguments[3]);
        std::string condition;
        std::size_t expiry_value_index = 4;
        if (count == 6) {
            const std::string last_option = uppercase_ascii(arguments[5]);
            if (option == "NX" || option == "XX") {
                condition = option;
                option = uppercase_ascii(arguments[4]);
                expiry_value_index = 5;
            } 
            else if (last_option == "NX" || last_option == "XX") {
                condition = last_option;
            } else {
                return "-ERR syntax error\r\n";
            }
        }
        if(option!="EX" && option!="PX" && option!="EXAT" && option!="PXAT"){// SET mykey hello EX 5       # store "hello" and expire it after 5 seconds
            return "-ERR syntax error\r\n";// SET mykey hello PX 500       # store "hello" and expire it after 500 milliseconds
        }

        std::int64_t duration = 0;
        if(!parse_int64(arguments[expiry_value_index],duration)){
            return "-ERR value is not an integer or out of range\r\n";
        }
        if(duration<=0){
            return "-ERR invalid expire time in 'set' command\r\n";
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const std::int64_t available_ms =
            std::numeric_limits<std::int64_t>::max() - now_ms;


        std::int64_t deadline_ms = 0;
        if (option == "EX") {
            if (duration > available_ms / 1000) {
                return "-ERR invalid expire time in 'set' command\r\n";
            }
            deadline_ms = duration*1000 + now_ms; 
        } 
        else if (option == "PX") {
            if(duration > available_ms){
                return "-ERR invalid expire time in 'set' command\r\n";
            }
            deadline_ms = duration + now_ms;
        }
        else if(option == "EXAT"){// sets expiry absolute time in unix seconds
            if (duration > std::numeric_limits<std::int64_t>::max() / 1000) {
                return "-ERR invalid expire time in 'set' command\r\n";
            }
            deadline_ms = duration * 1000;
        }
        else{// PXAT
            deadline_ms = duration;
        }
        if (!condition.empty()) {
            const bool exists = database.find_live(arguments[1]) != database.values.end();
            if ((condition == "NX" && exists) ||
                (condition == "XX" && !exists)) {
                return "$-1\r\n";
            }
        }
        database.set(arguments[1], arguments[2], deadline_ms);
        return "+OK\r\n";
    }

    if (name == "MSET") {
        if (count < 3 || count % 2 == 0) return wrong_argument_count(name);
        // Arguments after MSET alternate between a key and its value.
        for (std::size_t i = 1; i < count; i += 2) {
            database.set(arguments[i],arguments[i+1]);
        }
        return "+OK\r\n";
    }

    if (name == "GET") {
        if (count != 2) return wrong_argument_count(name);
        auto found = database.find_live(arguments[1]);
        if (found == database.values.end()) return "$-1\r\n";

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if(stored_string == nullptr) return wrong_type_error;

        return protocol::encode_bulk_string(*stored_string);
    }

    if (name == "DEL") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t removed = 0;
        for (std::size_t i = 1; i < count; ++i) {
            removed += database.erase_live(arguments[i]);
        }
        return protocol::encode_integer(removed);
    }

    if (name == "EXISTS") {
        if (count < 2) return wrong_argument_count(name);
        std::int64_t found_count = 0;
        for (std::size_t i = 1; i < count; i++) {
            if (database.find_live(arguments[i]) != database.values.end()) {
                found_count++;
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
        for (std::size_t i = 1; i < count; i++) {
            auto found = database.find_live(arguments[i]);
            if(found == database.values.end()){
                elements.push_back("$-1\r\n");
                continue;
            }

            auto* stored_string = std::get_if<std::string>(&found->second.value);
            elements.push_back(stored_string == nullptr ? "$-1\r\n" : protocol::encode_bulk_string(*stored_string)); 
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

    if(name == "PTTL") {// tells remaining expiry time in milliseconds
        if(count!=2) return wrong_argument_count(name);
        return protocol::encode_integer(database.pttl(arguments[1]));
    }

    if(name == "TTL") {// tells remaining expiry time in seconds
        if(count!=2) return wrong_argument_count(name);
        const std::int64_t remaining_ms = database.pttl(arguments[1]);
        if(remaining_ms < 0) {
            return protocol::encode_integer(remaining_ms);
        }
        return protocol::encode_integer((remaining_ms + 500) / 1000);
    }

    if(name == "PERSIST"){// reomve the expiry time
        if(count!=2) return wrong_argument_count(name);
        return protocol::encode_integer(database.persist(arguments[1])?1:0);
    }

    if (name == "PEXPIRE") {
        if (count != 3) return wrong_argument_count(name);

        std::int64_t milliseconds = 0;
        if (!parse_int64(arguments[2], milliseconds)) {
            return "-ERR value is not an integer or out of range\r\n";
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (milliseconds > 0 &&
            milliseconds > std::numeric_limits<std::int64_t>::max() - now_ms) {
            return "-ERR invalid expire time in 'pexpire' command\r\n";
        }

        const std::int64_t deadline_ms =
            milliseconds <= 0 ? now_ms : now_ms + milliseconds;

        return protocol::encode_integer(
            database.expires_at(arguments[1], deadline_ms) ? 1 : 0);
    }

    if(name == "EXPIREAT") {//set expires in unix seconds
        if(count!=3) return wrong_argument_count(name);
        std::int64_t unix_seconds = 0;
        if (!parse_int64(arguments[2], unix_seconds)) {
            return "-ERR value is not an integer or out of range\r\n";
        }

        if (unix_seconds > std::numeric_limits<std::int64_t>::max() / 1000 ||
            unix_seconds < std::numeric_limits<std::int64_t>::min() / 1000) {
            return "-ERR invalid expire time in 'expireat' command\r\n";
        }

        const std::int64_t deadline_ms = unix_seconds * 1000;
        return protocol::encode_integer(
            database.expires_at(arguments[1], deadline_ms) ? 1 : 0);
    }

    if(name == "PEXPIREAT") {// set expires in unix milliseconds
        if (count != 3) return wrong_argument_count(name);

        std::int64_t deadline_ms = 0;
        if (!parse_int64(arguments[2], deadline_ms)) {
            return "-ERR value is not an integer or out of range\r\n";
        }

        return protocol::encode_integer(
            database.expires_at(arguments[1], deadline_ms) ? 1 : 0);
    }

    if(name == "PEXPIRETIME"){// tells expirty time stamp in unix milliseconds
        if(count != 2) return wrong_argument_count(name);
        return protocol::encode_integer(database.pexpiretime(arguments[1]));
    }

    if (name == "EXPIRETIME") {// this tells expiry time stamp in unix seconds
        if (count != 2) return wrong_argument_count(name);

        const std::int64_t deadline_ms = database.pexpiretime(arguments[1]);
        if (deadline_ms < 0) {
            return protocol::encode_integer(deadline_ms);
        }

        const std::int64_t deadline_seconds =
            deadline_ms / 1000 + (deadline_ms % 1000 >= 500 ? 1 : 0);

        return protocol::encode_integer(deadline_seconds);
    }

    if(name == "GETDEL"){// get and delete key
        if(count!=2) return wrong_argument_count(name);
        auto found = database.find_live(arguments[1]);
        if(found == database.values.end()){
            return "$-1\r\n";
        }

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if (stored_string == nullptr) return wrong_type_error;

        std::string reply = protocol::encode_bulk_string(*stored_string);
        database.erase_entry(found);
        return reply;
    }

    if (name == "INCR") {
        if (count != 2) return wrong_argument_count(name);

        auto found = database.find_live(arguments[1]);
        if (found == database.values.end()) {
            database.set(arguments[1], "1");
            return protocol::encode_integer(1);
        }

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if(stored_string == nullptr) return wrong_type_error;

        std::int64_t value = 0;
        if (!arithmetic_parse(*stored_string) || !parse_int64(*stored_string, value)) {
            return "-ERR value is not an integer or out of range\r\n";
        }
        if (value == std::numeric_limits<std::int64_t>::max()) {
            return "-ERR increment or decrement would overflow\r\n";
        }

        value++;
        *stored_string = std::to_string(value);
        return protocol::encode_integer(value);
    }

    if (name == "DECR") {
        if (count != 2) return wrong_argument_count(name);

        auto found = database.find_live(arguments[1]);
        if (found == database.values.end()) {
            database.set(arguments[1], "-1");
            return protocol::encode_integer(-1);
        }

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if(stored_string == nullptr) return wrong_type_error;

        std::int64_t value = 0;
        if (!arithmetic_parse(*stored_string) || !parse_int64(*stored_string, value)) {
            return "-ERR value is not an integer or out of range\r\n";
        }
        if (value == std::numeric_limits<std::int64_t>::min()) {
            return "-ERR increment or decrement would overflow\r\n";
        }

        value--;
        *stored_string = std::to_string(value);
        return protocol::encode_integer(value);
    }

    if (name == "INCRBY"){
        if(count != 3) return wrong_argument_count(name);

        auto found = database.find_live(arguments[1]);

        std::int64_t value = 0;
        if(!arithmetic_parse(arguments[2]) || !parse_int64(arguments[2], value)){
            return "-ERR value is not an integer or out of range\r\n";
        }

        if(found == database.values.end()){
            database.set(arguments[1],arguments[2]);
            return protocol::encode_integer(value);
        }

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if (stored_string == nullptr) return wrong_type_error;

        std::int64_t stored_value=0;
        if(!arithmetic_parse(*stored_string) || !parse_int64(*stored_string, stored_value)){
            return "-ERR value is not an integer or out of range\r\n";
        }

        if ((value > 0 && stored_value > std::numeric_limits<std::int64_t>::max() - value) ||
            (value < 0 && stored_value < std::numeric_limits<std::int64_t>::min() - value)) {
            return "-ERR increment or decrement would overflow\r\n";
        }

        value = value + stored_value;
        *stored_string = std::to_string(value);
        return protocol::encode_integer(value);
    }

    if (name == "DECRBY"){
        if(count != 3) return wrong_argument_count(name);

        auto found = database.find_live(arguments[1]);

        std::int64_t value = 0;
    
        if(!arithmetic_parse(arguments[2]) || !parse_int64(arguments[2], value)){
            return "-ERR value is not an integer or out of range\r\n";
        }

        if (value == std::numeric_limits<std::int64_t>::min()) {
            return "-ERR decrement would overflow\r\n";
        }
        value = -value;
        if(found == database.values.end()){
            database.set(arguments[1],std::to_string(value));
            return protocol::encode_integer(value);
        }

        auto* stored_string = std::get_if<std::string>(&found->second.value);
        if (stored_string == nullptr) return wrong_type_error;

        std::int64_t stored_value=0;
        if(!arithmetic_parse(*stored_string) || !parse_int64(*stored_string, stored_value)){
            return "-ERR value is not an integer or out of range\r\n";
        }
        if ((value > 0 && stored_value > std::numeric_limits<std::int64_t>::max() - value) ||
            (value < 0 && stored_value < std::numeric_limits<std::int64_t>::min() - value)) {
            return "-ERR increment or decrement would overflow\r\n";
        }

        value = value + stored_value;
        *stored_string = std::to_string(value);
        return protocol::encode_integer(value);
    }

    if(name == "LPUSH"){// push elements into the list data structure
        if(count < 3) return wrong_argument_count(name);
        std::size_t length=0;
        for(std::size_t i=2; i<count; i++){
            auto result = database.lpush(arguments[1],arguments[i]);
            if(result == std::nullopt) return wrong_type_error;

            length=*result;
        }
        return protocol::encode_integer(length);
    }

    if(name == "LRANGE"){
        if(count != 4) return wrong_argument_count(name);

        std::int64_t start = 0;
        std::int64_t stop = 0;
        if(!parse_int64(arguments[2], start) || !parse_int64(arguments[3], stop)){
            return "-ERR value is not an integer or out of range\r\n";
        }

        auto result = database.lrange(arguments[1], start, stop);
        if(result == std::nullopt) return wrong_type_error;

        std::vector<std::string> elements;
        elements.reserve(result->size());
        for(const auto& value : *result){
            elements.emplace_back(protocol::encode_bulk_string(value));
        }

        return protocol::encode_array(elements);
    }

    return "-ERR unknown command\r\n";
}
}  // namespace commands
