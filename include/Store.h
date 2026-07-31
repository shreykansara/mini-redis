#pragma once
// Store: the core string key-value engine.
//
// - O(1) average GET/SET/DEL via unordered_map.
// - TTL expiry is handled the way real Redis does it: a *lazy* check on
//   every read (so a stale key never appears to exist), PLUS an *active*
//   background sweep on a separate thread that pops from a min-heap
//   ordered by expiry time, so idle expired keys still eventually get
//   reclaimed instead of sitting in memory forever.
// - Because a key's TTL can be overwritten (SET/EXPIRE called again before
//   the old timer fires), heap entries can go stale. We tag every heap
//   entry with a version number and only act on it if it still matches the
//   key's current version - classic "lazy deletion from a heap" technique.

#include <string>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <optional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cstdint>

class Store {
public:
    using Clock = std::chrono::steady_clock;

    Store();
    ~Store();

    // ttlSeconds = nullopt means "no expiry".
    void set(const std::string& key, const std::string& value,
             std::optional<long> ttlSeconds = std::nullopt);

    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);

    // Attach/replace a TTL on an existing key. False if key doesn't exist.
    bool expire(const std::string& key, long seconds);

    // -2 = key doesn't exist, -1 = exists but no TTL, else seconds remaining.
    long ttl(const std::string& key);

    std::vector<std::string> keys();
    size_t size();

private:
    struct HeapEntry {
        Clock::time_point when;
        std::string key;
        uint64_t version;
        // priority_queue is a max-heap by default; we want soonest-expiry-first,
        // so invert the comparison to make it a min-heap on `when`.
        bool operator<(const HeapEntry& other) const { return when > other.when; }
    };

    std::unordered_map<std::string, std::string> data_;
    std::unordered_map<std::string, Clock::time_point> expireAt_;
    std::unordered_map<std::string, uint64_t> version_;
    std::priority_queue<HeapEntry> heap_;

    mutable std::mutex mtx_;
    std::thread sweeper_;
    std::atomic<bool> running_;
    std::condition_variable cv_;

    // Must hold mtx_ when calling these.
    bool isExpiredLocked(const std::string& key, Clock::time_point now) const;
    void eraseKeyLocked(const std::string& key);
    void activeExpireLoop();
};
