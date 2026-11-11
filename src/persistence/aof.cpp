#include "aof.h"

#include "../commands/command_handler.h"
#include "../protocol/resp_parser.h"
#include "../storage/database.h"
#include "snapshot.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
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

std::string checkpoint_record(std::string_view checksum_bytes) {
    std::string marker = "MRSNAP:";
    marker.append(checksum_bytes);
    std::string record;
    append_command(record, {"PING", marker});
    return record;
}

std::size_t checkpoint_frame_size() {
    return 8 + checkpoint_record(std::string(4, '\0')).size();
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
    else if(arguments.size() >= 3 && (name == "RENAME" || name == "RENAMENX" ||
                                      name == "SMOVE")){
        // Both keys change: the source loses something and the destination gains it.
        keys.push_back(arguments[1]);
        keys.push_back(arguments[2]);
    } 
    else if(arguments.size() >= 2 &&
              (name == "SET" || name == "GETDEL" || name == "INCR" ||
               name == "DECR" || name == "INCRBY" || name == "DECRBY" ||
               name == "EXPIRE" || name == "PEXPIRE" || name == "EXPIREAT" ||
               name == "PEXPIREAT" || name == "PERSIST" ||
               name == "LPUSH" || name == "RPUSH" || name == "LPOP" ||
               name == "RPOP" || name == "HSET" || name == "HDEL" ||
               name == "SADD" || name == "SREM" ||
               name == "APPEND" || name == "SETNX" || name == "GETSET" ||
               name == "INCRBYFLOAT" || name == "LSET" || name == "LREM" ||
               name == "LTRIM" || name == "LINSERT" || name == "HMSET" ||
               name == "HSETNX" || name == "HINCRBY" || name == "HINCRBYFLOAT" ||
               name == "SPOP" ||
               // For the *STORE commands, arguments[1] is the destination.
               name == "SINTERSTORE" || name == "SUNIONSTORE" || name == "SDIFFSTORE")){
        keys.push_back(arguments[1]);
    }
    return keys;
}

