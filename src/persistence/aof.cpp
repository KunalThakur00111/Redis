#include "aof.h"

#include "../commands/command_handler.h"
#include "../protocol/resp_parser.h"
#include "../storage/database.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <variant>

namespace persistence {
namespace {

constexpr std::uint64_t max_record_size = 64 * 1024 * 1024;

std::string uppercase_ascii(std::string text) {
    for(char& ch : text){
        if(ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return text;
}

void append_command(std::string& output, std::initializer_list<std::string> arguments) {
    output += "*" + std::to_string(arguments.size()) + "\r\n";
    for(const auto& argument : arguments){
        output += "$" + std::to_string(argument.size()) + "\r\n";
        output += argument;
        output += "\r\n";
    }
}

std::vector<std::string> affected_keys(const std::vector<std::string>& arguments) {
    std::vector<std::string> keys;
    if(arguments.empty()) return keys;
    const std::string name = uppercase_ascii(arguments[0]);

    if(name == "MSET"){
        for(std::size_t i = 1; i + 1 < arguments.size(); i += 2) keys.push_back(arguments[i]);
    } 
    else if(name == "DEL"){
        for(std::size_t i = 1; i < arguments.size(); i++) keys.push_back(arguments[i]);
    } 
    else if(arguments.size() >= 2 &&
              (name == "SET" || name == "GETDEL" || name == "INCR" ||
               name == "DECR" || name == "INCRBY" || name == "DECRBY" ||
               name == "EXPIRE" || name == "PEXPIRE" || name == "EXPIREAT" ||
               name == "PEXPIREAT" || name == "PERSIST" ||
               name == "LPUSH" || name == "RPUSH" || name == "LPOP" ||
               name == "RPOP" || name == "HSET" || name == "HDEL" ||
               name == "SADD" || name == "SREM")){
        keys.push_back(arguments[1]);
    }
    return keys;
}

std::string snapshot_keys(const std::vector<std::string>& keys, storage::Database& database) {
    std::string output;
    for(const auto& key : keys){
        // DEL first makes replay an assignment of the final state, not a repeat
        // of a conditional command or a relative TTL calculation.
        append_command(output, {"DEL", key});
        auto found = database.find_live(key);
        if(found == database.values.end()) continue;

        const auto& entry = found->second;
        if(const auto* value = std::get_if<std::string>(&entry.value)){
            append_command(output, {"SET", key, *value});
        } 
        else if(const auto* list = std::get_if<std::deque<std::string>>(&entry.value)){
            for(const auto& element : *list) append_command(output, {"RPUSH", key, element});
        } 
        else if(const auto* hash = std::get_if<storage::Hash>(&entry.value)){
            for(const auto& field : *hash) append_command(output, {"HSET", key, field.first, field.second});
        } 
        else if(const auto* set = std::get_if<storage::Set>(&entry.value)){
            for(const auto& member : *set) append_command(output, {"SADD", key, member});
        }
        if(entry.expires_at_ms){
            append_command(output, {"PEXPIREAT", key, std::to_string(*entry.expires_at_ms)});
        }
    }
    return output;
}

bool write_all(int fd, const char* data, std::size_t size, std::string& error) {
    std::size_t written = 0;
    while(written < size){
        const ssize_t result = write(fd, data + written, size - written);
        if(result > 0){
            written += static_cast<std::size_t>(result);
        } 
        else if(result < 0 && errno == EINTR){
            continue;
        } 
        else if(result == 0){
            error = "AOF write returned zero";
            return false;
        } 
        else {
            error = "AOF write failed: " + std::string(std::strerror(errno));
            return false;
        }
    }
    return true;
}

ssize_t read_up_to(int fd, char* data, std::size_t size) {
    std::size_t received = 0;
    while(received < size){
        const ssize_t result = read(fd, data + received, size - received);
        if(result > 0){
            received += static_cast<std::size_t>(result);
        } 
        else if(result == 0){
            break;
        } 
        else if(errno != EINTR){
            return -1;
        }
    }
    return static_cast<ssize_t>(received);
}

void encode_length(char header[8], std::uint64_t length) {
    for(int i = 0; i < 8; i++){
        header[i] = static_cast<char>(length & 0xff);
        length >>= 8;
    }
}

std::uint64_t decode_length(const char header[8]) {
    std::uint64_t length = 0;
    for(int i = 7; i >= 0; i--){
        length = (length << 8) | static_cast<unsigned char>(header[i]);
    }
    return length;
}

bool replay_record(const std::string& record, storage::Database& database,
                   std::string& error) {
    if(record.empty()){
        error = "AOF contains an empty record";
        return false;
    }
    std::size_t offset = 0;
    while(offset < record.size()){
        auto parsed = protocol::parse_request(record.substr(offset));
        if(parsed.status != protocol::ParseStatus::complete){
            error = "AOF contains an invalid request";
            return false;
        }
        const std::string reply = commands::execute_command(parsed.arguments, database);
        if(reply.empty() || reply[0] == '-'){
            error = "AOF replay command failed";
            return false;
        }
        offset += parsed.consumed;
    }
    return true;
}

}  // namespace

Aof::~Aof() {
    if(fd_ >= 0) close(fd_);
}

bool Aof::open_and_replay(const std::string& path, storage::Database& database,
                          std::string& error) {
    if(path.empty()) return true;
    fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(fd_ < 0){
        error = "AOF open failed: " + std::string(std::strerror(errno));
        return false;
    }

    off_t complete_end = 0;
    while(true){
        char header[8];
        const ssize_t header_size = read_up_to(fd_, header, sizeof(header));
        if(header_size < 0){
            error = "AOF read failed: " + std::string(std::strerror(errno));
            return false;
        }
        if(header_size == 0) break;
        if(header_size != static_cast<ssize_t>(sizeof(header))){
            if(ftruncate(fd_, complete_end) < 0){
                error = "AOF truncation failed: " + std::string(std::strerror(errno));
                return false;
            }
            break;
        }

        const std::uint64_t length = decode_length(header);
        if(length == 0 || length > max_record_size){
            error = "AOF record length is invalid";
            return false;
        }
        std::string record(static_cast<std::size_t>(length), '\0');
        const ssize_t record_size = read_up_to(fd_, record.data(), record.size());
        if(record_size < 0){
            error = "AOF read failed: " + std::string(std::strerror(errno));
            return false;
        }
        if(record_size != static_cast<ssize_t>(record.size())){
            if(ftruncate(fd_, complete_end) < 0){
                error = "AOF truncation failed: " + std::string(std::strerror(errno));
                return false;
            }
            break;
        }
        if(!replay_record(record, database, error)) return false;
        complete_end += static_cast<off_t>(sizeof(header) + record.size());
    }
    if(lseek(fd_, 0, SEEK_END) < 0){
        error = "AOF seek failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool Aof::append_after_command(const std::vector<std::string>& arguments,
                               const std::string& reply, storage::Database& database,
                               std::string& error) {
    if(fd_ < 0 || reply.empty() || reply[0] == '-') return true;
    const auto keys = affected_keys(arguments);
    if(keys.empty()) return true;

    const std::string record = snapshot_keys(keys, database);
    if(record.empty() || record.size() > max_record_size){
        error = "AOF record exceeds the 64 MiB limit";
        return false;
    }
    char header[8];
    encode_length(header, record.size());

    if(!write_all(fd_, header, sizeof(header), error) ||
       !write_all(fd_, record.data(), record.size(), error)) return false;

    if(fdatasync(fd_) < 0){
        error = "AOF sync failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

}  // namespace persistence
