#include "ZSetStore.h"

bool ZSetStore::zadd(const std::string& key, double score, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& set = sets_[key];
    if (!set) set = std::make_unique<SkipList>();
    return set->insert(score, member);
}

bool ZSetStore::zrem(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return false;
    bool removed = it->second->remove(member);
    if (removed && it->second->size() == 0) sets_.erase(it); // clean up empty zsets
    return removed;
}

std::optional<double> ZSetStore::zscore(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return std::nullopt;
    return it->second->getScore(member);
}

std::vector<std::pair<double, std::string>> ZSetStore::zrange(const std::string& key, long start, long stop) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return {};
    return it->second->rangeByIndex(start, stop);
}

std::vector<std::pair<double, std::string>> ZSetStore::zrangebyscore(const std::string& key, double min, double max) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return {};
    return it->second->rangeByScore(min, max);
}

long ZSetStore::zrank(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return -1;
    return it->second->rank(member);
}

long ZSetStore::zcard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return 0;
    return static_cast<long>(it->second->size());
}

bool ZSetStore::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return sets_.count(key) > 0;
}
