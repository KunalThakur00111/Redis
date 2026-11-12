#pragma once

// A Redis sorted set: members ordered by (score, member), with O(1) score
// lookup by member and O(log N) rank and range queries.
//
// Like Redis, it pairs two structures:
//   scores   unordered_map member -> score, for ZSCORE and membership
//   list     a skip list ordered by (score, member bytes), for ranks and ranges
//
// The skip list follows Redis's t_zset.c (zskiplist). Each node has a random
// number of levels; level i links to the next node that has at least i+1
// levels. Each link also stores its span: how many level-0 steps it skips.
// Summing spans while walking down gives a node's rank in O(log N), which a
// plain std::set cannot do.

#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace storage {

// A score interval such as "(1 5" (1 < score <= 5).
struct ScoreRange {
    double min = 0;
    double max = 0;
    bool min_exclusive = false;
    bool max_exclusive = false;
};

// One end of a lexicographic interval: "-" (below everything), "+" (above
// everything), "[value" (inclusive) or "(value" (exclusive).
struct LexBound {
    enum class Kind { lowest, highest, value };
    Kind kind = Kind::value;
    std::string value;
    bool exclusive = false;
};

struct LexRange {
    LexBound min;
    LexBound max;
};

class SkipList {
public:
    struct Node {
        struct Level {
            Node* forward = nullptr;
            std::size_t span = 0;
        };
        std::string member;
        double score = 0;
        Node* backward = nullptr;
        std::vector<Level> level;
    };

    SkipList() : header_(new Node) { header_->level.resize(max_level); }
    ~SkipList() { clear(); delete header_; }

    SkipList(const SkipList& other) : SkipList() {
        for(const Node* node = other.first(); node; node = node->level[0].forward){
            insert(node->score, node->member);
        }
    }
    SkipList& operator=(const SkipList& other) {
        if(this != &other){
            SkipList copy(other);
            swap(copy);
        }
        return *this;
    }
    SkipList(SkipList&& other) noexcept : SkipList() { swap(other); }
    SkipList& operator=(SkipList&& other) noexcept {
        swap(other);
        return *this;
    }

    void swap(SkipList& other) noexcept {
        std::swap(header_, other.header_);
        std::swap(tail_, other.tail_);
        std::swap(length_, other.length_);
        std::swap(level_, other.level_);
    }

    std::size_t size() const { return length_; }
    const Node* first() const { return header_->level[0].forward; }
    const Node* last() const { return tail_; }

    // Insert a member the caller knows is absent.
    void insert(double score, const std::string& member) {
        Node* update[max_level];
        std::size_t rank[max_level];
        Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            // rank[i] is the rank of update[i]; it starts where the level above stopped.
            rank[i] = i == level_ - 1 ? 0 : rank[i + 1];
            while(x->level[i].forward && before(x->level[i].forward, score, member)){
                rank[i] += x->level[i].span;
                x = x->level[i].forward;
            }
            update[i] = x;
        }

        const int levels = random_level();
        if(levels > level_){
            for(int i = level_; i < levels; i++){
                rank[i] = 0;
                update[i] = header_;
                update[i]->level[i].span = length_;
            }
            level_ = levels;
        }

        Node* node = new Node;
        node->member = member;
        node->score = score;
        node->level.resize(static_cast<std::size_t>(levels));
        for(int i = 0; i < levels; i++){
            node->level[i].forward = update[i]->level[i].forward;
            update[i]->level[i].forward = node;
            // update[i] is rank[i]; the new node is rank[0] + 1.
            node->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
            update[i]->level[i].span = (rank[0] - rank[i]) + 1;
        }
        // Links above the new node's height now skip one more node.
        for(int i = levels; i < level_; i++) update[i]->level[i].span++;

