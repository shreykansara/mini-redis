#pragma once
// SkipList: backs each ZSET. This is the same core data structure Redis
// itself uses for sorted sets (as opposed to a balanced tree), because it
// gives O(log n) insert/search/range-query with much simpler, lock-friendly
// code than a red-black tree.
//
// Ordering: primarily by score (ascending), tie-broken lexicographically by
// member name, so ZRANGE has a deterministic, Redis-compatible order.

#include <string>
#include <vector>
#include <random>
#include <optional>
#include <unordered_map>
#include <utility>

struct SkipListNode {
    double score;
    std::string member;
    std::vector<SkipListNode*> forward;
    SkipListNode(double s, std::string m, int level)
        : score(s), member(std::move(m)), forward(level, nullptr) {}
};

class SkipList {
public:
    explicit SkipList(int maxLevel = 16, float probability = 0.5f);
    ~SkipList();

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Insert or update (member, score). Returns true if member is new.
    bool insert(double score, const std::string& member);

    // Remove member. Returns true if it existed.
    bool remove(const std::string& member);

    // O(1) score lookup via side index.
    std::optional<double> getScore(const std::string& member) const;

    // 0-indexed inclusive range by rank, Python-slice-style negative indices
    // supported (-1 = last element), matching Redis ZRANGE semantics.
    std::vector<std::pair<double, std::string>> rangeByIndex(long start, long stop) const;

    // All (score, member) pairs with minScore <= score <= maxScore.
    std::vector<std::pair<double, std::string>> rangeByScore(double minScore, double maxScore) const;

    // 0-indexed rank of member in sorted order, -1 if absent.
    long rank(const std::string& member) const;

    size_t size() const { return count_; }

private:
    int maxLevel_;
    float probability_;
    int level_;               // current highest level in use
    size_t count_;
    SkipListNode* head_;       // sentinel
    mutable std::mt19937 rng_;
    std::unordered_map<std::string, double> memberScore_; // member -> score index

    int randomLevel();
};
