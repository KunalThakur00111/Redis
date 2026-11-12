#include "zset_commands.h"

#include "../protocol/resp_writer.h"

#include <cerrno>
#include <charconv>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace commands {
namespace {

using storage::SkipList;
using storage::SortedSet;
using Node = SkipList::Node;

constexpr const char* wrong_type_error =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
constexpr const char* syntax_error = "-ERR syntax error\r\n";
constexpr const char* not_integer_error = "-ERR value is not an integer or out of range\r\n";
constexpr const char* not_float_error = "-ERR value is not a valid float\r\n";
constexpr const char* score_range_error = "-ERR min or max is not a float\r\n";
constexpr const char* lex_range_error = "-ERR min or max not valid string range item\r\n";

std::string lower(std::string text) {
    for(char& ch : text) if(ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

std::string upper(std::string text) {
    for(char& ch : text) if(ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return text;
}

std::string wrong_arguments(const std::string& name) {
    return "-ERR wrong number of arguments for '" + lower(name) + "' command\r\n";
}

// Redis's integer rules (string2ll): optional '-', no '+', no leading zeros.
bool parse_integer(const std::string& text, long long& value) {
    if(text.empty() || text.size() > 20) return false;
    if(text == "0"){
        value = 0;
        return true;
    }
    const std::size_t digits = text[0] == '-' ? 1 : 0;
    if(digits >= text.size() || text[digits] < '1' || text[digits] > '9') return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// A score argument, as Redis's string2d reads it: the whole text is a number
// without leading spaces, not NaN, and not out of range. "inf" is allowed.
bool parse_score(const std::string& text, double& value) {
    if(text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    if(end != text.c_str() + text.size() || std::isnan(value)) return false;
    if(errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL || value == 0.0)) return false;
    return true;
}

// One end of a score range, as Redis 8's zslParseRange reads it: "(" marks it
// exclusive, then the number must use the rest of the text up to any NUL byte.
// Redis 8 parses with fast_float, which, unlike strtod, neither skips leading
// spaces nor reads hexadecimal; an empty number is read as 0, as Redis does.
bool parse_score_bound(const std::string& text, double& value, bool& exclusive) {
    exclusive = !text.empty() && text[0] == '(';
    const char* start = text.c_str() + (exclusive ? 1 : 0);
    if(std::isspace(static_cast<unsigned char>(*start))) return false;
    const char* digits = (*start == '-' || *start == '+') ? start + 1 : start;
    if(digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) return false;
    char* end = nullptr;
    value = std::strtod(start, &end);
    return *end == '\0' && !std::isnan(value);
}

std::optional<storage::ScoreRange> parse_score_range(const std::string& min, const std::string& max) {
    storage::ScoreRange range;
    if(!parse_score_bound(min, range.min, range.min_exclusive) ||
       !parse_score_bound(max, range.max, range.max_exclusive)) return std::nullopt;
    return range;
}

// "-" and "+" are the lowest and highest bounds, and Redis marks them
// exclusive; "[x" includes x and "(x" excludes it. Anything else is an error.
bool parse_lex_bound(const std::string& text, storage::LexBound& bound) {
    using Kind = storage::LexBound::Kind;
    if(text.empty()) return false;
    switch(text[0]){
        case '+':
        case '-':
            if(text.size() != 1) return false;
            bound = {text[0] == '-' ? Kind::lowest : Kind::highest, "", true};
            return true;
        case '(':
        case '[':
            bound = {Kind::value, text.substr(1), text[0] == '('};
            return true;
        default:
            return false;
    }
}

std::optional<storage::LexRange> parse_lex_range(const std::string& min, const std::string& max) {
    storage::LexRange range;
    if(!parse_lex_bound(min, range.min) || !parse_lex_bound(max, range.max)) return std::nullopt;
    return range;
}

// The sorted set at key, or nullptr if it is missing. wrong_type is set when
// the key holds another type.
SortedSet* find_zset(storage::Database& database, const std::string& key, bool& wrong_type) {
    wrong_type = false;
    auto found = database.find_live(key);
    if(found == database.values.end()) return nullptr;
    auto* zset = std::get_if<SortedSet>(&found->second.value);
    wrong_type = zset == nullptr;
    return zset;
}

SortedSet& create_zset(storage::Database& database, const std::string& key) {
    auto inserted = database.values.emplace(key, storage::Entry{SortedSet{}, std::nullopt});
    return std::get<SortedSet>(inserted.first->second.value);
}

std::string bulk_score(double score) { return protocol::encode_bulk_string(format_score(score)); }

// A flat array of members, each followed by its score when with_scores is set.
class ReplyList {
    std::vector<std::string> items_;
    bool with_scores_;
public:
    explicit ReplyList(bool with_scores) : with_scores_(with_scores) {}
    void add(const Node* node) {
        items_.push_back(protocol::encode_bulk_string(node->member));
        if(with_scores_) items_.push_back(bulk_score(node->score));
    }
    std::string finish() const { return protocol::encode_array(items_); }
};

enum class RangeBy { rank, score, lex };

// ZRANGE and the older Z[REV]RANGE[BYSCORE|BYLEX] commands, as one function the
// way Redis's zrangeGenericCommand does it. by and reverse are fixed for the
// older commands; for ZRANGE they start unset and may come from the arguments.
std::string range_command(const std::vector<std::string>& arguments, storage::Database& database,
                          std::optional<RangeBy> by, std::optional<bool> reverse) {
    const bool fixed = by.has_value();
    bool with_scores = false;
    long long offset = 0;
    long long limit = -1;// -1 means no limit, as in Redis, even when written as LIMIT 0 -1
    for(std::size_t j = 4; j < arguments.size(); j++){
        const std::string option = upper(arguments[j]);
        const std::size_t left = arguments.size() - j - 1;
        if(option == "WITHSCORES"){
            with_scores = true;
        }
        else if(option == "LIMIT" && left >= 2){
            if(!parse_integer(arguments[j + 1], offset) || !parse_integer(arguments[j + 2], limit)){
                return not_integer_error;
            }
            j += 2;
        }
        else if(!fixed && !reverse && option == "REV"){
            reverse = true;
        }
        else if(!fixed && !by && option == "BYLEX"){
            by = RangeBy::lex;
        }
        else if(!fixed && !by && option == "BYSCORE"){
            by = RangeBy::score;
        }
        else {
            return syntax_error;
        }
    }
    const RangeBy range_by = by.value_or(RangeBy::rank);
    const bool rev = reverse.value_or(false);

    if(limit != -1 && range_by == RangeBy::rank){
        return "-ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX\r\n";
    }
    if(with_scores && range_by == RangeBy::lex){
        return "-ERR syntax error, WITHSCORES not supported in combination with BYLEX\r\n";
    }

    // A reversed score or lex range is written max first.
    const std::string& low = rev && range_by != RangeBy::rank ? arguments[3] : arguments[2];
    const std::string& high = rev && range_by != RangeBy::rank ? arguments[2] : arguments[3];

    long long start = 0;
    long long end = 0;
    std::optional<storage::ScoreRange> scores;
    std::optional<storage::LexRange> lex;
    if(range_by == RangeBy::rank){
        if(!parse_integer(low, start) || !parse_integer(high, end)) return not_integer_error;
    }
    else if(range_by == RangeBy::score){
        scores = parse_score_range(low, high);
        if(!scores) return score_range_error;
    }
    else {
        lex = parse_lex_range(low, high);
        if(!lex) return lex_range_error;
    }

    bool wrong_type = false;
    const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
    if(wrong_type) return wrong_type_error;
    ReplyList reply(with_scores);
    if(!zset) return reply.finish();
    const SkipList& list = zset->list();
    const auto length = static_cast<long long>(list.size());

    if(range_by == RangeBy::rank){
        if(start < 0) start += length;
        if(end < 0) end += length;
        if(start < 0) start = 0;
        if(start > end || start >= length) return reply.finish();
        if(end >= length) end = length - 1;
        const Node* node = list.by_rank(static_cast<std::size_t>(rev ? length - start : start + 1));
        for(long long i = start; i <= end && node; i++){
            reply.add(node);
            node = rev ? node->backward : node->level[0].forward;
        }
        return reply.finish();
    }

    // Score and lex ranges: find the first node, skip offset nodes, then emit
    // up to limit nodes while they stay inside the range. A negative offset
    // returns nothing, as in Redis.
    if(offset < 0 || (offset > 0 && offset >= length)) return reply.finish();
    auto inside = [&](const Node* node){
        if(scores) return SkipList::above_min(node->score, *scores) && SkipList::below_max(node->score, *scores);
        return SkipList::above_min(node->member, *lex) && SkipList::below_max(node->member, *lex);
    };
    const Node* node = scores ? (rev ? list.last_in(*scores) : list.first_in(*scores))
                              : (rev ? list.last_in(*lex) : list.first_in(*lex));
    auto step = [&](const Node* current){ return rev ? current->backward : current->level[0].forward; };
    for(; node && offset > 0; offset--) node = step(node);
    for(long long emitted = 0; node && inside(node) && (limit < 0 || emitted < limit); emitted++){
        reply.add(node);
        node = step(node);
    }
    return reply.finish();
}

// ZCOUNT and ZLEXCOUNT: count by rank difference, in O(log N).
std::string count_command(const std::vector<std::string>& arguments, storage::Database& database,
                          bool lex) {
    std::optional<storage::ScoreRange> scores;
    std::optional<storage::LexRange> lex_range;
    if(lex){
        lex_range = parse_lex_range(arguments[2], arguments[3]);
        if(!lex_range) return lex_range_error;
    }
    else {
        scores = parse_score_range(arguments[2], arguments[3]);
        if(!scores) return score_range_error;
    }
    bool wrong_type = false;
    const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
    if(wrong_type) return wrong_type_error;
    if(!zset) return ":0\r\n";
    const SkipList& list = zset->list();
    const Node* first = lex ? list.first_in(*lex_range) : list.first_in(*scores);
    if(!first) return ":0\r\n";
    const Node* last = lex ? list.last_in(*lex_range) : list.last_in(*scores);
    const std::size_t count = list.rank(last->score, last->member) -
                              list.rank(first->score, first->member) + 1;
    return protocol::encode_integer(static_cast<std::int64_t>(count));
}

// ZREMRANGEBYRANK, ZREMRANGEBYSCORE and ZREMRANGEBYLEX.
std::string remove_range_command(const std::string& name, const std::vector<std::string>& arguments,
                                 storage::Database& database) {
    long long start = 0;
    long long end = 0;
    std::optional<storage::ScoreRange> scores;
    std::optional<storage::LexRange> lex;
    if(name == "ZREMRANGEBYRANK"){
        if(!parse_integer(arguments[2], start) || !parse_integer(arguments[3], end)) return not_integer_error;
    }
    else if(name == "ZREMRANGEBYSCORE"){
        scores = parse_score_range(arguments[2], arguments[3]);
        if(!scores) return score_range_error;
    }
    else {
        lex = parse_lex_range(arguments[2], arguments[3]);
        if(!lex) return lex_range_error;
    }

    bool wrong_type = false;
    SortedSet* zset = find_zset(database, arguments[1], wrong_type);
    if(wrong_type) return wrong_type_error;
    if(!zset) return ":0\r\n";
    const SkipList& list = zset->list();

    // Collect the members first: removing while walking would free the node
    // the walk stands on.
    std::vector<std::string> doomed;
    if(scores || lex){
        const Node* node = scores ? list.first_in(*scores) : list.first_in(*lex);
        while(node && (scores ? SkipList::below_max(node->score, *scores)
                              : SkipList::below_max(node->member, *lex))){
            doomed.push_back(node->member);
            node = node->level[0].forward;
        }
    }
    else {
        const auto length = static_cast<long long>(list.size());
        if(start < 0) start += length;
        if(end < 0) end += length;
        if(start < 0) start = 0;
        if(start <= end && start < length){
            if(end >= length) end = length - 1;
            const Node* node = list.by_rank(static_cast<std::size_t>(start + 1));
            for(long long i = start; i <= end && node; i++, node = node->level[0].forward){
                doomed.push_back(node->member);
            }
        }
    }
    for(const auto& member : doomed) zset->erase(member);
    if(zset->empty()) database.erase_live(arguments[1]);
    return protocol::encode_integer(static_cast<std::int64_t>(doomed.size()));
}

// ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
std::string zadd_command(const std::vector<std::string>& arguments, storage::Database& database) {
    bool nx = false, xx = false, gt = false, lt = false, ch = false, incr = false;
    std::size_t first_score = 2;
    for(; first_score < arguments.size(); first_score++){
        const std::string flag = upper(arguments[first_score]);
        if(flag == "NX") nx = true;
        else if(flag == "XX") xx = true;
        else if(flag == "GT") gt = true;
        else if(flag == "LT") lt = true;
        else if(flag == "CH") ch = true;
        else if(flag == "INCR") incr = true;
        else break;
    }

    // The same checks, in the same order, as Redis's zaddGenericCommand.
    const std::size_t left = arguments.size() - first_score;
    if(left == 0 || left % 2 != 0) return syntax_error;
    if(nx && xx) return "-ERR XX and NX options at the same time are not compatible\r\n";
    if((gt && nx) || (lt && nx) || (gt && lt)){
        return "-ERR GT, LT, and/or NX options at the same time are not compatible\r\n";
    }
    if(incr && left > 2) return "-ERR INCR option supports a single increment-element pair\r\n";

    // Every score is checked before anything changes: ZADD applies all pairs or none.
    std::vector<double> scores;
    for(std::size_t i = first_score; i < arguments.size(); i += 2){
        double score = 0;
        if(!parse_score(arguments[i], score)) return not_float_error;
        scores.push_back(score);
    }

    bool wrong_type = false;
    SortedSet* zset = find_zset(database, arguments[1], wrong_type);
    if(wrong_type) return wrong_type_error;
    if(!zset){
        if(xx) return incr ? "$-1\r\n" : ":0\r\n";// nothing can be updated
        zset = &create_zset(database, arguments[1]);
    }

    std::int64_t added = 0;
    std::int64_t updated = 0;
    std::optional<double> incr_result;
    for(std::size_t i = 0; i < scores.size(); i++){
        const std::string& member = arguments[first_score + 2 * i + 1];
        double score = scores[i];
        const double* current = zset->score(member);
        if(current){
            if(nx) continue;
            if(incr){
                score += *current;
                if(std::isnan(score)){
                    if(zset->empty()) database.erase_live(arguments[1]);
                    return "-ERR resulting score is not a number (NaN)\r\n";
                }
            }
            // GT and LT only stop updates of existing members, never additions.
            if((lt && score >= *current) || (gt && score <= *current)) continue;
            if(score != *current){
                zset->set(member, score);
                updated++;
            }
            if(incr) incr_result = score;
        }
        else {
            if(xx) continue;
            zset->set(member, score);
            added++;
            if(incr) incr_result = score;
        }
    }
    if(zset->empty()) database.erase_live(arguments[1]);
    if(incr) return incr_result ? bulk_score(*incr_result) : "$-1\r\n";
    return protocol::encode_integer(ch ? added + updated : added);
}

// ZPOPMIN and ZPOPMAX key [count]
std::string pop_command(const std::string& name, const std::vector<std::string>& arguments,
                        storage::Database& database) {
    if(arguments.size() > 3) return syntax_error;
    long long count = 1;
    if(arguments.size() == 3){
        if(!parse_integer(arguments[2], count) || count < 0){
            return "-ERR value is out of range, must be positive\r\n";
        }
    }
    bool wrong_type = false;
    SortedSet* zset = find_zset(database, arguments[1], wrong_type);
    if(wrong_type) return wrong_type_error;
    ReplyList reply(true);
    if(!zset) return reply.finish();

    const bool max = name == "ZPOPMAX";
    std::vector<std::string> popped;
    for(long long i = 0; i < count && !zset->empty(); i++){
        const Node* node = max ? zset->list().last() : zset->list().first();
        reply.add(node);
        popped.push_back(node->member);
        zset->erase(popped.back());// node is freed here; its reply is already copied
    }
    if(zset->empty()) database.erase_live(arguments[1]);
    return reply.finish();
}

}  // namespace

std::string format_score(double score) {
    // Redis's d2string, in its order: NaN, infinities, then whole numbers
    // within +-LLONG_MAX/2 as integers (double2ll), which also turns -0 into "0".
    if(std::isnan(score)) return "nan";
    if(std::isinf(score)) return score > 0 ? "inf" : "-inf";
    if(score >= static_cast<double>(-(LLONG_MAX / 2)) && score <= static_cast<double>(LLONG_MAX / 2)){
        const auto whole = static_cast<long long>(score);
        if(static_cast<double>(whole) == score) return std::to_string(whole);
    }

    // Otherwise Redis's fpconv_dtoa: the shortest digits that read back as the
    // same double (std::to_chars finds the same ones), laid out by its
    // emit_digits rules. With value = digits * 10^K and E = |K + n - 1| the
    // decimal exponent of the first digit:
    //   K >= 0 and E < n + 7          whole number, digits then K zeros
    //   K < 0 and (K > -7 or E < 4)   plain decimal
    //   otherwise                     d.ddde+X, the exponent without padding
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), score, std::chars_format::scientific);
    std::string text(buffer, result.ptr);// like "-1.2345e+05"
    const bool negative = text[0] == '-';
    if(negative) text.erase(0, 1);
    const std::size_t e = text.find('e');
    const int first_exponent = std::stoi(text.substr(e + 1));
    std::string digits = text.substr(0, e);
    if(digits.size() > 1) digits.erase(1, 1);// drop the '.'
    const int n = static_cast<int>(digits.size());
    const int k = first_exponent - (n - 1);
    const int magnitude = first_exponent < 0 ? -first_exponent : first_exponent;

    std::string out;
    if(k >= 0 && magnitude < n + 7){
        out = digits + std::string(static_cast<std::size_t>(k), '0');
    } 
    else if(k < 0 && (k > -7 || magnitude < 4)){
        const int offset = n + k;// digits before the decimal point
        if(offset <= 0) out = "0." + std::string(static_cast<std::size_t>(-offset), '0') + digits;
        else out = digits.substr(0, static_cast<std::size_t>(offset)) + "." +
                   digits.substr(static_cast<std::size_t>(offset));
    } 
    else {
        out = digits.substr(0, 1);
        if(n > 1) out += "." + digits.substr(1);
        out += first_exponent < 0 ? "e-" : "e+";
        out += std::to_string(magnitude);
    }
    return negative ? "-" + out : out;
}

std::string execute_zset_command(const std::string& name,
                                 const std::vector<std::string>& arguments,
                                 storage::Database& database) {
    const std::size_t count = arguments.size();
    bool wrong_type = false;

    if(name == "ZADD"){
        if(count < 4) return wrong_arguments(name);
        return zadd_command(arguments, database);
    }

    if(name == "ZINCRBY"){
        if(count != 4) return wrong_arguments(name);
        // Exactly ZADD key INCR increment member.
        return zadd_command({arguments[0], arguments[1], "INCR", arguments[2], arguments[3]}, database);
    }

    if(name == "ZSCORE"){
        if(count != 3) return wrong_arguments(name);
        const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
        if(wrong_type) return wrong_type_error;
        const double* score = zset ? zset->score(arguments[2]) : nullptr;
        return score ? bulk_score(*score) : "$-1\r\n";
    }

    if(name == "ZMSCORE"){
        if(count < 3) return wrong_arguments(name);
        const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
        if(wrong_type) return wrong_type_error;
        std::vector<std::string> items;
        for(std::size_t i = 2; i < count; i++){
            const double* score = zset ? zset->score(arguments[i]) : nullptr;
            items.push_back(score ? bulk_score(*score) : "$-1\r\n");
        }
        return protocol::encode_array(items);
    }

    if(name == "ZCARD"){
        if(count != 2) return wrong_arguments(name);
        const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
        if(wrong_type) return wrong_type_error;
        return protocol::encode_integer(zset ? static_cast<std::int64_t>(zset->size()) : 0);
    }

    if(name == "ZREM"){
        if(count < 3) return wrong_arguments(name);
        SortedSet* zset = find_zset(database, arguments[1], wrong_type);
        if(wrong_type) return wrong_type_error;
        if(!zset) return ":0\r\n";
        std::int64_t removed = 0;
        for(std::size_t i = 2; i < count; i++) removed += zset->erase(arguments[i]) ? 1 : 0;
        if(zset->empty()) database.erase_live(arguments[1]);
        return protocol::encode_integer(removed);
    }

    if(name == "ZRANK" || name == "ZREVRANK"){
        if(count < 3 || count > 4) return wrong_arguments(name);
        const bool with_score = count == 4;
        if(with_score && upper(arguments[3]) != "WITHSCORE") return syntax_error;
        const SortedSet* zset = find_zset(database, arguments[1], wrong_type);
        if(wrong_type) return wrong_type_error;
        const long long rank = zset ? zset->rank(arguments[2]) : -1;
        if(rank < 0) return with_score ? "*-1\r\n" : "$-1\r\n";
        const long long shown = name == "ZRANK" ? rank : static_cast<long long>(zset->size()) - 1 - rank;
        if(!with_score) return protocol::encode_integer(shown);
        return protocol::encode_array({protocol::encode_integer(shown),
                                       bulk_score(*zset->score(arguments[2]))});
    }

    if(name == "ZRANGE"){
        if(count < 4) return wrong_arguments(name);
        return range_command(arguments, database, std::nullopt, std::nullopt);
    }
    if(name == "ZREVRANGE" || name == "ZRANGEBYSCORE" || name == "ZREVRANGEBYSCORE" ||
       name == "ZRANGEBYLEX" || name == "ZREVRANGEBYLEX"){
        if(count < 4) return wrong_arguments(name);
        const RangeBy by = name == "ZREVRANGE" ? RangeBy::rank
                         : name.find("BYSCORE") != std::string::npos ? RangeBy::score
                                                                     : RangeBy::lex;
        return range_command(arguments, database, by, name.rfind("ZREV", 0) == 0);
    }

    if(name == "ZCOUNT" || name == "ZLEXCOUNT"){
        if(count != 4) return wrong_arguments(name);
        return count_command(arguments, database, name == "ZLEXCOUNT");
    }

    if(name == "ZPOPMIN" || name == "ZPOPMAX"){
        if(count < 2) return wrong_arguments(name);
        return pop_command(name, arguments, database);
    }

    if(name == "ZREMRANGEBYRANK" || name == "ZREMRANGEBYSCORE" || name == "ZREMRANGEBYLEX"){
        if(count != 4) return wrong_arguments(name);
        return remove_range_command(name, arguments, database);
    }

    return {};
}

}  // namespace commands
