#include "SkipList.h"

SkipList::SkipList(int maxLevel, float probability)
    : maxLevel_(maxLevel), probability_(probability), level_(1), count_(0),
      rng_(std::random_device{}()) {
    head_ = new SkipListNode(0.0, "", maxLevel_);
}

SkipList::~SkipList() {
    SkipListNode* node = head_->forward[0];
    while (node) {
        SkipListNode* next = node->forward[0];
        delete node;
        node = next;
    }
    delete head_;
}

int SkipList::randomLevel() {
    int lvl = 1;
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    while (dist(rng_) < probability_ && lvl < maxLevel_) {
        lvl++;
    }
    return lvl;
}

// Compares (score, member) tuples the way Redis orders ZSET entries.
static bool lessThan(double s1, const std::string& m1, double s2, const std::string& m2) {
    if (s1 != s2) return s1 < s2;
    return m1 < m2;
}

bool SkipList::insert(double score, const std::string& member) {
    // ZADD semantics: if member already exists, this is an update (remove + reinsert
    // at the new score), not a duplicate entry.
    auto it = memberScore_.find(member);
    bool isNew = (it == memberScore_.end());
    if (!isNew) {
        if (it->second == score) return false; // no-op, nothing changed
        remove(member);
    }

    std::vector<SkipListNode*> update(maxLevel_, head_);
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] &&
               lessThan(cur->forward[i]->score, cur->forward[i]->member, score, member)) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }

    int newLevel = randomLevel();
    if (newLevel > level_) {
        for (int i = level_; i < newLevel; i++) update[i] = head_;
        level_ = newLevel;
    }

    SkipListNode* node = new SkipListNode(score, member, newLevel);
    for (int i = 0; i < newLevel; i++) {
        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;
    }

    memberScore_[member] = score;
    count_++;
    return isNew;
}

bool SkipList::remove(const std::string& member) {
    auto it = memberScore_.find(member);
    if (it == memberScore_.end()) return false;
    double score = it->second;

    std::vector<SkipListNode*> update(maxLevel_, head_);
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] &&
               lessThan(cur->forward[i]->score, cur->forward[i]->member, score, member)) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }

    SkipListNode* target = cur->forward[0];
    if (!target || target->member != member) return false; // shouldn't happen if index is consistent

    for (int i = 0; i < level_; i++) {
        if (update[i]->forward[i] != target) continue;
        update[i]->forward[i] = target->forward[i];
    }
    delete target;

    while (level_ > 1 && head_->forward[level_ - 1] == nullptr) level_--;

    memberScore_.erase(it);
    count_--;
    return true;
}

std::optional<double> SkipList::getScore(const std::string& member) const {
    auto it = memberScore_.find(member);
    if (it == memberScore_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<double, std::string>> SkipList::rangeByIndex(long start, long stop) const {
    std::vector<std::pair<double, std::string>> result;
    long n = static_cast<long>(count_);
    if (n == 0) return result;

    if (start < 0) start = std::max(0L, n + start);
    if (stop < 0) stop = n + stop;
    if (stop >= n) stop = n - 1;
    if (start > stop || start >= n) return result;

    SkipListNode* cur = head_->forward[0];
    long idx = 0;
    while (cur && idx < start) { cur = cur->forward[0]; idx++; }
    while (cur && idx <= stop) {
        result.emplace_back(cur->score, cur->member);
        cur = cur->forward[0];
        idx++;
    }
    return result;
}

std::vector<std::pair<double, std::string>> SkipList::rangeByScore(double minScore, double maxScore) const {
    std::vector<std::pair<double, std::string>> result;
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] && cur->forward[i]->score < minScore) {
            cur = cur->forward[i];
        }
    }
    cur = cur->forward[0];
    while (cur && cur->score <= maxScore) {
        result.emplace_back(cur->score, cur->member);
        cur = cur->forward[0];
    }
    return result;
}

long SkipList::rank(const std::string& member) const {
    auto it = memberScore_.find(member);
    if (it == memberScore_.end()) return -1;
    double score = it->second;

    SkipListNode* cur = head_;
    long idx = 0;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] &&
               lessThan(cur->forward[i]->score, cur->forward[i]->member, score, member)) {
            cur = cur->forward[i];
            idx++;
        }
    }
    // cur->forward[0] should now be the target node
    if (cur->forward[0] && cur->forward[0]->member == member) return idx;
    return -1;
}
