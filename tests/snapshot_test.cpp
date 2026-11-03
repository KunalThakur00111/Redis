#include "../src/persistence/snapshot.h"
#include "../src/storage/database.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

}

int main() {
    try {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t deadline = now_ms + 60000;

        storage::Database original;
        const std::string binary_key("key\0part", 8);
        const std::string binary_value("one\0two", 7);
        original.set(binary_key, binary_value);
        original.values.emplace("list", storage::Entry{
            std::deque<std::string>{"first", std::string("\0last", 5)}, deadline});
        original.track_expiry("list");
        original.values.emplace("hash", storage::Entry{
            storage::Hash{{"name", "Kunal"}, {"place", "Delhi"}}, std::nullopt});
        original.values.emplace("set", storage::Entry{
            storage::Set{"one", "two"}, std::nullopt});
        original.values.emplace("expired", storage::Entry{
            std::string("old"), now_ms - 1000});
        original.track_expiry("expired");

        const std::string encoded = persistence::encode_snapshot(original, 12345);
        storage::Database restored;
        restored.set("stale", "remove me");
        std::uint64_t offset = 0;
        std::string error;
        check(persistence::decode_snapshot(encoded, restored, offset, error),
              "snapshot did not decode");
        check(offset == 12345, "AOF checkpoint was not restored");
        check(restored.values.size() == 4 && restored.values.count("stale") == 0 &&
              restored.values.count("expired") == 0,
              "snapshot did not replace the database or skip expired keys");
        check(std::get<std::string>(restored.values.at(binary_key).value) == binary_value,
              "binary string did not round-trip");
        check(std::get<std::deque<std::string>>(restored.values.at("list").value) ==
                  std::get<std::deque<std::string>>(original.values.at("list").value),
              "list did not round-trip");
        check(restored.values.at("list").expires_at_ms == deadline &&
              restored.expiry_positions.count("list") == 1,
              "expiry deadline or index did not round-trip");
        check(std::get<storage::Hash>(restored.values.at("hash").value) ==
                  std::get<storage::Hash>(original.values.at("hash").value),
              "hash did not round-trip");
        check(std::get<storage::Set>(restored.values.at("set").value) ==
                  std::get<storage::Set>(original.values.at("set").value),
              "set did not round-trip");

        std::string damaged = encoded;
        damaged[damaged.size() / 2] ^= 1;
        check(!persistence::decode_snapshot(damaged, restored, offset, error),
              "damaged snapshot was accepted");
        check(restored.values.size() == 4 && offset == 12345,
              "damaged snapshot changed the destination");

        for(std::size_t length = 0; length < encoded.size(); length++){
            check(!persistence::decode_snapshot(
                      std::string_view(encoded.data(), length), restored, offset, error),
                  "truncated snapshot was accepted");
        }
        check(restored.values.size() == 4 && offset == 12345,
              "truncated snapshot changed the destination");

        std::cout << "Snapshot tests passed\n";
        return 0;
    } catch(const std::exception& failure){
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
