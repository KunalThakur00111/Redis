#include "snapshot.h"

#include "../storage/database.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
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

bool save_snapshot_file(const std::string& path, std::string_view data,
                        std::string& error) {

    /*
    Save the new snapshot without overwriting the old one while writing.

    First, create a temporary file in the same directory as the final snapshot
    and write and sync all its bytes. Until then, the old snapshot remains intact.

    rename(temporary, path) atomically changes the final filename to refer to the
    complete new file. The temporary filename disappears. The old file loses its
    name and is reclaimed once nothing else has it open.

    The paths passed to rename() already identify the parent directory. We open
    that directory separately only so fsync(directory_fd) can persist the filename
    change. fdatasync(fd) persists the new file's contents; fsync(directory_fd)
    persists the directory entry that names it.
    */                        
    std::string temporary = path + ".tmp.XXXXXX";

    int fd = mkstemp(temporary.data());// mkstemp replaces the six X characters with a unique suffix, creates that file, and opens it.
    //                                    it returns a file descriptor on success, or -1 on failure
    if(fd < 0){
        error = "Snapshot temporary file creation failed: " +
                std::string(std::strerror(errno));
        return false;
    }

    std::size_t written = 0;
    while(written < data.size()){
        const ssize_t result = write(fd, data.data() + written, data.size() - written);
        if(result > 0){
            written += static_cast<std::size_t>(result);
        } else if(result < 0 && errno == EINTR){
            continue;
        } else {
            const int saved_errno = result == 0 ? EIO : errno;
            close(fd);
            unlink(temporary.c_str());
            error = "Snapshot write failed: " + std::string(std::strerror(saved_errno));
            return false;
        }
    }

    int sync_result;
    do {
        sync_result = fdatasync(fd);
    } while(sync_result < 0 && errno == EINTR);

    if(sync_result < 0){
        const int saved_errno = errno;
        close(fd);
        unlink(temporary.c_str());
        error = "Snapshot sync failed: " + std::string(std::strerror(saved_errno));
        return false;
    }

    if(close(fd) < 0){
        const int saved_errno = errno;
        unlink(temporary.c_str());
        error = "Snapshot close failed: " + std::string(std::strerror(saved_errno));
        return false;
    }

    // find the parent directory
    const std::size_t slash = path.find_last_of('/');// find the last slash in the final filename.
    const std::string directory = slash == std::string::npos ? "." :
                                  slash == 0 ? "/" : path.substr(0, slash);

                            
    // first argument: directory path as a c string
    // O_RDONLY: open for reading, O_DIRECTORY: require this path to be a directory, O_CLOSEXEC: close if run different program
    int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(directory_fd < 0){
        const int saved_errno = errno;
        unlink(temporary.c_str());
        error = "Snapshot directory open failed: " +
                std::string(std::strerror(saved_errno));
        return false;
    }

    // rename(old_path, new_path)
    if(rename(temporary.c_str(), path.c_str()) < 0){
        const int saved_errno = errno;
        close(directory_fd);
        unlink(temporary.c_str());
        error = "Snapshot rename failed: " + std::string(std::strerror(saved_errno));
        return false;
    }
    // after a successful rename , the temperory file name is gone; it's context are now reachable at path.

    if(fsync(directory_fd) < 0){
        const int saved_errno = errno;
        close(directory_fd);
        error = "Snapshot directory sync failed: " +
                std::string(std::strerror(saved_errno));
        return false;
    }

    if(close(directory_fd) < 0){
        error = "Snapshot directory close failed: " +
                std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool load_snapshot_file(const std::string& path, std::string& data,
                        bool& found, std::string& error) {

    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0){
        if(errno == ENOENT){
            found = false;
            return true;
        }
        error = "Snapshot open failed: " + std::string(std::strerror(errno));
        return false;
    }

    std::string loaded;
    char buffer[65536];
    while(true){
        const ssize_t result = read(fd, buffer, sizeof(buffer));
        if(result > 0){
            loaded.append(buffer, static_cast<std::size_t>(result));
        } 
        else if(result == 0){
            break;
        } 
        else if(errno != EINTR){
            const int saved_errno = errno;
            close(fd);
            error = "Snapshot read failed: " + std::string(std::strerror(saved_errno));
            return false;
        }
    }
    if(close(fd) < 0){
        error = "Snapshot close failed: " + std::string(std::strerror(errno));
        return false;
    }
    data = std::move(loaded);
    found = true;
    return true;
}

}
