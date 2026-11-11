// Check Redis-style glob matching used by KEYS, SCAN MATCH and CONFIG GET.
#include "../src/commands/glob.h"

#include <iostream>
#include <string>

using namespace std::string_literals;

namespace {
int failures = 0;

void expect(bool expected, std::string_view pattern, std::string_view text, bool nocase = false) {
    if(commands::glob_match(pattern, text, nocase) != expected){
        std::cerr << "pattern '" << pattern << "' on '" << text << "' should "
                  << (expected ? "" : "not ") << "match\n";
        failures++;
    }
}
}  // namespace

int main() {
    expect(true, "*", "");
    expect(true, "*", "anything");
    expect(true, "", "");
    expect(false, "", "a");
    expect(true, "user:*", "user:1");
    expect(false, "user:*", "users:1");
    expect(true, "*:1", "user:1");
    expect(true, "a*b*c", "aXXbYYc");
    expect(false, "a*b*c", "aXXbYY");
    expect(true, "a**b", "ab");
    expect(true, "h?llo", "hello");
    expect(false, "h?llo", "hllo");
    expect(true, "h[ae]llo", "hallo");
    expect(false, "h[ae]llo", "hillo");
    expect(true, "h[^e]llo", "hallo");
    expect(false, "h[^e]llo", "hello");
    expect(true, "h[a-b]llo", "hbllo");
    expect(true, "h[b-a]llo", "hallo");// a reversed range still matches
    expect(false, "h[a-b]llo", "hcllo");
    expect(true, "h\\*llo", "h*llo");
    expect(false, "h\\*llo", "hello");
    expect(true, "[\\]]", "]");
    expect(true, "*\\?", "why?");
    expect(true, "a\0b*"s, "a\0bc"s);// bytes, not C strings
    expect(false, "a\0b"s, "a"s);
    expect(true, "APPEND*", "appendonly", true);
    expect(false, "APPEND*", "appendonly", false);
    expect(true, "[A-Z]x", "qx", true);
    // Backtracking stays linear: this would take forever if each '*' recursed.
    expect(false, "a*a*a*a*a*a*a*a*a*a*a*a*b", std::string(5000, 'a'));
    return failures == 0 ? 0 : 1;
}
