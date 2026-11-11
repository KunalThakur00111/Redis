#include "command_handler.h"

#include "../protocol/resp_writer.h"
#include "glob.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <charconv>
#include <chrono>
#include <limits>
#include <system_error>
#include <variant>
#include <algorithm>
#include <functional>
#include <utility>

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

constexpr const char* syntax_error = "-ERR syntax error\r\n";
constexpr const char* not_integer_error = "-ERR value is not an integer or out of range\r\n";

// Every command name this server accepts, for COMMAND COUNT and COMMAND LIST.
// SAVE, BGREWRITEAOF, INFO and CONFIG are run by the server, not this file.
constexpr const char* supported_commands[] = {
    "PING", "ECHO", "SET", "MSET", "GET", "DEL", "EXISTS", "DBSIZE", "MGET",
    "EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT", "TTL", "PTTL", "PERSIST",
    "EXPIRETIME", "PEXPIRETIME", "GETDEL", "INCR", "DECR", "INCRBY", "DECRBY",
    "LPUSH", "RPUSH", "LPOP", "RPOP", "LRANGE", "LLEN", "LINDEX",
    "HSET", "HGET", "HDEL", "HLEN", "SADD", "SREM", "SISMEMBER", "SCARD", "SMEMBERS",
    "TYPE", "KEYS", "SCAN", "RENAME", "RENAMENX", "FLUSHDB", "FLUSHALL", "SELECT",
    "COMMAND", "INFO", "CONFIG", "SAVE", "BGREWRITEAOF",
};

// Text quoted inside an error reply must not end the line early.
std::string one_line(std::string text) {
    for(char& ch : text){
        if(ch == '\r' || ch == '\n') ch = ' ';
    }
    return text;
}

std::string encode_bulk_strings(const std::vector<std::string>& values) {
    std::vector<std::string> encoded;
    encoded.reserve(values.size());
    for(const auto& value : values) encoded.push_back(protocol::encode_bulk_string(value));
    return protocol::encode_array(encoded);
}

