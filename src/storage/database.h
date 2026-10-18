#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include <chrono>
#include <cstddef>
#include <vector>
#include <deque>
#include <variant>

namespace storage {

struct Entry{
    std::variant<std::deque<std::string>, std::string> value;
    std::optional<std::int64_t> expires_at_ms;
};

struct Database {
    std::unordered_map<std::string, Entry> values;

    std::vector<std::string> expiring_keys;// only keeping keys that have an expiry 
    std::unordered_map<std::string, std::size_t> expiry_positions;// there positions, expiring_keys[expiry_positions[key]] == key
    std::size_t expiry_cursor=0;

    // Add a key to the expiry index once.
    void track_expiry(const std::string& key) {
        if (expiry_positions.find(key) != expiry_positions.end()) return;
        expiry_positions.emplace(key, expiring_keys.size());
        expiring_keys.push_back(key);
    }

    // Remove a key by replacing its slot with the last indexed key.
    void untrack_expiry(const std::string& key) {
        auto position = expiry_positions.find(key);
        if (position == expiry_positions.end()) return;

        const std::size_t index = position->second;
        const std::size_t last_index = expiring_keys.size() - 1;
        if (index != last_index) {
            expiring_keys[index] = expiring_keys[last_index];
            expiry_positions[expiring_keys[index]] = index;
        }
        expiring_keys.pop_back();
        expiry_positions.erase(position);
    }

    // Remove the map entry and its expiry-index entry together.
    void erase_entry(std::unordered_map<std::string, Entry>::iterator found) {
        untrack_expiry(found->first);
        values.erase(found);
    }
    
    auto find_live(const std::string& key){// actually the return type is std::unordered_map<std::string, Entry>::iterator
        auto found = values.find(key);
        if(found==values.end() || !found->second.expires_at_ms){
            return found;
        }
        
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if(*found->second.expires_at_ms<=now_ms){
            erase_entry(found);
            return values.end();
        }
        return found;
    }

    void expire_some(std::size_t max_checks) {
        std::size_t checked = 0;
        const std::size_t check_this_turn = max_checks < expiring_keys.size() ? max_checks : expiring_keys.size();
        while (checked < check_this_turn && !expiring_keys.empty()) {
            if (expiry_cursor >= expiring_keys.size()) {
                expiry_cursor = 0;
            }

            const std::string key = expiring_keys[expiry_cursor];
            const std::size_t size_before = expiring_keys.size();

            find_live(key);

            if (expiring_keys.size() == size_before) {
                expiry_cursor++;
            }
            checked++;
        }
    }

    bool expires_at(const std::string& key, std::int64_t deadline_ms){
        auto found = find_live(key);
        if(found == values.end()){
            return false;
        }
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if(deadline_ms<=now_ms){
            erase_entry(found);
        }
        else{
            found->second.expires_at_ms=deadline_ms;
            track_expiry(key);
        }
        return true;
    }

    void set(const std::string& key, const std::string& value, const std::optional<std::int64_t>& deadline_ms = std::nullopt){
        untrack_expiry(key);
        values[key] = Entry{value,deadline_ms};
        if(deadline_ms){
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            if (*deadline_ms <= now_ms) {
                values.erase(key);
            } else {
                track_expiry(key);
            }
        }
    }

    std::int64_t pttl(const std::string& key){
        auto found = values.find(key);
        if(found == values.end()){
            return -2;
        }

        if(!found->second.expires_at_ms){
            return -1;
        }

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const std::int64_t remaining = *found->second.expires_at_ms - now_ms;

        if(remaining > 0){
            return remaining;
        }

        erase_entry(found);
        return -2;
    }

    bool persist(const std::string& key){
        auto found = find_live(key);
        if(found == values.end()) return 0;
        if(!found->second.expires_at_ms) return 0;

        untrack_expiry(key);
        found->second.expires_at_ms.reset();
        return 1;
    }

    std::int64_t pexpiretime(const std::string& key) {
        auto found = find_live(key);
        if (found == values.end()) return -2;
        if (!found->second.expires_at_ms) return -1;
        return *found->second.expires_at_ms;
    }

    bool erase_live(const std::string& key){
        auto found = find_live(key);
        if(found == values.end()){
            return false;
        }
        erase_entry(found);
        return true;
    }

    std::optional<std::size_t> lpush(const std::string& key, const std::string& element){
        auto found = find_live(key);

        if(found == values.end()){
            values.emplace(key, Entry{std::deque<std::string>{element},std::nullopt});
            return 1;
        }

        auto* list = std::get_if<std::deque<std::string>>(&found->second.value);
        if(list == nullptr) return std::nullopt;
        list->push_front(element);
        return list->size();
    }

    std::optional<std::vector<std::string>> lrange(const std::string& key, std::int64_t start, std::int64_t stop){
        auto found = find_live(key);
        if(found == values.end()) return std::vector<std::string>{};

        auto *ptr = std::get_if<std::deque<std::string>>(&found->second.value);
        if(ptr == nullptr){
            return std::nullopt;
        }

        std::vector<std::string> lists;
        std::int64_t size = static_cast<std::int64_t>(ptr->size());

        if(start < 0) start += size;
        if(stop < 0 ) stop += size; 

        if(start < 0) start = 0;
        if(stop < 0 || start >= size || start > stop) return lists;
        if(stop >= size) stop = size - 1;

        for(std::int64_t i = start; i <= stop; i++){
            lists.emplace_back((*ptr)[static_cast<std::size_t>(i)]);
        }
        return lists;
    }

};
}  // namespace storage
