#pragma once

#include "../storage/database.h"

#include <string>
#include <vector>

namespace commands {

// Run a sorted-set command (ZADD, ZRANGE, ...). name is already uppercase.
// Returns an empty string when name is not a sorted-set command.
std::string execute_zset_command(const std::string& name,
                                 const std::vector<std::string>& arguments,
                                 storage::Database& database);

// The text Redis sends for a score: an integer when the value is one (within
// +-2^62), otherwise the shortest digits that read back as the same double, in
// printf("%.17g") layout. Also "inf", "-inf" and "-0".
std::string format_score(double score);

}  // namespace commands
