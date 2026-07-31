#pragma once
// ZSetStore: maps a zset name -> its SkipList. This is the layer that lets
// you have many independent sorted sets (e.g. "leaderboard", "recent_scores")
// the way Redis does with ZADD key score member.

#include "SkipList.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <optional>

class ZSetStore {
public:
    bool zadd(const std::string& key, double score, const std::string& member);
    bool zrem(const std::string& key, const std::string& member);
    std::optional<double> zscore(const std::string& key, const std::string& member);
    std::vector<std::pair<double, std::string>> zrange(const std::string& key, long start, long stop);
    std::vector<std::pair<double, std::string>> zrangebyscore(const std::string& key, double min, double max);
    long zrank(const std::string& key, const std::string& member);
    long zcard(const std::string& key);
    bool exists(const std::string& key);

private:
    std::unordered_map<std::string, std::unique_ptr<SkipList>> sets_;
    std::mutex mtx_;
};