        node->backward = update[0] == header_ ? nullptr : update[0];
        if(node->level[0].forward) node->level[0].forward->backward = node;
        else tail_ = node;
        length_++;
    }

    // Remove (score, member) if present.
    bool erase(double score, const std::string& member) {
        Node* update[max_level];
        Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && before(x->level[i].forward, score, member)){
                x = x->level[i].forward;
            }
            update[i] = x;
        }
        x = x->level[0].forward;
        if(!x || x->score != score || x->member != member) return false;
        unlink(x, update);
        delete x;
        return true;
    }

    // 1-based rank of (score, member), or 0 when it is absent.
    std::size_t rank(double score, const std::string& member) const {
        std::size_t rank = 0;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward &&
                  (before(x->level[i].forward, score, member) ||
                   (x->level[i].forward->score == score && x->level[i].forward->member == member))){
                rank += x->level[i].span;
                x = x->level[i].forward;
            }
            if(x != header_ && x->score == score && x->member == member) return rank;
        }
        return 0;
    }

    // The node at a 1-based rank, or nullptr when out of range.
    const Node* by_rank(std::size_t rank) const {
        if(rank == 0 || rank > length_) return nullptr;
        std::size_t traversed = 0;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && traversed + x->level[i].span <= rank){
                traversed += x->level[i].span;
                x = x->level[i].forward;
            }
            if(traversed == rank) return x;
        }
        return nullptr;
    }

    static bool above_min(double score, const ScoreRange& range) {
        return range.min_exclusive ? score > range.min : score >= range.min;
    }
    static bool below_max(double score, const ScoreRange& range) {
        return range.max_exclusive ? score < range.max : score <= range.max;
    }

    // First node whose score is inside the range, or nullptr.
    const Node* first_in(const ScoreRange& range) const {
        if(!may_overlap(range)) return nullptr;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && !above_min(x->level[i].forward->score, range)){
                x = x->level[i].forward;
            }
        }
        x = x->level[0].forward;
        return x && below_max(x->score, range) ? x : nullptr;
    }

    // Last node whose score is inside the range, or nullptr.
    const Node* last_in(const ScoreRange& range) const {
        if(!may_overlap(range)) return nullptr;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && below_max(x->level[i].forward->score, range)){
                x = x->level[i].forward;
            }
        }
        return x != header_ && above_min(x->score, range) ? x : nullptr;
    }

    // Compare a member with a bound, treating "-" as below and "+" as above all.
    static int compare(const std::string& member, const LexBound& bound) {
        if(bound.kind == LexBound::Kind::lowest) return 1;
        if(bound.kind == LexBound::Kind::highest) return -1;
        return member.compare(bound.value) < 0 ? -1 : member == bound.value ? 0 : 1;
    }
    static bool above_min(const std::string& member, const LexRange& range) {
        const int c = compare(member, range.min);
        return range.min.exclusive ? c > 0 : c >= 0;
    }
    static bool below_max(const std::string& member, const LexRange& range) {
        const int c = compare(member, range.max);
        return range.max.exclusive ? c < 0 : c <= 0;
    }

    const Node* first_in(const LexRange& range) const {
        if(!may_overlap(range)) return nullptr;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && !above_min(x->level[i].forward->member, range)){
                x = x->level[i].forward;
            }
        }
        x = x->level[0].forward;
        return x && below_max(x->member, range) ? x : nullptr;
    }

    const Node* last_in(const LexRange& range) const {
        if(!may_overlap(range)) return nullptr;
        const Node* x = header_;
        for(int i = level_ - 1; i >= 0; i--){
            while(x->level[i].forward && below_max(x->level[i].forward->member, range)){
                x = x->level[i].forward;
            }
        }
        return x != header_ && above_min(x->member, range) ? x : nullptr;
    }

