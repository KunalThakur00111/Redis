#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace storage { struct Database; }// snapshot.cpp includes header of database cause that file access it's member

namespace persistence {

// The caller must keep the database stable while encoding and capture the AOF
// position under the same database lock.
std::string encode_snapshot(const storage::Database& database, std::uint64_t aof_offset);

// Decode into a temporary database, replacing the destination only on success.
bool decode_snapshot(std::string_view data, storage::Database& database,
                     std::uint64_t& aof_offset, std::string& error);

}
// encoder says:- how do i turn by database into bytes
// decoder says:- how do i rebuild my database from those bytes?
