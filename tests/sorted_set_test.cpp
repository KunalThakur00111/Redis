// Check the sorted set's skip list against a plain sorted vector, over many
// random inserts, score updates and removals.
#include "../src/storage/sorted_set.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression) \
    do { if(!(expression)){ std::cerr << __LINE__ << ": " #expression "\n"; failures++; } } while(0)

using Item = std::pair<double, std::string>;

std::vector<Item> sorted_items(const storage::SortedSet& set) {
    std::vector<Item> items;
    for(const auto& entry : set.scores()) items.emplace_back(entry.second, entry.first);
    std::sort(items.begin(), items.end());// (score, member bytes), the skip list's order
    return items;
}

void check_against_model(const storage::SortedSet& set, std::mt19937& engine) {
    const auto expected = sorted_items(set);
    const auto& list = set.list();
    CHECK(list.size() == expected.size());

    // Forward and backward walks visit the same order.
    std::size_t i = 0;
    for(const auto* node = list.first(); node; node = node->level[0].forward, i++){
        CHECK(i < expected.size() && node->score == expected[i].first &&
              node->member == expected[i].second);
    }
    CHECK(i == expected.size());
    i = expected.size();
    for(const auto* node = list.last(); node; node = node->backward){
        i--;
        CHECK(node->member == expected[i].second);
    }
    CHECK(i == 0);

    // Ranks both ways.
    for(std::size_t r = 0; r < expected.size(); r++){
        CHECK(set.rank(expected[r].second) == static_cast<long long>(r));
        const auto* node = list.by_rank(r + 1);
        CHECK(node && node->member == expected[r].second);
    }
    CHECK(list.by_rank(0) == nullptr);
    CHECK(list.by_rank(expected.size() + 1) == nullptr);
    CHECK(set.rank("absent-member") == -1);

    // Score ranges with every combination of open and closed ends.
    std::uniform_int_distribution<int> score_pick(-2, 22);
    for(int round = 0; round < 20; round++){
        storage::ScoreRange range{static_cast<double>(score_pick(engine)),
                                  static_cast<double>(score_pick(engine)),
                                  (engine() & 1) != 0, (engine() & 1) != 0};
        const Item* first = nullptr;
        const Item* last = nullptr;
        for(const auto& item : expected){
            if(storage::SkipList::above_min(item.first, range) &&
               storage::SkipList::below_max(item.first, range)){
                if(!first) first = &item;
                last = &item;
            }
        }
        const auto* low = list.first_in(range);
        const auto* high = list.last_in(range);
        CHECK((low == nullptr) == (first == nullptr));
        CHECK((high == nullptr) == (last == nullptr));
        if(low && first) CHECK(low->member == first->second);
        if(high && last) CHECK(high->member == last->second);
    }
}

}  // namespace

int main() {
    std::mt19937 engine(12345);
    std::uniform_int_distribution<int> member_pick(0, 299);
    std::uniform_int_distribution<int> score_pick(0, 20);// few scores, so ties are common
    std::uniform_int_distribution<int> action(0, 9);

    storage::SortedSet set;
    for(int step = 0; step < 20000; step++){
        const std::string member = "m" + std::to_string(member_pick(engine));
        if(action(engine) < 7) set.set(member, score_pick(engine));
        else set.erase(member);
        if(step % 500 == 0) check_against_model(set, engine);
    }
    check_against_model(set, engine);

    // Copies are independent deep copies; moves leave a valid empty set.
    storage::SortedSet copy = set;
    CHECK(copy == set);
    copy.set("only-in-copy", 1);
    CHECK(set.score("only-in-copy") == nullptr);
    check_against_model(copy, engine);
    storage::SortedSet moved = std::move(copy);
    check_against_model(moved, engine);

    // Lex ranges over members that share one score.
    storage::SortedSet lex;
    for(const char* member : {"a", "b", "c", "d", "e"}) lex.set(member, 0);
    using Kind = storage::LexBound::Kind;
    auto members = [&](storage::LexRange range){
        std::string out;
        const auto* end = lex.list().last_in(range);
        for(const auto* node = lex.list().first_in(range); node; node = node->level[0].forward){
            out += node->member;
            if(node == end) break;
        }
        return out;
    };
    CHECK(members({{Kind::lowest, "", true}, {Kind::highest, "", true}}) == "abcde");
    CHECK(members({{Kind::value, "b", false}, {Kind::value, "d", false}}) == "bcd");
    CHECK(members({{Kind::value, "b", true}, {Kind::value, "d", true}}) == "c");
    CHECK(members({{Kind::value, "c", true}, {Kind::value, "c", false}}).empty());
    CHECK(members({{Kind::highest, "", true}, {Kind::lowest, "", true}}).empty());
    CHECK(members({{Kind::lowest, "", true}, {Kind::lowest, "", true}}).empty());
    CHECK(members({{Kind::value, "bb", false}, {Kind::highest, "", true}}) == "cde");

    if(failures == 0) std::cout << "sorted set matches the model\n";
    return failures == 0 ? 0 : 1;
}
