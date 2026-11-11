#include "glob.h"

#include <cstddef>

namespace commands {
namespace {

unsigned char fold(unsigned char ch, bool nocase) {
    if(nocase && ch >= 'A' && ch <= 'Z') return static_cast<unsigned char>(ch - 'A' + 'a');
    return ch;
}

// Match one [...] class starting just after '['. On return, p points just past
// the closing ']' (or at the end of the pattern when the class is unclosed,
// which Redis also accepts).
bool match_class(std::string_view pattern, std::size_t& p, unsigned char ch, bool nocase) {
    bool negate = false;
    if(p < pattern.size() && pattern[p] == '^'){
        negate = true;
        p++;
    }
    bool matched = false;
    ch = fold(ch, nocase);
    while(p < pattern.size() && pattern[p] != ']'){
        if(pattern[p] == '\\' && p + 1 < pattern.size()){
            p++;
            if(fold(static_cast<unsigned char>(pattern[p]), nocase) == ch) matched = true;
            p++;
        }
        else if(p + 2 < pattern.size() && pattern[p + 1] == '-'){
            unsigned char start = fold(static_cast<unsigned char>(pattern[p]), nocase);
            unsigned char end = fold(static_cast<unsigned char>(pattern[p + 2]), nocase);
            if(start > end){
                const unsigned char swap = start;
                start = end;
                end = swap;
            }
            if(ch >= start && ch <= end) matched = true;
            p += 3;
        }
        else {
            if(fold(static_cast<unsigned char>(pattern[p]), nocase) == ch) matched = true;
            p++;
        }
    }
    if(p < pattern.size()) p++;// skip the closing ']'
    return negate ? !matched : matched;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text, bool nocase) {

    /*
    Walk both strings once. When a '*' is seen, remember where it was and where
    the text stood; on a later mismatch, let that '*' absorb one more byte and
    retry from there. This is linear backtracking with only the latest '*'
    remembered, which avoids the exponential cost of recursing on every '*'.
    */
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star_p = std::string_view::npos;// position just after the latest '*'
    std::size_t star_t = 0;// text position that '*' currently starts absorbing from

    while(t < text.size()){
        if(p < pattern.size()){
            const char token = pattern[p];
            if(token == '*'){
                while(p < pattern.size() && pattern[p] == '*') p++;// "**" is the same as "*"
                if(p == pattern.size()) return true;
                star_p = p;
                star_t = t;
                continue;
            }
            const unsigned char ch = static_cast<unsigned char>(text[t]);
            std::size_t next = p + 1;
            bool matched;
            if(token == '?'){
                matched = true;
            }
            else if(token == '['){
                matched = match_class(pattern, next, ch, nocase);
            }
            else if(token == '\\' && p + 1 < pattern.size()){
                matched = fold(static_cast<unsigned char>(pattern[p + 1]), nocase) == fold(ch, nocase);
                next = p + 2;
            }
            else {
                matched = fold(static_cast<unsigned char>(token), nocase) == fold(ch, nocase);
            }
            if(matched){
                p = next;
                t++;
                continue;
            }
        }
        if(star_p == std::string_view::npos) return false;
        p = star_p;
        t = ++star_t;
    }

    // The text is used up; only '*' may remain in the pattern.
    while(p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

}  // namespace commands
