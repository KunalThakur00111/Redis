#include "snapshot.h"

#include "../storage/database.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <bit>

namespace persistence {
namespace {

constexpr std::string_view magic = "MRSNAP";// when loading they help identify weather the input looks like one of our snapshots.
constexpr std::uint64_t version = 1;

void append_unsigned(std::string& output, std::uint64_t value, std::size_t bytes) {
    for(std::size_t i = 0; i < bytes; i++){
        output.push_back(static_cast<char>(value & 0xff));// (value & 0xff) keeps only the lowest 8 bits
        //                                                   static_cast<char> makes that one byte appendable to the string
        value >>= 8;                                      // now next 8 bytes become the lowest 8 bits
    }
}

void append_string(std::string& output, const std::string& value){
    append_unsigned(output, value.size(), 8);
    output.append(value);
    // so after this size and string both is appended into output
}

void append_expiry(std::string& output, const storage::Entry& entry) {
    append_unsigned(output, entry.expires_at_ms.has_value() ? 1 : 0, 1);
    if(entry.expires_at_ms){
        const std::int64_t deadline = *entry.expires_at_ms;
        const auto bits = std::bit_cast<std::uint64_t>(deadline);
        append_unsigned(output, bits, 8);
    }
}

std::uint32_t checksum(std::string_view data) {
    std::uint32_t result = 0xffffffffU;
    for(unsigned char byte : data){
        result ^= byte;
        for(int bit = 0; bit < 8; bit++){
            if(result & 1U) result = (result >> 1) ^ 0xedb88320U;
            else result >>= 1;
        }
    }
    return ~result;
}

class Reader {
    std::string_view data_;// we can't change it, it is the view of the bytes
    std::size_t position_;// remember the next bytes position to read

public:
    explicit Reader(std::string_view data, std::size_t position = 0)
        : data_(data), position_(position) {}

    std::size_t remaining() const { return data_.size() - position_; }
    bool at_end() const { return position_ == data_.size(); }

    bool read_unsigned(std::uint64_t& value, std::size_t bytes) {
        if(bytes > remaining()) return false;
        value = 0;
        for(std::size_t i = 0; i < bytes; i++){
            value |= static_cast<std::uint64_t>(
                static_cast<unsigned char>(data_[position_++])) << (8 * i);// basically reverse of append unsigned
        }
        return true;
    }

