#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include <chrono>

namespace storage {

struct Entry{
    std::string value;
    std::optional<std::int64_t> expires_at_ms;
};

struct Database {
    std::unordered_map<std::string, Entry> values;
    
    auto find_live(const std::string& key){// actually the return type is std::unordered_map<std::string, Entry>::iterator
        auto found = values.find(key);
        if(found==values.end() || !found->second.expires_at_ms){
            return found;
        }
        
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if(*found->second.expires_at_ms<=now_ms){
            values.erase(found);
            return values.end();
        }
        return found;
    }

    bool expires_at(const std::string& key, std::int64_t deadline_ms){
        auto found = find_live(key);
        if(found == values.end()){
            return false;
        }
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if(deadline_ms<=now_ms){
            values.erase(found);
        }
        else{
            found->second.expires_at_ms=deadline_ms;
        }
        return true;
    }
};
}  // namespace storage