/*
SCAN cursor design. Redis walks its hash table's buckets, which works because it
controls how the table grows. std::unordered_map can rehash into a different
bucket order at any insert, so a bucket index would skip keys after a rehash.

Instead, the cursor is a position in the order of each key's 64-bit hash, which
never changes while the process runs. Each call returns the keys with the
smallest hashes at or above the cursor, and the next cursor is one past the
largest hash returned. So every key present for the whole scan is returned,
however the map grows or shrinks. The cost: each call reads every key, so a
full scan costs O(N * N / COUNT) rather than O(N).
*/
std::string scan_keys(const std::vector<std::string>& arguments, storage::Database& database) {
    // Like Redis, accept an optional leading '+'; from_chars rejects it.
    std::string_view cursor_text = arguments[1];
    if(cursor_text.size() > 1 && cursor_text[0] == '+') cursor_text.remove_prefix(1);
    std::uint64_t cursor = 0;
    const auto parsed = std::from_chars(cursor_text.data(),
                                        cursor_text.data() + cursor_text.size(), cursor);
    if(cursor_text.empty() || parsed.ec != std::errc{} ||
       parsed.ptr != cursor_text.data() + cursor_text.size()){
        return "-ERR invalid cursor\r\n";
    }

    std::int64_t batch = 10;
    std::optional<std::string> pattern;
    std::optional<std::string> type;
    for(std::size_t i = 2; i < arguments.size(); i += 2){
        const std::string option = uppercase_ascii(arguments[i]);
        if(i + 1 >= arguments.size()) return syntax_error;
        if(option == "COUNT"){
            if(!parse_int64(arguments[i + 1], batch)) return not_integer_error;
            if(batch < 1) return syntax_error;
        } 
        else if(option == "MATCH"){
            pattern = arguments[i + 1];
        } 
        else if(option == "TYPE"){
            type = lower_ascii(arguments[i + 1]);
        } 
        else {
            return syntax_error;
        }
    }

    const std::hash<std::string> hasher;
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    for(auto& key : database.live_keys()){
        const std::uint64_t hash = hasher(key);
        if(hash >= cursor) candidates.emplace_back(hash, std::move(key));
    }

    std::size_t take = candidates.size();
    std::uint64_t next_cursor = 0;
    if(static_cast<std::uint64_t>(batch) < candidates.size()){
        // Put the batch smallest hashes first, then also take every key that
        // shares the last hash so no key is split across two calls.
        auto by_hash = [](const auto& a, const auto& b){ return a.first < b.first; };
        auto boundary = candidates.begin() + static_cast<std::ptrdiff_t>(batch - 1);
        std::nth_element(candidates.begin(), boundary, candidates.end(), by_hash);
        const std::uint64_t last_hash = boundary->first;
        auto rest = std::partition(candidates.begin(), candidates.end(),
                                   [&](const auto& item){ return item.first <= last_hash; });
        take = static_cast<std::size_t>(rest - candidates.begin());
        if(take < candidates.size()) next_cursor = last_hash + 1;// last_hash < every remaining hash
    }

    // Like Redis, MATCH and TYPE filter a batch after it is chosen, so a call can
    // return fewer keys than COUNT, or none, before the scan is finished.
    std::vector<std::string> keys;
    for(std::size_t i = 0; i < take; i++){
        const std::string& key = candidates[i].second;
        if(pattern && !glob_match(*pattern, key)) continue;
        if(type && database.type_name(key) != *type) continue;
        keys.push_back(key);
    }

    return "*2\r\n" + protocol::encode_bulk_string(std::to_string(next_cursor)) +
           encode_bulk_strings(keys);
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
        if(!arithmetic_parse(arguments[2]) || !parse_int64(arguments[2], start) ||
           !arithmetic_parse(arguments[3]) || !parse_int64(arguments[3], stop)){
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

    if(name == "RPUSH"){
        if(count < 3) return wrong_argument_count(name);

        std::size_t length = 0;
        for(std::size_t i = 2; i < count; i++){
            auto result = database.rpush(arguments[1], arguments[i]);
            if(result == std::nullopt) return wrong_type_error;
            length = *result;
        }

        return protocol::encode_integer(static_cast<std::int64_t>(length));
    }

    if(name == "LPOP" || name == "RPOP"){
        if(count != 2 && count != 3) return wrong_argument_count(name);

        const bool has_count = count == 3;
        std::int64_t pop_count = 1;

        if(has_count){
            if(!arithmetic_parse(arguments[2]) ||
            !parse_int64(arguments[2], pop_count)){
                return "-ERR value is not an integer or out of range\r\n";
            }
            if(pop_count < 0){
                return "-ERR value is out of range, must be positive\r\n";
            }
        }

        auto result = database.pop_list(
            arguments[1],
            static_cast<std::size_t>(pop_count),
            name == "LPOP");

        if(result.wrong_type) return wrong_type_error;

        // Redis uses a null array for the count form and a null bulk string otherwise.
        if(!result.found) return has_count ? "*-1\r\n" : "$-1\r\n";

        if(!has_count){
            return protocol::encode_bulk_string(result.elements[0]);
        }

        std::vector<std::string> encoded_elements;
        encoded_elements.reserve(result.elements.size());
        for(const auto& element : result.elements){
            encoded_elements.push_back(protocol::encode_bulk_string(element));
        }
        return protocol::encode_array(encoded_elements);
    }

    if(name == "LLEN"){
        if(count != 2) return wrong_argument_count(name);

        auto result = database.llen(arguments[1]);
        if(result == std::nullopt) return wrong_type_error;

        return protocol::encode_integer(static_cast<std::int64_t>(*result));
    }

    if(name == "LINDEX"){
        if(count != 3) return wrong_argument_count(name);

        auto found = database.find_live(arguments[1]);
        if(found == database.values.end()) return "$-1\r\n";

        auto* list = std::get_if<std::deque<std::string>>(&found->second.value);
        if(list == nullptr) return wrong_type_error;

        std::int64_t index = 0;
        if(!arithmetic_parse(arguments[2]) ||
        !parse_int64(arguments[2], index)){
            return "-ERR value is not an integer or out of range\r\n";
        }

        const std::int64_t length = static_cast<std::int64_t>(list->size());

        // -1 means the last element, -2 the one before it.
        if(index < 0) index += length;
        if(index < 0 || index >= length) return "$-1\r\n";

        return protocol::encode_bulk_string((*list)[static_cast<std::size_t>(index)]);
    }

    if(name == "HSET"){
    // HSET key field value [field value ...]
        if(count < 4 || count % 2 != 0) return wrong_argument_count(name);

        std::int64_t added = 0;
        for(std::size_t i = 2; i < count; i += 2){
            auto result = database.hset(arguments[1], arguments[i], arguments[i + 1]);
            if(result == std::nullopt) return wrong_type_error;
            if(*result) added++;
        }

        // Replacing an existing field does not increase this count.
        return protocol::encode_integer(added);
    }

    if(name == "HGET"){
        if(count != 3) return wrong_argument_count(name);

        auto result = database.hget(arguments[1], arguments[2]);
        if(result.wrong_type) return wrong_type_error;
        if(result.value == std::nullopt) return "$-1\r\n";

        return protocol::encode_bulk_string(*result.value);
    }

    if(name == "HDEL"){
        // HDEL key field [field ...]
        if(count < 3) return wrong_argument_count(name);

        std::int64_t removed = 0;
        for(std::size_t i = 2; i < count; i++){
            auto result = database.hdel(arguments[1], arguments[i]);
            if(result == std::nullopt) return wrong_type_error;
            if(*result) removed++;
        }

        return protocol::encode_integer(removed);
    }

    if(name == "HLEN"){
        if(count != 2) return wrong_argument_count(name);

        auto result = database.hlen(arguments[1]);
        if(result == std::nullopt) return wrong_type_error;

        return protocol::encode_integer(static_cast<std::int64_t>(*result));
    }

    if(name == "SADD"){
        // SADD key member [member ...]
        if(count < 3) return wrong_argument_count(name);

        std::int64_t added = 0;
        for(std::size_t i = 2; i < count; i++){
            auto result = database.sadd(arguments[1], arguments[i]);
            if(result == std::nullopt) return wrong_type_error;
            if(*result) added++;
        }

        return protocol::encode_integer(added);
    }

    if(name == "SREM"){
        // SREM key member [member ...]
        if(count < 3) return wrong_argument_count(name);

        std::int64_t removed = 0;
        for(std::size_t i = 2; i < count; i++){
            auto result = database.srem(arguments[1], arguments[i]);
            if(result == std::nullopt) return wrong_type_error;
            if(*result) removed++;
        }

        return protocol::encode_integer(removed);
    }

    if(name == "SISMEMBER"){
        if(count != 3) return wrong_argument_count(name);

        auto result = database.sismember(arguments[1], arguments[2]);
        if(result == std::nullopt) return wrong_type_error;

        return protocol::encode_integer(*result ? 1 : 0);
    }

    if(name == "SCARD"){
        if(count != 2) return wrong_argument_count(name);

        auto result = database.scard(arguments[1]);
        if(result == std::nullopt) return wrong_type_error;

        return protocol::encode_integer(static_cast<std::int64_t>(*result));
    }

    if(name == "SMEMBERS"){
        if(count != 2) return wrong_argument_count(name);

        auto result = database.smembers(arguments[1]);
        if(result == std::nullopt) return wrong_type_error;

        std::vector<std::string> encoded_members;
        encoded_members.reserve(result->size());
        for(const auto& member : *result){
            encoded_members.push_back(protocol::encode_bulk_string(member));
        }

        return protocol::encode_array(encoded_members);
    }

    if(name == "TYPE"){
        if(count != 2) return wrong_argument_count(name);
        return "+" + database.type_name(arguments[1]) + "\r\n";
    }

    if(name == "KEYS"){
        if(count != 2) return wrong_argument_count(name);
        std::vector<std::string> keys;
        for(auto& key : database.live_keys()){
            if(glob_match(arguments[1], key)) keys.push_back(std::move(key));
        }
        return encode_bulk_strings(keys);
    }

    if(name == "SCAN"){
        if(count < 2) return wrong_argument_count(name);
        return scan_keys(arguments, database);
    }

    if(name == "RENAME" || name == "RENAMENX"){
        if(count != 3) return wrong_argument_count(name);
        const bool only_if_absent = name == "RENAMENX";
        const auto renamed = database.rename(arguments[1], arguments[2], only_if_absent);
        if(!renamed) return "-ERR no such key\r\n";
        if(!only_if_absent) return "+OK\r\n";
        return protocol::encode_integer(*renamed ? 1 : 0);
    }

    if(name == "FLUSHDB" || name == "FLUSHALL"){
        if(count > 2) return syntax_error;
        if(count == 2){
            const std::string mode = uppercase_ascii(arguments[1]);
            if(mode != "ASYNC" && mode != "SYNC") return syntax_error;
        }
        database.clear();
        return "+OK\r\n";
    }

    if(name == "SELECT"){
        if(count != 2) return wrong_argument_count(name);
        std::int64_t index = 0;
        if(!parse_int64(arguments[1], index)) return not_integer_error;
        // Only database 0 exists.
        if(index != 0) return "-ERR DB index is out of range\r\n";
        return "+OK\r\n";
    }

    if(name == "COMMAND"){
        if(count == 1){
            return "-ERR COMMAND without a subcommand is not supported; "
                   "use COMMAND COUNT, COMMAND LIST or COMMAND DOCS\r\n";
        }
        const std::string subcommand = uppercase_ascii(arguments[1]);
        if(subcommand == "COUNT" && count == 2){
            return protocol::encode_integer(static_cast<std::int64_t>(
                sizeof(supported_commands) / sizeof(supported_commands[0])));
        }
        if(subcommand == "LIST" && count == 2){
            std::vector<std::string> names;
            for(const char* command : supported_commands) names.push_back(lower_ascii(command));
            return encode_bulk_strings(names);
        }
        // redis-cli asks for COMMAND DOCS when it connects. No documentation is
        // kept here, and an empty reply tells it to go without hints.
        if(subcommand == "DOCS") return "*0\r\n";
        if(subcommand == "COUNT") return wrong_argument_count("command|count");
        // Redis's COMMAND LIST takes a FILTERBY clause, which is not supported here.
        if(subcommand == "LIST") return syntax_error;
        return "-ERR unknown subcommand '" + one_line(arguments[1]) + "'. Try COMMAND HELP.\r\n";
    }

    return "-ERR unknown command\r\n";
}
}  // namespace commands
