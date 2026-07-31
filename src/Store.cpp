#include "Store.h"

Store::Store() : running_(true) {
    sweeper_ = std::thread(&Store::activeExpireLoop, this);
}

Store::~Store() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        running_ = false;
    }
    cv_.notify_all();
    if (sweeper_.joinable()) sweeper_.join();
}

bool Store::isExpiredLocked(const std::string& key, Clock::time_point now) const {
    auto it = expireAt_.find(key);
    if (it == expireAt_.end()) return false;
    return it->second <= now;
}

void Store::eraseKeyLocked(const std::string& key) {
    data_.erase(key);
    expireAt_.erase(key);
    version_.erase(key);
}

void Store::set(const std::string& key, const std::string& value, std::optional<long> ttlSeconds) {
    std::lock_guard<std::mutex> lock(mtx_);
    data_[key] = value;
    uint64_t v = ++version_[key]; // bump version regardless, invalidating any older heap entries

    if (ttlSeconds.has_value()) {
        auto when = Clock::now() + std::chrono::seconds(*ttlSeconds);
        expireAt_[key] = when;
        heap_.push(HeapEntry{when, key, v});
    } else {
        expireAt_.erase(key); // SET without EX clears any prior TTL, same as real Redis
    }
    cv_.notify_all();
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    if (isExpiredLocked(key, now)) {
        eraseKeyLocked(key);
        return std::nullopt;
    }
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

bool Store::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    bool existed = data_.count(key) > 0;
    eraseKeyLocked(key);
    return existed;
}

bool Store::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    if (isExpiredLocked(key, now)) {
        eraseKeyLocked(key);
        return false;
    }
    return data_.count(key) > 0;
}

bool Store::expire(const std::string& key, long seconds) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    if (isExpiredLocked(key, now)) {
        eraseKeyLocked(key);
        return false;
    }
    if (!data_.count(key)) return false;

    auto when = now + std::chrono::seconds(seconds);
    expireAt_[key] = when;
    uint64_t v = ++version_[key];
    heap_.push(HeapEntry{when, key, v});
    cv_.notify_all();
    return true;
}

long Store::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    if (isExpiredLocked(key, now)) {
        eraseKeyLocked(key);
        return -2;
    }
    if (!data_.count(key)) return -2;
    auto it = expireAt_.find(key);
    if (it == expireAt_.end()) return -1;
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(it->second - now).count();
    return remaining < 0 ? 0 : remaining;
}

std::vector<std::string> Store::keys() {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    std::vector<std::string> result;
    result.reserve(data_.size());
    for (auto& [k, v] : data_) {
        if (!isExpiredLocked(k, now)) result.push_back(k);
    }
    return result;
}

size_t Store::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_.size();
}

// Background thread: wakes periodically, pops the heap while the earliest
// entry is due, and reclaims memory for anything genuinely expired. Entries
// whose version no longer matches the key's live version are just discarded
// (they're leftovers from an older TTL that was replaced/cleared).
void Store::activeExpireLoop() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (running_) {
        auto now = Clock::now();
        while (!heap_.empty() && heap_.top().when <= now) {
            HeapEntry top = heap_.top();
            heap_.pop();
            auto vit = version_.find(top.key);
            if (vit != version_.end() && vit->second == top.version) {
                eraseKeyLocked(top.key);
            }
            // else: stale heap entry from a superseded TTL, ignore it.
        }

        if (heap_.empty()) {
            cv_.wait_for(lock, std::chrono::milliseconds(200));
        } else {
            cv_.wait_until(lock, heap_.top().when);
        }
    }
}