// Append the commands that recreate one live key, including its absolute expiry.
void append_entry(std::string& output, const std::string& key, const storage::Entry& entry) {
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

std::string snapshot_keys(const std::vector<std::string>& keys, storage::Database& database) {
    std::string output;
    for(const auto& key : keys){
        // DEL first makes replay an assignment of the final state, not a repeat
        // of a conditional command or a relative TTL calculation.
        append_command(output, {"DEL", key});
        auto found = database.find_live(key);
        if(found == database.values.end()) continue;
        append_entry(output, key, found->second);
    }
    return output;
}

std::string error_text(const char* what, int number) {
    return std::string(what) + ": " + std::strerror(number);
}

// A crash during rewrite() leaves the temporary files it was writing. Delete
// those named exactly like mkstemp() output for this AOF: prefix plus six characters.
void remove_stale_temporaries(const std::string& aof_path) {
    namespace fs = std::filesystem;
    const fs::path path(aof_path);
    const std::string name = path.filename().string();
    const std::string prefixes[] = {name + ".rewrite.", name + ".snapshot.tmp."};
    const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::path(".");

    // This is best effort, so every call uses an error_code and never throws.
    // A range-for would use the throwing operator++, so increment(error) is
    // called instead, and the loop stops at the first read error.
    std::error_code error;
    fs::directory_iterator item(directory, error);
    for(; !error && item != fs::directory_iterator(); item.increment(error)){
        const std::string candidate = item->path().filename().string();
        for(const auto& prefix : prefixes){
            std::error_code ignored;
            if(candidate.size() == prefix.size() + 6 &&
               candidate.compare(0, prefix.size(), prefix) == 0 &&
               item->is_regular_file(ignored)){
                fs::remove(item->path(), ignored);
            }
        }
    }
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

int sync_file(int fd) {
    int result;
    do {
        result = fdatasync(fd);
    } while(result < 0 && errno == EINTR);
    return result;
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

// Write one length-prefixed record.
bool write_frame(int fd, std::string_view record, std::string& error) {
    char header[8];
    encode_length(header, record.size());
    return write_all(fd, header, sizeof(header), error) &&
           write_all(fd, record.data(), record.size(), error);
}

bool checkpoint_matches(int fd, std::uint64_t offset, std::string_view checksum_bytes) {

    const std::string expected = checkpoint_record(checksum_bytes);

    const std::size_t frame_size = sizeof(std::uint64_t) + expected.size();
    if(offset < frame_size) return false;

    if(lseek(fd, static_cast<off_t>(offset - frame_size), SEEK_SET) < 0) return false;
    char header[8];

    if(read_up_to(fd, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
       decode_length(header) != expected.size()) return false;

    std::string actual(expected.size(), '\0');
    return read_up_to(fd, actual.data(), actual.size()) ==
               static_cast<ssize_t>(actual.size()) && actual == expected;
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
    if(sync_thread_.joinable()){
        std::string ignored_error;
        stop_and_sync(ignored_error);
    }
    if(fd_ >= 0) close(fd_);
}

bool Aof::stop_and_sync(std::string& error) {
    if(sync_thread_.joinable()){
        {
            std::lock_guard<std::mutex> lock(sync_mutex_);
            stop_sync_ = true;
        }
        sync_cv_.notify_one();
        sync_thread_.join();
    }
    return check_sync_error(error);
}

bool Aof::open_and_replay(const std::string& path, storage::Database& database,
                          std::string& error, std::uint64_t snapshot_offset,
                          std::string_view snapshot_checksum) {

    if(path.empty()) return true;
    path_ = path;
    remove_stale_temporaries(path);

    fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC |
               (snapshot_checksum.empty() ? O_CREAT : 0), 0600);
               
    if(fd_ < 0){
        error = "AOF open failed: " + std::string(std::strerror(errno));
        return false;
    }

    off_t complete_end = 0;
    if(!snapshot_checksum.empty()){
        const off_t end = lseek(fd_, 0, SEEK_END);
        if(end < 0){
            error = "AOF seek failed: " + std::string(std::strerror(errno));
            return false;
        }
        if(snapshot_offset > static_cast<std::uint64_t>(end)){
            error = "AOF ends before the snapshot checkpoint";
            return false;
        }
        if(snapshot_checksum.size() == 4 && snapshot_offset > 0 &&
           checkpoint_matches(fd_, snapshot_offset, snapshot_checksum)){
            complete_end = static_cast<off_t>(snapshot_offset);
        } 
        else {
            database = storage::Database{};// if not matches, then it clears the database
        }
        if(lseek(fd_, complete_end, SEEK_SET) < 0){// it set the position at the exact complete_end position from the start of file
            error = "AOF seek failed: " + std::string(std::strerror(errno));
            return false;
        }
    }

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

    const off_t end = lseek(fd_, 0, SEEK_END);// puts the file position excatly at the end, ready for future writes 
    if(end < 0){
        error = "AOF seek failed: " + std::string(std::strerror(errno));
        return false;
    }
    rewrite_base_size_ = static_cast<std::uint64_t>(end);
    last_save_ = std::chrono::steady_clock::now();
    return start_sync_thread(error);
}

bool Aof::start_sync_thread(std::string& error) {
    if(sync_policy_ != SyncPolicy::every_second) return true;
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        stop_sync_ = false;
    }
    try {
        sync_thread_ = std::thread(&Aof::sync_loop, this);// 'this' tells run this function on this particular AOF object.
    } 
    catch(const std::system_error& failure) {
        error = "AOF sync worker failed: " + std::string(failure.what());
        return false;
    }
    return true;
}

bool Aof::append_after_command(const std::vector<std::string>& arguments,
                               const std::string& reply, storage::Database& database,
                               std::string& error) {

    if(!check_sync_error(error)) return false;
    if(fd_ < 0 || reply.empty() || reply[0] == '-') return true;

    std::string record;
    const std::string name = arguments.empty() ? "" : uppercase_ascii(arguments[0]);
    if(name == "FLUSHDB" || name == "FLUSHALL"){
        // The keys are already gone, so log the command itself; replaying it
        // empties the database at the same point in the log.
        append_command(record, {"FLUSHALL"});
    } 
    else {
        const auto keys = affected_keys(arguments);
        if(keys.empty()) return true;
        record = snapshot_keys(keys, database);
    }
    if(record.empty() || record.size() > max_record_size){
        error = "AOF record exceeds the 64 MiB limit";
        return false;
    }
    char header[8];
    encode_length(header, record.size());

    if(!write_all(fd_, header, sizeof(header), error) ||
       !write_all(fd_, record.data(), record.size(), error)) return false;
    changes_since_save_++;

    if(sync_policy_ == SyncPolicy::every_second){
        {
            std::lock_guard<std::mutex> lock(sync_mutex_);
            if(!dirty_){
                next_sync_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            dirty_ = true;
            write_generation_++;
        }// here '}' unlocks the lock 
        sync_cv_.notify_one();// wakes the condition variable 
        return check_sync_error(error);
    }

    if(sync_file(fd_) < 0){
        error = "AOF sync failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool Aof::current_offset(std::uint64_t& offset, std::string& error) const {
    if(fd_ < 0){
        error = "AOF is not open";
        return false;
    }

    // 2nd argument of lseek tells move x bytes(here 0) from the 3rd argument position (which is current here).
    const off_t position = lseek(fd_, 0, SEEK_CUR);// where is the current position..
    if(position < 0){
        error = "AOF position failed: " + std::string(std::strerror(errno));
        return false;
    }
    
    offset = static_cast<std::uint64_t>(position);
    return true;
}

bool Aof::next_snapshot_offset(std::uint64_t& offset, std::string& error) const {
    std::uint64_t current;
    if(!current_offset(current, error)) return false;
    const std::size_t frame_size = checkpoint_frame_size();
    if(current > std::numeric_limits<std::uint64_t>::max() - frame_size){
        error = "AOF snapshot checkpoint exceeds the offset limit";
        return false;
    }
    offset = current + frame_size;
    return true;
}

bool Aof::append_snapshot_checkpoint(std::string_view snapshot,
                                     std::uint64_t expected_offset, std::string& error) {
    if(snapshot.size() < 4){
        error = "Snapshot checksum is missing";
        return false;
    }
    std::uint64_t next_offset;
    if(!next_snapshot_offset(next_offset, error)) return false;
    if(next_offset != expected_offset){
        error = "AOF changed during snapshot capture";
        return false;
    }
    const std::string record = checkpoint_record(snapshot.substr(snapshot.size() - 4));
    char header[8];
    encode_length(header, record.size());
    if(!write_all(fd_, header, sizeof(header), error) ||
       !write_all(fd_, record.data(), record.size(), error)) return false;
    if(sync_file(fd_) < 0){
        error = "AOF checkpoint sync failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

void Aof::set_schedule(std::chrono::seconds save_interval, std::uint64_t rewrite_min_size) {
    save_interval_ = save_interval;
    rewrite_min_size_ = rewrite_min_size;
}

PersistResult Aof::save_snapshot(storage::Database& database, std::string& error) {
    if(fd_ < 0){
        error = "AOF is not enabled";
        return PersistResult::failed;
    }
    if(!check_sync_error(error)) return PersistResult::fatal;

    std::uint64_t aof_offset;
    if(!next_snapshot_offset(aof_offset, error)) return PersistResult::fatal;

    const std::string data = encode_snapshot(database, aof_offset);
    // A failed checkpoint write may leave a partial record, so later appends
    // would land after garbage. That is why this failure stops the server.
    if(!append_snapshot_checkpoint(data, aof_offset, error)) return PersistResult::fatal;

    // Without the snapshot file, startup replays the whole AOF, which is still correct.
    if(!save_snapshot_file(snapshot_path(), data, error)) return PersistResult::failed;

    changes_since_save_ = 0;
    last_save_ = std::chrono::steady_clock::now();
    return PersistResult::ok;
}

PersistResult Aof::rewrite(storage::Database& database, std::string& error) {

    /*
    Replace the AOF with the shortest log that rebuilds the current database,
    followed by a checkpoint record, and save a matching snapshot.

    The new log is complete on its own, so a missing or damaged snapshot still
    restores everything through a full replay. Files are switched in this order
    so a crash at any point leaves a state startup can load:

      1. delete the old snapshot  -> startup fully replays the old AOF
      2. rename the new AOF       -> startup fully replays the new AOF
      3. rename the new snapshot  -> startup loads it and replays nothing
    */
    if(fd_ < 0){
        error = "AOF is not enabled";
        return PersistResult::failed;
    }
    if(!check_sync_error(error)) return PersistResult::fatal;

    // Collect names first: find_live() erases an expired key, which would
    // invalidate an iterator over database.values.
    std::vector<std::string> keys;
    keys.reserve(database.values.size());
    for(const auto& item : database.values) keys.push_back(item.first);

    std::string aof_temporary = path_ + ".rewrite.XXXXXX";
    const int new_fd = mkstemp(aof_temporary.data());
    if(new_fd < 0){
        error = error_text("AOF rewrite file creation failed", errno);
        return PersistResult::failed;
    }
    std::string snapshot_temporary;
    auto abandon = [&]{
        close(new_fd);
        unlink(aof_temporary.c_str());
        if(!snapshot_temporary.empty()) unlink(snapshot_temporary.c_str());
    };

    if(fcntl(new_fd, F_SETFD, FD_CLOEXEC) < 0){
        error = error_text("AOF rewrite file setup failed", errno);
        abandon();
        return PersistResult::failed;
    }

    // One record per key keeps each record under the replay size limit.
    std::uint64_t offset = 0;
    std::string record;
    for(const auto& key : keys){
        auto found = database.find_live(key);
        if(found == database.values.end()) continue;
        record.clear();
        append_entry(record, key, found->second);
        if(record.size() > max_record_size){
            error = "AOF rewrite record exceeds the 64 MiB limit";
            abandon();
            return PersistResult::failed;
        }
        if(!write_frame(new_fd, record, error)){
            abandon();
            return PersistResult::failed;
        }
        offset += sizeof(std::uint64_t) + record.size();
    }

    offset += checkpoint_frame_size();
    const std::string snapshot = encode_snapshot(database, offset);
    if(!write_frame(new_fd, checkpoint_record(std::string_view(snapshot).substr(snapshot.size() - 4)),
                    error)){
        abandon();
        return PersistResult::failed;
    }
    if(sync_file(new_fd) < 0){
        error = error_text("AOF rewrite sync failed", errno);
        abandon();
        return PersistResult::failed;
    }
    if(!write_temporary_file(snapshot_path(), snapshot, snapshot_temporary, error)){
        snapshot_temporary.clear();
        abandon();
        return PersistResult::failed;
    }

    // Stop the sync worker so it never uses the old descriptor during the swap.
    if(!stop_and_sync(error)){
        abandon();
        return PersistResult::fatal;
    }

    // Step 1: no snapshot means a full replay of whichever AOF is in place.
    bool removed = true;
    if(unlink(snapshot_path().c_str()) < 0 && errno != ENOENT){
        error = error_text("Old snapshot removal failed", errno);
        removed = false;
    }
    if(!removed || !sync_parent_directory(path_, error)){
        abandon();
        return start_sync_thread(error) ? PersistResult::failed : PersistResult::fatal;
    }

    // Step 2: the new log takes the old log's name.
    if(rename(aof_temporary.c_str(), path_.c_str()) < 0){
        error = error_text("AOF rewrite rename failed", errno);
        abandon();
        return start_sync_thread(error) ? PersistResult::failed : PersistResult::fatal;
    }
    close(fd_);
    fd_ = new_fd;// it is already positioned at the end, ready for future writes
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        dirty_ = false;
    }
    rewrite_base_size_ = offset;
    changes_since_save_ = 0;
    last_save_ = std::chrono::steady_clock::now();

    // If this rename may not survive a crash, later writes to the new file may
    // vanish with it, so continuing would be unsafe.
    if(!sync_parent_directory(path_, error)){
        unlink(snapshot_temporary.c_str());
        return PersistResult::fatal;
    }
    if(!start_sync_thread(error)){
        unlink(snapshot_temporary.c_str());
        return PersistResult::fatal;
    }

    // Step 3: without it, startup still fully replays the new log.
    if(rename(snapshot_temporary.c_str(), snapshot_path().c_str()) < 0){
        error = error_text("Snapshot rename failed", errno);
        unlink(snapshot_temporary.c_str());
        return PersistResult::failed;
    }
    if(!sync_parent_directory(path_, error)) return PersistResult::failed;
    return PersistResult::ok;
}

PersistResult Aof::run_scheduled(storage::Database& database, std::string& error) {
    if(fd_ < 0) return PersistResult::ok;
    const auto now = std::chrono::steady_clock::now();
    if(now < next_scheduled_attempt_) return PersistResult::ok;

    std::uint64_t size;
    if(!current_offset(size, error)) return PersistResult::fatal;

    PersistResult result = PersistResult::ok;
    if(rewrite_min_size_ > 0 && size >= rewrite_min_size_ &&
       size / 2 >= rewrite_base_size_){
        result = rewrite(database, error);
    } 
    else if(save_interval_.count() > 0 && changes_since_save_ > 0 &&
              now - last_save_ >= save_interval_){
        result = save_snapshot(database, error);
    }

    // Retry a failed attempt later instead of every loop iteration.
    if(result == PersistResult::failed){
        next_scheduled_attempt_ = now + std::chrono::seconds(5);
    }
    return result;
}

bool Aof::check_sync_error(std::string& error) {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    if(!sync_error_.empty()){
        error = sync_error_;
        return false;
    }
    return true;
}

void Aof::sync_loop() {
    std::unique_lock<std::mutex> lock(sync_mutex_);// we use unique lock becuase function needs to unlock and lock later.
    //                                                unique lock provide manual locking and unlocking
    while(true){
        if(!dirty_){
            if(stop_sync_) return;
            sync_cv_.wait(lock, [this]{ return dirty_ || stop_sync_; }/*lamdda function*/);// put the worker to sleep
            //                                                                                and this wait() function unlocks the sync mutex
            //                                                                                and on return reaquire the lock
            continue;
        }
        if(!stop_sync_ && std::chrono::steady_clock::now() < next_sync_){
            sync_cv_.wait_until(lock, next_sync_);
            continue;
        }

        const std::uint64_t syncing_generation = write_generation_;
        const int fd = fd_;// rewrite() changes fd_ only after joining this thread
        lock.unlock();
        const int result = sync_file(fd);
        const int sync_errno = errno;
        lock.lock();
        if(result < 0){ 
            sync_error_ = "AOF sync failed: " +
                          std::error_code(sync_errno, std::generic_category()).message();
            return;
        }
        if(write_generation_ == syncing_generation){// checks if inbetween the syncing, is there any records to sync has come or not
            dirty_ = false;
        } else {
            next_sync_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }
}

}  // namespace persistence