private:
    static constexpr int max_level = 32;// enough for 2^64 elements with p = 1/4

    Node* header_;// a sentinel with max_level links and no member
    Node* tail_ = nullptr;
    std::size_t length_ = 0;
    int level_ = 1;// the tallest node's level count

    // Does node come before (score, member)? Members compare as bytes, as in Redis.
    static bool before(const Node* node, double score, const std::string& member) {
        return node->score < score || (node->score == score && node->member.compare(member) < 0);
    }

    // Each extra level has probability 1/4, as in Redis.
    static int random_level() {
        thread_local std::mt19937 engine{std::random_device{}()};
        int levels = 1;
        while(levels < max_level && (engine() & 0xFFFF) < (0xFFFF / 4)) levels++;
        return levels;
    }

    void unlink(Node* x, Node** update) {
        for(int i = 0; i < level_; i++){
            if(update[i]->level[i].forward == x){
                update[i]->level[i].span += x->level[i].span - 1;
                update[i]->level[i].forward = x->level[i].forward;
            }
            else {
                update[i]->level[i].span -= 1;
            }
        }
        if(x->level[0].forward) x->level[0].forward->backward = x->backward;
        else tail_ = x->backward;
        while(level_ > 1 && header_->level[level_ - 1].forward == nullptr) level_--;
        length_--;
    }

    bool may_overlap(const ScoreRange& range) const {
        if(range.min > range.max ||
           (range.min == range.max && (range.min_exclusive || range.max_exclusive))) return false;
        if(!tail_ || !above_min(tail_->score, range)) return false;
        const Node* head = header_->level[0].forward;
        return head && below_max(head->score, range);
    }

    bool may_overlap(const LexRange& range) const {
        // Compare the two bounds the way Redis's sdscmplex does.
        auto order = [](const LexBound& a, const LexBound& b){
            if(a.kind == b.kind && a.kind != LexBound::Kind::value) return 0;
            if(a.kind == LexBound::Kind::lowest || b.kind == LexBound::Kind::highest) return -1;
            if(a.kind == LexBound::Kind::highest || b.kind == LexBound::Kind::lowest) return 1;
            const int c = a.value.compare(b.value);
            return c < 0 ? -1 : c > 0 ? 1 : 0;
        };
        const int c = order(range.min, range.max);
        if(c > 0 || (c == 0 && (range.min.exclusive || range.max.exclusive))) return false;
        if(!tail_ || !above_min(tail_->member, range)) return false;
        const Node* head = header_->level[0].forward;
        return head && below_max(head->member, range);
    }

    void clear() {
        Node* x = header_->level[0].forward;
        while(x){
            Node* next = x->level[0].forward;
            delete x;
            x = next;
        }
        for(auto& level : header_->level) level = Node::Level{};
        tail_ = nullptr;
        length_ = 0;
        level_ = 1;
    }
};

class SortedSet {
public:
    std::size_t size() const { return scores_.size(); }
    bool empty() const { return scores_.empty(); }
    const SkipList& list() const { return list_; }
    const std::unordered_map<std::string, double>& scores() const { return scores_; }

    const double* score(const std::string& member) const {
        auto found = scores_.find(member);
        return found == scores_.end() ? nullptr : &found->second;
    }

    // Add a new member or move an existing one to a new score.
    // Returns true when the member is new.
    bool set(const std::string& member, double score) {
        auto found = scores_.find(member);
        if(found == scores_.end()){
            scores_.emplace(member, score);
            list_.insert(score, member);
            return true;
        }
        if(found->second != score){
            list_.erase(found->second, member);
            list_.insert(score, member);
            found->second = score;
        }
        return false;
    }

    bool erase(const std::string& member) {
        auto found = scores_.find(member);
        if(found == scores_.end()) return false;
        list_.erase(found->second, member);
        scores_.erase(found);
        return true;
    }

    // 0-based rank, or -1 when absent.
    long long rank(const std::string& member) const {
        auto found = scores_.find(member);
        if(found == scores_.end()) return -1;
        return static_cast<long long>(list_.rank(found->second, member)) - 1;
    }

    bool operator==(const SortedSet& other) const { return scores_ == other.scores_; }

private:
    std::unordered_map<std::string, double> scores_;
    SkipList list_;
};

}  // namespace storage