    bool read_string(std::string& value) {
        std::uint64_t length;
        if(!read_unsigned(length, 8) || length > remaining()) return false;
        value = std::string(data_.substr(position_, static_cast<std::size_t>(length)));
        position_ += static_cast<std::size_t>(length);
        return true;
    }
};

bool read_entry(Reader& reader, storage::Entry& entry, std::string& error) {
    std::uint64_t type;
    std::uint64_t has_expiry;
    if(!reader.read_unsigned(type, 1) || !reader.read_unsigned(has_expiry, 1) ||
       has_expiry > 1){
        error = "Snapshot entry header is invalid";
        return false;
    }

    if(has_expiry){
        std::uint64_t bits;
        if(!reader.read_unsigned(bits, 8)){
            error = "Snapshot expiry is incomplete";
            return false;
        }
        std::int64_t deadline;
        std::memcpy(&deadline, &bits, sizeof(deadline));
        entry.expires_at_ms = deadline;
    }

    if(type == 1){ // key type is string
        std::string value;
        if(!reader.read_string(value)){
            error = "Snapshot string is incomplete";
            return false;
        }
        entry.value = std::move(value);
    } 

    else if(type == 2){ // key type is list
        // next 8 bytes stores how many elements it contains
        std::uint64_t count;
        if(!reader.read_unsigned(count, 8) || count > reader.remaining() / 8){//each element needs at least 8 bytes for its length
            error = "Snapshot list is invalid";
            return false;
        }

        std::deque<std::string> list;
        for(std::uint64_t i = 0; i < count; i++){
            std::string element;
            if(!reader.read_string(element)){
                error = "Snapshot list is incomplete";
                return false;
            }
            list.push_back(std::move(element));
        }
        entry.value = std::move(list);
    } 

    else if(type == 3){// type 3 is hash
        std::uint64_t count;
        if(!reader.read_unsigned(count, 8) || count > reader.remaining() / 16){// each pair contains two strings, that's why 16
            error = "Snapshot hash is invalid";
            return false;
        }
        storage::Hash hash;
        for(std::uint64_t i = 0; i < count; i++){
            std::string field;
            std::string value;
            if(!reader.read_string(field) || !reader.read_string(value) ||
               !hash.emplace(std::move(field), std::move(value)).second){
                error = "Snapshot hash field is invalid";
                return false;
            }
        }
        entry.value = std::move(hash);
    } 

    else if(type == 4){// type 4 is set
        std::uint64_t count;
        if(!reader.read_unsigned(count, 8) || count > reader.remaining() / 8){
            error = "Snapshot set is invalid";
            return false;
        }
        storage::Set set;
        for(std::uint64_t i = 0; i < count; i++){
            std::string member;
            if(!reader.read_string(member) || !set.insert(std::move(member)).second){
                error = "Snapshot set member is invalid";
                return false;
            }
        }
        entry.value = std::move(set);
    } 

    else {
        error = "Snapshot value type is unsupported";
        return false;
    }
    return true;
}

}

std::string encode_snapshot(const storage::Database& database, std::uint64_t aof_offset) {

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::uint64_t count = 0;
    for(const auto& item : database.values){
        if(!item.second.expires_at_ms || *item.second.expires_at_ms > now_ms) count++;
    }

    std::string output(magic);
    append_unsigned(output, version, 4);
    append_unsigned(output, aof_offset, 8);
    append_unsigned(output, count, 8);

    for(const auto& item : database.values){
        const storage::Entry& entry = item.second;
        if(entry.expires_at_ms && *entry.expires_at_ms <= now_ms) continue;

        append_string(output, item.first);// key
        if(const auto* value = std::get_if<std::string>(&entry.value)){
            append_unsigned(output, 1, 1);
            append_expiry(output, entry);
            append_string(output, *value);
        }

        else if(const auto* list = std::get_if<std::deque<std::string>>(&entry.value)){
            append_unsigned(output, 2, 1);
            append_expiry(output, entry);
            append_unsigned(output, list->size(), 8);
            for(const auto& element : *list) append_string(output, element);
        }

        else if(const auto* hash = std::get_if<storage::Hash>(&entry.value)){
            append_unsigned(output, 3, 1);
            append_expiry(output, entry);
            append_unsigned(output, hash->size(), 8);
            for(const auto& field : *hash){
                append_string(output, field.first);
                append_string(output, field.second);
            }
        }

        else if(const auto* set = std::get_if<storage::Set>(&entry.value)){
            append_unsigned(output, 4, 1);
            append_expiry(output, entry);
            append_unsigned(output, set->size(), 8);
            for(const auto& member : *set) append_string(output, member);
        }
    }
    append_unsigned(output, checksum(output), 4);
    return output;
}

bool decode_snapshot(std::string_view data, storage::Database& database,
                     std::uint64_t& aof_offset, std::string& error) {

    if(data.size() < magic.size() + 4 + 8 + 8 + 4){
        error = "Snapshot is incomplete";
        return false;
    }

    const std::string_view body = data.substr(0, data.size() - 4);

    Reader trailer(data.substr(data.size() - 4));
    std::uint64_t stored_checksum;
    trailer.read_unsigned(stored_checksum, 4);

    if(stored_checksum != checksum(body)){
        error = "Snapshot checksum does not match";
        return false;
    }
    if(body.substr(0, magic.size()) != magic){
        error = "Snapshot signature is invalid";
        return false;
    }

    Reader reader(body, magic.size());
    std::uint64_t file_version;
    std::uint64_t checkpoint;
    std::uint64_t count;
    if(!reader.read_unsigned(file_version, 4) || file_version != version ||
       !reader.read_unsigned(checkpoint, 8) || !reader.read_unsigned(count, 8)){
        error = "Snapshot version or header is invalid";
        return false;
    }

    storage::Database restored;
    for(std::uint64_t i = 0; i < count; i++){
        std::string key;
        storage::Entry entry;
        if(!reader.read_string(key)){
            error = "Snapshot key is incomplete";
            return false;
        }
        if(!read_entry(reader, entry, error)) return false;

        auto inserted = restored.values.emplace(std::move(key), std::move(entry));
        
        if(!inserted.second){
            error = "Snapshot contains a duplicate key";
            return false;
        }
        if(inserted.first->second.expires_at_ms) restored.track_expiry(inserted.first->first);
    }
    if(!reader.at_end()){
        error = "Snapshot has trailing data";
        return false;
    }

    restored.expire_some(restored.expiring_keys.size());
    database = std::move(restored);
    aof_offset = checkpoint;
    error.clear();
    return true;
}

}
