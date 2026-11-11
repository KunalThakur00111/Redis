#pragma once

#include <string_view>

namespace commands {

// Redis-style glob matching, used by KEYS, SCAN MATCH and CONFIG GET.
//   *       any sequence, including an empty one
//   ?       exactly one byte
//   [abc]   one byte from the set; [^abc] one byte not in it; [a-z] a range
//   \x      the byte x itself
// Matching works on bytes, so keys containing any byte value can be matched.
// nocase compares ASCII letters without regard to case.
bool glob_match(std::string_view pattern, std::string_view text, bool nocase = false);

}  // namespace commands
