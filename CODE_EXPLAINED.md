# mini-redis — Full Code Walkthrough

This document explains **every file, every class/struct, every function
(with parameters and return types), and every line of logic** in the
mini-redis project: what it does, why it's written that way, and what each
variable/condition/data structure is for.

The project has 6 translation units and 5 headers:

```
include/SkipList.h          src/SkipList.cpp
include/Store.h             src/Store.cpp
include/WAL.h                src/WAL.cpp
include/ZSetStore.h         src/ZSetStore.cpp
include/CommandProcessor.h  src/CommandProcessor.cpp
                             src/main.cpp
Makefile
```

Read them in this order (dependency order, bottom-up): `SkipList` →
`ZSetStore` → `Store` → `WAL` → `CommandProcessor` → `main`.

---

## 1. `include/SkipList.h`

A skip list is a linked, multi-level, sorted data structure that gives
`O(log n)` average insert/search/delete without the rebalancing logic a
red-black or AVL tree needs. Redis itself uses a skip list to back `ZSET`,
which is why this project reimplements one instead of using `std::map`.

```cpp
#pragma once
```
Header guard: tells the compiler to only include this file once per
translation unit, even if it's `#include`d multiple times (directly or
transitively). Equivalent to the classic `#ifndef/#define/#endif` guard.

```cpp
#include <string>
#include <vector>
#include <random>
#include <optional>
#include <unordered_map>
#include <utility>
```
Standard library includes:
- `<string>` — `std::string` for member names.
- `<vector>` — dynamic arrays, used for the node's `forward` pointer array and for returned ranges.
- `<random>` — `std::mt19937` (Mersenne Twister PRNG) and `std::uniform_real_distribution`, used to randomly decide each new node's height.
- `<optional>` — `std::optional<double>` as a "maybe a score, maybe nothing" return type (replaces sentinel values like `-1` or exceptions).
- `<unordered_map>` — the `memberScore_` side index (hash map) for O(1) score lookups.
- `<utility>` — `std::pair`, used as the `(score, member)` tuple type returned by range queries, and `std::move`.

### `struct SkipListNode`

```cpp
struct SkipListNode {
    double score;
    std::string member;
    std::vector<SkipListNode*> forward;
    SkipListNode(double s, std::string m, int level)
        : score(s), member(std::move(m)), forward(level, nullptr) {}
};
```
One node in the skip list.

- **`score`** (`double`) — the sort key. `ZSET` entries are primarily ordered by this.
- **`member`** (`std::string`) — the value stored at this node; also the tie-breaker for equal scores.
- **`forward`** (`std::vector<SkipListNode*>`) — this node's "next node" pointers, one per level it participates in. `forward[0]` is the ordinary singly-linked-list "next" pointer; `forward[i]` for `i > 0` lets a search skip over many level-0 nodes at once. This is what makes the structure logarithmic instead of linear.
- **Constructor** `SkipListNode(double s, std::string m, int level)` — parameters: `s` (score), `m` (member, taken by value and then moved-from to avoid a copy), `level` (how many forward pointers this node needs). Initializer list sets `score`, moves `m` into `member`, and constructs `forward` as a vector of `level` null pointers (`forward(level, nullptr)` is the `vector(count, value)` constructor).

### `class SkipList`

```cpp
class SkipList {
public:
    explicit SkipList(int maxLevel = 16, float probability = 0.5f);
    ~SkipList();
```
- **Constructor** `SkipList(int maxLevel = 16, float probability = 0.5f)` — `explicit` prevents implicit conversion from a bare `int` to a `SkipList`. `maxLevel` caps how tall the list can grow (16 levels comfortably supports millions of elements). `probability` is the per-level "coin flip" chance used by `randomLevel()` (classic skip-list value is `0.5`, meaning on average 1/2 of nodes reach level 2, 1/4 reach level 3, etc. — giving the geometric height distribution that produces `O(log n)` search).
- **Destructor** `~SkipList()` — frees every heap-allocated node (see `.cpp`).

```cpp
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;
```
Copy constructor and copy-assignment are explicitly deleted. Because the class manually manages raw `SkipListNode*` pointers (manual `new`/`delete`), a naive compiler-generated copy would copy the pointers, not the nodes — two `SkipList` objects would then point at the same nodes and double-free them on destruction. Deleting these prevents that bug at compile time; you can only move or pass by reference.

```cpp
    bool insert(double score, const std::string& member);
```
`insert(score, member) -> bool`. Inserts or updates. Returns `true` if `member` is brand new, `false` if it already existed (and was just re-scored).

```cpp
    bool remove(const std::string& member);
```
`remove(member) -> bool`. Deletes `member` if present. Returns `true` if it existed and was removed, `false` if it wasn't there.

```cpp
    std::optional<double> getScore(const std::string& member) const;
```
`getScore(member) -> optional<double>`. `O(1)` lookup via the side hash index (not a skip-list traversal). Returns `nullopt` if the member isn't present. `const` — doesn't mutate the list.

```cpp
    std::vector<std::pair<double, std::string>> rangeByIndex(long start, long stop) const;
```
`rangeByIndex(start, stop) -> vector<pair<double,string>>`. 0-indexed, **inclusive** range by rank/position in sorted order. Supports negative indices Python-slice style (`-1` = last element), matching Redis's `ZRANGE key start stop` semantics.

```cpp
    std::vector<std::pair<double, std::string>> rangeByScore(double minScore, double maxScore) const;
```
`rangeByScore(minScore, maxScore) -> vector<pair<double,string>>`. All `(score, member)` pairs with `minScore <= score <= maxScore`, in ascending order. Backs `ZRANGEBYSCORE`.

```cpp
    long rank(const std::string& member) const;
```
`rank(member) -> long`. 0-indexed position of `member` in the sorted order, or `-1` if it doesn't exist. Backs `ZRANK`.

```cpp
    size_t size() const { return count_; }
```
Trivial inline accessor — number of elements currently stored. Backs `ZCARD`.

```cpp
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
```
Private state:
- **`maxLevel_`** — the cap passed to the constructor; bounds how many forward pointers any node (and the head sentinel) can have.
- **`probability_`** — the per-level growth probability used by `randomLevel()`.
- **`level_`** — the *current* highest level actually in use (starts at 1, can grow up to `maxLevel_` as taller nodes get inserted, and shrinks back down when the tallest nodes are removed). Keeping this separate from `maxLevel_` means searches don't waste time descending through unused high levels.
- **`count_`** — element count, backs `size()`.
- **`head_`** — a sentinel node that is *not* a real element (score `0.0`, empty member, but with `maxLevel_` forward pointers). All searches start here; it lets every node — including the very first real one — be treated uniformly as "some node's `forward[i]`", so there's no special-casing of "is this the first node?" in the algorithms.
- **`rng_`** — a per-instance Mersenne Twister random engine, marked `mutable` so that `const` methods (there are none that call it here, but `randomLevel()` itself isn't `const`) aren't blocked from advancing its internal state; it's mutable defensively/idiomatically since RNG state mutates on every draw.
- **`memberScore_`** — the "side index": a hash map from member name to its current score. This is what makes `getScore()` `O(1)` instead of requiring a list walk, and it's also what `insert`/`remove`/`rank` use to quickly check "does this member exist, and at what score" before doing the positional skip-list walk.
- **`randomLevel()`** — private helper, described below.

---

## 2. `src/SkipList.cpp`

```cpp
#include "SkipList.h"

SkipList::SkipList(int maxLevel, float probability)
    : maxLevel_(maxLevel), probability_(probability), level_(1), count_(0),
      rng_(std::random_device{}()) {
    head_ = new SkipListNode(0.0, "", maxLevel_);
}
```
Constructor body. Member-initializer list sets the simple fields, and seeds `rng_` with a value drawn from `std::random_device` (a non-deterministic, OS-provided entropy source) — `std::random_device{}()` constructs a temporary `random_device` and immediately invokes it to get one seed value. `level_` starts at `1` (an empty list has one active level). The body then heap-allocates the sentinel `head_` node: score `0.0` (unused, since it's a sentinel), empty member string, with `maxLevel_` forward slots (all `nullptr`, from the `SkipListNode` constructor).

```cpp
SkipList::~SkipList() {
    SkipListNode* node = head_->forward[0];
    while (node) {
        SkipListNode* next = node->forward[0];
        delete node;
        node = next;
    }
    delete head_;
}
```
Destructor. Walks the level-0 chain (which by construction touches *every* node, since every node has at least one forward pointer at level 0) starting from `head_->forward[0]` (the first real node). For each node: save its `next` pointer *before* deleting it (`next` must be captured first — reading `node->forward[0]` after `delete node` would be a use-after-free), delete the current node, advance. Finally deletes the sentinel itself. This avoids leaking memory, since nodes were `new`-allocated.

```cpp
int SkipList::randomLevel() {
    int lvl = 1;
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    while (dist(rng_) < probability_ && lvl < maxLevel_) {
        lvl++;
    }
    return lvl;
}
```
`randomLevel() -> int`. Decides how tall a newly-inserted node should be. Starts at level 1 (every node is at least a level-0/1 node). `dist` draws a uniform float in `[0, 1)`. Each iteration is a "coin flip": while the draw is `< probability_` (default `0.5`, i.e. "heads") *and* we haven't hit the ceiling `maxLevel_`, climb one more level. This produces a geometric distribution: `P(level >= k) = probability_^(k-1)`, which is exactly what makes skip-list search `O(log n)` on average — half the nodes are level-1-only, a quarter reach level 2, an eighth reach level 3, and so on, so each level up roughly halves the number of nodes a search has to pass through.

```cpp
// Compares (score, member) tuples the way Redis orders ZSET entries.
static bool lessThan(double s1, const std::string& m1, double s2, const std::string& m2) {
    if (s1 != s2) return s1 < s2;
    return m1 < m2;
}
```
`lessThan(s1, m1, s2, m2) -> bool`, a free (non-member) `static` function — internal linkage, visible only within this `.cpp` file. Implements the total ordering used everywhere in this file: primarily by `score` ascending; if scores are equal, tie-break by `member` using `std::string`'s lexicographic `<`. This single function is reused by `insert`, `remove`, and `rank` to keep the ordering rule consistent in one place.

### `SkipList::insert`

```cpp
bool SkipList::insert(double score, const std::string& member) {
    // ZADD semantics: if member already exists, this is an update (remove + reinsert
    // at the new score), not a duplicate entry.
    auto it = memberScore_.find(member);
    bool isNew = (it == memberScore_.end());
    if (!isNew) {
        if (it->second == score) return false; // no-op, nothing changed
        remove(member);
    }
```
`insert(score, member) -> bool`. First checks the side index (`memberScore_`) for the member.
- `it` — iterator result of the hash-map lookup.
- `isNew` — `true` if the member wasn't found (i.e., `it == end()`).
- If it *was* found (`!isNew`): if the score is unchanged, this is a genuine no-op — return `false` immediately without touching the list. Otherwise, the member needs to move to a new position, so the simplest correct approach is taken: remove the old node entirely (`remove(member)`), then fall through and insert fresh at the new score.

```cpp
    std::vector<SkipListNode*> update(maxLevel_, head_);
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] &&
               lessThan(cur->forward[i]->score, cur->forward[i]->member, score, member)) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }
```
The classic skip-list "find insertion point" walk.
- **`update`** — a vector sized `maxLevel_`, initialized so every slot defaults to `head_`. `update[i]` will end up holding "the node whose `forward[i]` pointer needs to be rewired to point at the new node" — i.e., the last node at level `i` that is still `lessThan` the value being inserted.
- **`cur`** — the current traversal pointer, starts at the sentinel.
- Outer loop: `i` counts *down* from the topmost active level (`level_ - 1`, since levels are 0-indexed internally) to `0`. Skip lists always start searches at the highest level and descend, because the top levels let you skip the most nodes per step.
- Inner `while`: at the current level `i`, keep moving `cur` forward as long as the *next* node (`cur->forward[i]`) is strictly less than `(score, member)` per the tuple ordering. This walks as far right as possible without overshooting.
- After the `while` exits (either `forward[i]` is null — end of that level's chain — or the next node is `>=` the target), `update[i] = cur` records where level `i`'s "insert after this node" point is.
- Then `i` decrements and the same `cur` (not reset) continues the walk one level down — this reuse of `cur` across levels is what makes the search sub-linear instead of restarting from `head_` at every level.

```cpp
    int newLevel = randomLevel();
    if (newLevel > level_) {
        for (int i = level_; i < newLevel; i++) update[i] = head_;
        level_ = newLevel;
    }
```
Decide the new node's height via the coin-flip helper. If it's taller than the list's current maximum (`level_`), the list needs to "grow": for every new level between the old `level_` and the new height, there's no existing node at that level yet, so the only valid predecessor is the sentinel `head_` itself — those `update[i]` slots are set to `head_` (overwriting the default they already had, for clarity/robustness). Then `level_` is raised to `newLevel`.

```cpp
    SkipListNode* node = new SkipListNode(score, member, newLevel);
    for (int i = 0; i < newLevel; i++) {
        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;
    }
```
Allocate the new node with exactly `newLevel` forward slots. Standard linked-list splice, done at every level the new node participates in: the new node's `forward[i]` takes over whatever `update[i]->forward[i]` was pointing to, and then `update[i]->forward[i]` is repointed at the new node. This is the "insert B between A and C" pattern (`A -> C` becomes `A -> B -> C`), repeated per level.

```cpp
    memberScore_[member] = score;
    count_++;
    return isNew;
}
```
Update the side index so O(1) lookups reflect the new score, bump the element count, and return whether this was a brand-new member (matching `ZADD`'s "1 if added, 0 if updated" reply convention — the `CommandProcessor` converts this bool to that integer).

### `SkipList::remove`

```cpp
bool SkipList::remove(const std::string& member) {
    auto it = memberScore_.find(member);
    if (it == memberScore_.end()) return false;
    double score = it->second;
```
`remove(member) -> bool`. Looks the member up in the side index first — if absent, nothing to do, return `false` immediately (this also means `remove` never has to scan the list to discover "not found"). `score` is needed because the list is ordered by `(score, member)`, so removal has to walk to the *same position* insertion would have used.

```cpp
    std::vector<SkipListNode*> update(maxLevel_, head_);
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] &&
               lessThan(cur->forward[i]->score, cur->forward[i]->member, score, member)) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }
```
Identical search pattern to `insert` — find, at every level, the predecessor of the target node. Same variables, same meaning.

```cpp
    SkipListNode* target = cur->forward[0];
    if (!target || target->member != member) return false; // shouldn't happen if index is consistent
```
After the walk, `cur->forward[0]` (the next node at the base level) should be the node to delete. Defensive check: if it's null or its member doesn't match, something is inconsistent (shouldn't happen given `memberScore_` and the list are kept in sync) — bail out safely rather than crash.

```cpp
    for (int i = 0; i < level_; i++) {
        if (update[i]->forward[i] != target) continue;
        update[i]->forward[i] = target->forward[i];
    }
    delete target;
```
Unsplice `target` from every level it appears on: for each level `i` up to the list's current height, if that level's predecessor's `forward[i]` actually points at `target` (a node only exists on levels `0..height-1`, so higher levels' predecessors won't point at it — the `continue` skips those), rewire it to skip over `target` directly to `target->forward[i]`. Then free the node's memory.

```cpp
    while (level_ > 1 && head_->forward[level_ - 1] == nullptr) level_--;
```
Shrink `level_` back down if the topmost level(s) are now empty (i.e., the tallest node(s) in the list were just removed) — keeps `level_` an accurate "highest level actually in use" so future searches don't waste steps descending through empty levels. Stops at `1` (never goes to 0).

```cpp
    memberScore_.erase(it);
    count_--;
    return true;
}
```
Remove from the side index using the iterator already held (avoids a second hash lookup), decrement the count, report success.

### `SkipList::getScore`

```cpp
std::optional<double> SkipList::getScore(const std::string& member) const {
    auto it = memberScore_.find(member);
    if (it == memberScore_.end()) return std::nullopt;
    return it->second;
}
```
Pure O(1) hash-map lookup — never touches the linked structure at all. Returns `nullopt` (an empty `optional`) if not found, otherwise the score.

### `SkipList::rangeByIndex`

```cpp
std::vector<std::pair<double, std::string>> SkipList::rangeByIndex(long start, long stop) const {
    std::vector<std::pair<double, std::string>> result;
    long n = static_cast<long>(count_);
    if (n == 0) return result;
```
`rangeByIndex(start, stop) -> vector<pair<double,string>>`. `result` accumulates the answer. `n` is the element count cast to `long` (to compare safely against the signed `start`/`stop` parameters — `count_` is `size_t`, unsigned). Empty list → empty result immediately.

```cpp
    if (start < 0) start = std::max(0L, n + start);
    if (stop < 0) stop = n + stop;
    if (stop >= n) stop = n - 1;
    if (start > stop || start >= n) return result;
```
Python-slice-style negative index normalization, matching Redis `ZRANGE`:
- Negative `start` counts from the end (`-1` = last element); `n + start` converts it, clamped up to `0` so an overly-negative start doesn't go below the first element.
- Negative `stop` is converted the same way (`n + stop`), *not* clamped to 0 here — if it's still negative after conversion, the next line's `start > stop` check will correctly produce an empty range.
- `stop` is clamped down to `n - 1` if it overshoots the end (asking for index 100 in a 5-element list just means "up to the last element").
- Final guard: if after normalization `start` is past `stop`, or `start` is at/past the end of the list, there's nothing to return (empty result).

```cpp
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
```
Plain linear walk along the level-0 chain (this is the one place the skip list is *not* used for its logarithmic advantage — a rank-based range still needs `O(start)` steps to reach the beginning of the window, since there's no positional index; the skip levels do help larger jumps but a straightforward implementation just walks level 0). First loop advances `cur`/`idx` until reaching position `start`. Second loop collects `(score, member)` pairs from `start` through `stop` inclusive into `result`, using `emplace_back` to construct the `pair` in place (avoids a copy). Returns the collected range.

### `SkipList::rangeByScore`

```cpp
std::vector<std::pair<double, std::string>> SkipList::rangeByScore(double minScore, double maxScore) const {
    std::vector<std::pair<double, std::string>> result;
    SkipListNode* cur = head_;
    for (int i = level_ - 1; i >= 0; i--) {
        while (cur->forward[i] && cur->forward[i]->score < minScore) {
            cur = cur->forward[i];
        }
    }
```
`rangeByScore(minScore, maxScore) -> vector<pair<double,string>>`. This one *does* use the multi-level structure to skip ahead quickly: same top-down, level-by-level walk pattern as `insert`/`remove`, but the condition is simply "keep going while the next node's score is still below `minScore`" (member name doesn't matter here — this walk is only trying to reach the first node whose score could possibly qualify).

```cpp
    cur = cur->forward[0];
    while (cur && cur->score <= maxScore) {
        result.emplace_back(cur->score, cur->member);
        cur = cur->forward[0];
    }
    return result;
}
```
After the level-skip search, `cur` is the last node with `score < minScore` (or the sentinel, if none qualify) at level 0. `cur->forward[0]` is therefore the first candidate node. Then it's a simple level-0 walk collecting nodes while `score <= maxScore`, stopping (implicitly, by the `while` condition failing) as soon as a score exceeds the upper bound — correct because the list is sorted by score, so once one node is too high, all subsequent ones are too.

### `SkipList::rank`

```cpp
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
```
`rank(member) -> long`. First confirms the member exists via the side index (O(1)) and gets its `score` (needed for the ordering comparison). Then the same top-down level walk as `insert`/`remove`, but this time it also tracks `idx`: every time `cur` actually advances at *any* level (`cur = cur->forward[i]`), that step passes over exactly one more element in the full ordering (regardless of which level the hop occurred at, since the skip list's structural invariant guarantees every node exists at level 0 too), so `idx++` alongside each advance keeps an accurate running count of "how many nodes have been passed so far" — this is the standard technique for getting `O(log n)` rank out of a skip list without a separate augmented "span" field per node (real Redis's skiplist implementation *does* add span counters for the same purpose, but this simpler version just counts hops directly).

```cpp
    // cur->forward[0] should now be the target node
    if (cur->forward[0] && cur->forward[0]->member == member) return idx;
    return -1;
}
```
After the walk, `cur->forward[0]` should be the member's own node (found via the score-based side index, we already know it must be there); `idx` at that point equals its 0-indexed rank. Defensive fallback returns `-1` if something is inconsistent.

---

## 3. `include/ZSetStore.h` and `src/ZSetStore.cpp`

This layer maps a **zset name** (e.g. `"leaderboard"`) to its own independent
`SkipList`, the way real Redis lets you have many different sorted sets
under different keys, each manipulated with `ZADD key score member`.

```cpp
#include "SkipList.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <optional>
```
Includes: `SkipList.h` (the type being stored), `<unordered_map>` (name→list map), `<memory>` (`std::unique_ptr` for owning the heap-allocated `SkipList`s), `<mutex>` (thread-safety), the rest as before.

```cpp
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
```
Each public method is a thin, key-qualified wrapper around the matching `SkipList` method (signatures/return types mirror `SkipList`'s, plus a leading `key` parameter to select which zset). `sets_` is the name→list map; using `std::unique_ptr<SkipList>` rather than storing `SkipList` by value is necessary because `SkipList`'s copy constructor is deleted (see above) and `std::unordered_map` needs to be able to move/rehash its values — a `unique_ptr` is cheaply movable regardless of what it points to. `mtx_` guards all access to `sets_` for thread safety (the background TTL-sweeper thread in `Store` runs concurrently with the REPL thread, and while `ZSetStore` itself isn't touched by that sweeper, this mutex protects it against any future concurrent callers and is consistent with `Store`'s locking discipline).

### `ZSetStore.cpp`

```cpp
bool ZSetStore::zadd(const std::string& key, double score, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& set = sets_[key];
    if (!set) set = std::make_unique<SkipList>();
    return set->insert(score, member);
}
```
`zadd(key, score, member) -> bool`. `std::lock_guard` acquires `mtx_` for the function's duration (released automatically via RAII when it goes out of scope, even on exceptions). `sets_[key]` — `operator[]` on `unordered_map` default-constructs an entry if `key` isn't present yet, giving a reference (`set`) to the (possibly just-created, null) `unique_ptr` slot. `if (!set)` — `unique_ptr` is contextually convertible to `bool`, `false` means it's empty/null — if so, this is a brand-new zset name, so allocate a fresh `SkipList` with default parameters via `std::make_unique`. Then delegate to the skip list's `insert`, returning its "was this a new member" bool.

```cpp
bool ZSetStore::zrem(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return false;
    bool removed = it->second->remove(member);
    if (removed && it->second->size() == 0) sets_.erase(it); // clean up empty zsets
    return removed;
}
```
`zrem(key, member) -> bool`. Look up the zset by `key`; if it doesn't exist at all, nothing to remove. Otherwise delegate to `SkipList::remove`. If removal succeeded *and* that emptied the list entirely, the map entry itself is erased (`sets_.erase(it)`) — this prevents an unbounded accumulation of empty `SkipList` objects for zsets whose members have all been removed, matching Redis's behavior where an empty key effectively stops existing.

```cpp
std::optional<double> ZSetStore::zscore(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return std::nullopt;
    return it->second->getScore(member);
}
```
`zscore(key, member) -> optional<double>`. If the zset doesn't exist, `nullopt`; otherwise delegate.

```cpp
std::vector<std::pair<double, std::string>> ZSetStore::zrange(const std::string& key, long start, long stop) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return {};
    return it->second->rangeByIndex(start, stop);
}
```
`zrange(key, start, stop) -> vector<pair<double,string>>`. Missing key → empty vector (`{}`); otherwise delegate to `rangeByIndex`.

```cpp
std::vector<std::pair<double, std::string>> ZSetStore::zrangebyscore(const std::string& key, double min, double max) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return {};
    return it->second->rangeByScore(min, max);
}
```
`zrangebyscore(key, min, max) -> vector<pair<double,string>>`. Same pattern, delegating to `rangeByScore`.

```cpp
long ZSetStore::zrank(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return -1;
    return it->second->rank(member);
}
```
`zrank(key, member) -> long`. Missing key → `-1` (same "not found" sentinel `SkipList::rank` itself uses); otherwise delegate.

```cpp
long ZSetStore::zcard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return 0;
    return static_cast<long>(it->second->size());
}
```
`zcard(key) -> long`. Missing key → `0` (an absent zset has zero members, matching Redis); otherwise cast `size_t` → `long` and return the count.

```cpp
bool ZSetStore::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return sets_.count(key) > 0;
}
```
`exists(key) -> bool`. `sets_.count(key)` returns `0` or `1` for a plain `unordered_map` (no duplicate keys), so `> 0` is equivalent to "is the key present". (Note: this method is declared but not currently called anywhere in `CommandProcessor` — it's available for future use, e.g. a `TYPE key` command.)

---

## 4. `include/Store.h` and `src/Store.cpp`

The core string key-value engine, with TTL (time-to-live) expiry handled
the same two ways real Redis does it: **lazy** (checked on every read) and
**active** (a background thread proactively reclaims idle expired keys).

### Header

```cpp
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
```
- `<queue>` → `std::priority_queue`, the min-heap of expiry times.
- `<chrono>` → `std::chrono::steady_clock` and duration types for TTL math.
- `<thread>` → `std::thread` for the background sweeper.
- `<atomic>` → `std::atomic<bool>` for the thread-safe "keep running" flag.
- `<condition_variable>` → lets the sweeper thread sleep efficiently instead of busy-polling.
- `<cstdint>` → `uint64_t` for the version counters.

```cpp
class Store {
public:
    using Clock = std::chrono::steady_clock;
```
`Clock` is a type alias for `std::chrono::steady_clock` — a monotonic clock (never jumps backward, unaffected by system time changes), the right choice for measuring durations/deadlines like TTLs (as opposed to `system_clock`, which is wall-clock time and can jump).

```cpp
    Store();
    ~Store();
```
Constructor starts the background sweeper thread; destructor stops it cleanly (see `.cpp`).

```cpp
    void set(const std::string& key, const std::string& value,
             std::optional<long> ttlSeconds = std::nullopt);
```
`set(key, value, ttlSeconds = nullopt) -> void`. `ttlSeconds` defaults to "no expiry" if omitted.

```cpp
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
```
- `get(key) -> optional<string>` — the stored value, or `nullopt` if missing/expired.
- `del(key) -> bool` — `true` if the key existed and was deleted.
- `exists(key) -> bool` — whether the key is currently live (accounting for expiry).

```cpp
    bool expire(const std::string& key, long seconds);
```
`expire(key, seconds) -> bool`. (Re)sets a TTL on an existing key. `false` if the key doesn't exist.

```cpp
    long ttl(const std::string& key);
```
`ttl(key) -> long`. Redis-style encoding: `-2` = key doesn't exist, `-1` = exists but has no expiry, otherwise seconds remaining.

```cpp
    std::vector<std::string> keys();
    size_t size();
```
- `keys() -> vector<string>` — all currently-live key names.
- `size() -> size_t` — raw count of entries in the underlying map (see note below on why this can differ subtly from "live" count).

```cpp
private:
    struct HeapEntry {
        Clock::time_point when;
        std::string key;
        uint64_t version;
        // priority_queue is a max-heap by default; we want soonest-expiry-first,
        // so invert the comparison to make it a min-heap on `when`.
        bool operator<(const HeapEntry& other) const { return when > other.when; }
    };
```
`HeapEntry` is what's stored in the expiry min-heap.
- **`when`** — the absolute time point at which this entry should expire.
- **`key`** — which key it refers to.
- **`version`** — a snapshot of the key's version counter *at the time this heap entry was created* (see below — this is the "lazy heap deletion" mechanism).
- **`operator<`** — `std::priority_queue` always pops the *largest* element first by default (using `operator<` to compare, keeping the "largest" at the top). To get min-heap-by-time behavior (soonest expiry popped first) without writing a custom comparator template argument, the `<` operator here is **inverted**: `a < b` is defined as `a.when > b.when`, so the priority_queue's notion of "a is less than b" actually corresponds to "a expires later than b" — meaning the entry the queue considers "largest" (and thus pops first) is the one with the smallest/soonest `when`.

```cpp
    std::unordered_map<std::string, std::string> data_;
    std::unordered_map<std::string, Clock::time_point> expireAt_;
    std::unordered_map<std::string, uint64_t> version_;
    std::priority_queue<HeapEntry> heap_;
```
- **`data_`** — the actual key→value store.
- **`expireAt_`** — key→expiry-time-point, only present for keys that currently have a TTL set. Absence from this map means "no expiry".
- **`version_`** — key→monotonically-increasing counter, bumped every time the key is written to (`set`) or re-`expire`d. This is the mechanism that lets stale heap entries be detected and ignored (explained below).
- **`heap_`** — the min-heap (by expiry time) of `HeapEntry` used by the active-expiry background thread to know what to check next without scanning the whole keyspace.

```cpp
    mutable std::mutex mtx_;
    std::thread sweeper_;
    std::atomic<bool> running_;
    std::condition_variable cv_;
```
- **`mtx_`** — guards *all* of the above state (`data_`, `expireAt_`, `version_`, `heap_`), since both the REPL/main thread and the background sweeper thread touch them concurrently. `mutable` so it can be locked even from `const`-qualified methods if any existed that needed read-only access under the lock (defensive/idiomatic; in this codebase no `const` method actually needs it, but it costs nothing and future-proofs the class).
- **`sweeper_`** — the background thread object running `activeExpireLoop`.
- **`running_`** — atomic flag the destructor flips to `false` to tell the sweeper loop to exit; atomic because it's read/written across threads without necessarily holding `mtx_` at every point (though here it's also guarded by the mutex for the write, out of caution — see destructor).
- **`cv_`** — used to wake the sweeper thread early (either because a new/updated TTL just got pushed onto the heap, or because the object is being destroyed) instead of always waiting out a fixed polling interval.

```cpp
    // Must hold mtx_ when calling these.
    bool isExpiredLocked(const std::string& key, Clock::time_point now) const;
    void eraseKeyLocked(const std::string& key);
    void activeExpireLoop();
};
```
Private helpers. The `...Locked` naming convention is a documentation convention (not compiler-enforced) signaling "caller must already hold `mtx_`" — both are called from within methods that have already taken the lock, so they don't lock it themselves (avoiding a deadlock from re-locking a non-recursive `std::mutex`).
- `isExpiredLocked(key, now) -> bool` — has `key`'s TTL passed?
- `eraseKeyLocked(key) -> void` — fully remove a key from all three maps.
- `activeExpireLoop() -> void` — the sweeper thread's entry point/main loop.

### `Store.cpp`

```cpp
Store::Store() : running_(true) {
    sweeper_ = std::thread(&Store::activeExpireLoop, this);
}
```
Constructor: initializes `running_` to `true`, then starts the background thread, giving it the member function pointer `&Store::activeExpireLoop` plus `this` (required for calling a non-static member function via `std::thread`, since it needs an object to invoke the method on).

```cpp
Store::~Store() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        running_ = false;
    }
    cv_.notify_all();
    if (sweeper_.joinable()) sweeper_.join();
}
```
Destructor: cleanly shuts down the sweeper thread. Takes the lock (extra `{}` block scopes the `lock_guard` so it releases immediately after), sets `running_ = false` (done under the lock so there's no race with the sweeper reading it right as it checks its loop condition), then `notify_all()` wakes the sweeper immediately if it's currently blocked in `cv_.wait_for`/`wait_until` (otherwise it might sleep up to 200ms — see `activeExpireLoop` — before noticing shutdown). `sweeper_.joinable()` checks the thread hasn't already been joined/detached; `join()` blocks until it actually finishes, ensuring no dangling thread survives the `Store` object being destroyed (which would be undefined behavior — the thread's `this` would become invalid).

```cpp
bool Store::isExpiredLocked(const std::string& key, Clock::time_point now) const {
    auto it = expireAt_.find(key);
    if (it == expireAt_.end()) return false;
    return it->second <= now;
}
```
`isExpiredLocked(key, now) -> bool`. If `key` has no entry in `expireAt_`, it has no TTL, so it can't be expired → `false`. Otherwise, expired if its stored expiry time is at or before `now`.

```cpp
void Store::eraseKeyLocked(const std::string& key) {
    data_.erase(key);
    expireAt_.erase(key);
    version_.erase(key);
}
```
`eraseKeyLocked(key) -> void`. Removes the key from all three maps (`unordered_map::erase` is a no-op if the key isn't present in a given map, so this is safe to call even if, say, `expireAt_` never had an entry for this key). Note: it does *not* remove any matching entries from `heap_` — those are left as "stale" and get silently discarded later when popped (see `activeExpireLoop`), because removing an arbitrary entry from a `priority_queue` isn't directly supported.

```cpp
void Store::set(const std::string& key, const std::string& value, std::optional<long> ttlSeconds) {
    std::lock_guard<std::mutex> lock(mtx_);
    data_[key] = value;
    uint64_t v = ++version_[key]; // bump version regardless, invalidating any older heap entries
```
`set(key, value, ttlSeconds) -> void`. Locks, writes the value. `version_[key]` — `operator[]` default-constructs a `uint64_t` (value `0`) if `key` isn't present yet; `++version_[key]` pre-increments it and `v` captures the new value. This bump happens **unconditionally**, even if this particular `set` doesn't specify a TTL — the comment explains why: it invalidates any older heap entry from a *previous* TTL on this key, so that a stale heap entry (from before this `set` call) won't later be mistaken for still being current.

```cpp
    if (ttlSeconds.has_value()) {
        auto when = Clock::now() + std::chrono::seconds(*ttlSeconds);
        expireAt_[key] = when;
        heap_.push(HeapEntry{when, key, v});
    } else {
        expireAt_.erase(key); // SET without EX clears any prior TTL, same as real Redis
    }
    cv_.notify_all();
}
```
If a TTL was given: compute the absolute expiry time (`Clock::now()` plus the duration), record it in `expireAt_`, and push a new `HeapEntry{when, key, v}` (using the just-incremented version `v`) onto the heap so the sweeper thread will eventually check it. If no TTL was given: erase any prior TTL entry for this key (matches real Redis's `SET` behavior — a plain `SET` clears any TTL that previously existed on that key, unlike `SET ... KEEPTTL` which this codebase doesn't implement). Finally `cv_.notify_all()` wakes the sweeper in case it was sleeping until a *later* time than this new/updated entry requires.

```cpp
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
```
`get(key) -> optional<string>`. This is the **lazy expiry** check: even if the background sweeper hasn't gotten around to this key yet, `get` first checks "is it actually expired right now?" — if so, erase it on the spot and report "not found", so an expired key can never appear to still exist just because the sweeper is running behind. Otherwise, look it up normally in `data_` and return the value (or `nullopt` if it was never set at all).

```cpp
bool Store::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    bool existed = data_.count(key) > 0;
    eraseKeyLocked(key);
    return existed;
}
```
`del(key) -> bool`. Checks existence *before* erasing (note: this doesn't check expiry first, so `del` on an already-expired-but-not-yet-swept key would report `true` — matches Redis semantics closely enough for this project's scope, though a strictly pedantic implementation might lazy-check expiry here too). Erases unconditionally (harmless no-op if it wasn't there), returns whether it had existed.

```cpp
bool Store::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = Clock::now();
    if (isExpiredLocked(key, now)) {
        eraseKeyLocked(key);
        return false;
    }
    return data_.count(key) > 0;
}
```
`exists(key) -> bool`. Same lazy-expiry pattern as `get`: check-and-reap first, then answer based on `data_`.

```cpp
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
```
`expire(key, seconds) -> bool`. Lazy-expiry check first (a key that's actually already expired can't have its TTL "refreshed"). Then confirms the key exists at all in `data_` (covers the case where it was never set). If both checks pass: compute the new absolute expiry `when`, store it, bump the version (invalidating any earlier heap entry for this key — same trick as in `set`), push the new heap entry, wake the sweeper, and report success.

```cpp
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
```
`ttl(key) -> long`. Lazy-reap if actually expired → `-2` ("doesn't exist", Redis's convention). If it's not in `data_` at all → also `-2`. If it exists but has no `expireAt_` entry → `-1` ("no TTL"). Otherwise compute `remaining = expireAt_[key] - now`, cast the `chrono::duration` down to whole seconds via `duration_cast`, and `.count()` extracts the raw number. The final ternary guards against a tiny negative value from rounding/race timing (e.g. the key expires in the next few microseconds but hasn't been lazily reaped by *this exact call path* yet) — clamps to `0` rather than reporting a nonsensical negative TTL.

```cpp
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
```
`keys() -> vector<string>`. `result.reserve(data_.size())` pre-allocates capacity for the worst case (all keys live) to avoid repeated reallocation during push_back. The range-for uses structured bindings (`auto& [k, v]`) to destructure each `unordered_map` entry into key `k` and value `v` (v is unused here, only `k` matters) — iterates every stored key and lazily filters out any that are actually expired (again, without physically erasing them here — that happens on the next `get`/`exists`/`ttl` touching that specific key, or whenever the sweeper gets to it).

```cpp
size_t Store::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_.size();
}
```
`size() -> size_t`. Raw entry count in `data_`. Note this can include keys that are logically expired but not yet lazily/actively reaped — a minor known imprecision consistent with a "prototype" scope (the README frames this whole project as a demonstration piece, not production Redis).

```cpp
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
```
`activeExpireLoop() -> void` — the sweeper thread's body, running for the lifetime of the `Store` object. Uses `std::unique_lock` (rather than `lock_guard`) because condition variables need a lock type that supports being temporarily released/reacquired internally by `wait_for`/`wait_until`.

- Outer `while (running_)` — keeps looping until the destructor signals shutdown.
- `now` — current time, read fresh each iteration.
- Inner `while (!heap_.empty() && heap_.top().when <= now)` — drains every heap entry that is currently due:
  - `top` — a **copy** of the min element (`heap_.top()` returns a `const&`; it's copied here because the next line pops it, which would invalidate a reference).
  - `heap_.pop()` — removes it from the heap.
  - `vit` — looks up the key's *current* version in `version_`.
  - **The core "lazy heap deletion" check**: only actually erase the key if it still has a version entry *and* that version still matches `top.version` (the version this heap entry was created with). If the key was `set`/`expire`d again since this heap entry was pushed, its live version will have moved on, `vit->second == top.version` will be `false`, and this stale entry is silently dropped — the key's *actual* current TTL has its own, newer heap entry that will fire at the right time later. This is exactly the technique described in `Store.h`'s file comment: since a `priority_queue` doesn't support arbitrary removal, superseded entries are left in place and simply ignored when they're eventually popped.
- After draining due entries: if the heap is now empty, there's nothing scheduled, so sleep up to 200ms (`cv_.wait_for`) — this bounded poll interval is a fallback safety net (e.g. in case of subtle race conditions) rather than the primary wake mechanism. If the heap isn't empty, sleep precisely until the next entry's due time (`cv_.wait_until(lock, heap_.top().when)`) — far more efficient than polling, since the thread only wakes exactly when needed (or earlier, if `notify_all()` is called by `set`/`expire`/the destructor, e.g. because a *sooner* entry was just added or because it's time to shut down).

---

## 5. `include/WAL.h` and `src/WAL.cpp`

Write-Ahead Log: durability mechanism. Every successful write command is
appended as a plain-text line to a log file immediately after it succeeds;
on startup the file is replayed to rebuild in-memory state. This mirrors
Redis's AOF (Append Only File) persistence mode.

### Header

```cpp
#include <string>
#include <fstream>
#include <functional>
#include <mutex>

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();
```
- `<fstream>` — file stream types (`std::ofstream`, `std::ifstream`).
- `<functional>` — `std::function`, used to accept an arbitrary callback for replay.
- Constructor `WAL(path)` — `explicit`, opens/creates the log file at `path`. Destructor closes it.

```cpp
    void append(const std::string& command);
```
`append(command) -> void`. Writes one command line to the log and flushes it.

```cpp
    // Reads the log file (if it exists) and calls executor(line) for each
    // command, in order, to rebuild in-memory state.
    void replay(const std::function<void(const std::string&)>& executor);
```
`replay(executor) -> void`. `executor` is a callback taking a `const std::string&` (one logged command line) and returning nothing — `replay` reads the file top to bottom and invokes `executor` once per line, in original order (order matters: it's what makes replaying deletions/overwrites end up in the correct final state).

```cpp
    size_t lineCount() const { return lineCount_; }

private:
    std::string path_;
    std::ofstream out_;
    std::mutex mtx_;
    size_t lineCount_ = 0;
};
```
- `lineCount() -> size_t` — trivial accessor for how many lines have been appended this session (declared but not currently used elsewhere in the codebase — available for diagnostics/future use).
- `path_` — the log file's path, remembered so `replay` (called separately from the constructor, from `main`) knows where to read from.
- `out_` — the open output stream used by `append`.
- `mtx_` — guards `append` (and, implicitly, `out_`/`lineCount_`) against concurrent writers — relevant if multiple threads ever called `execute` concurrently (currently `main`'s REPL loop is single-threaded, so this is defensive, but it costs nothing and matches the thread-safety discipline used in `Store`/`ZSetStore`).
- `lineCount_` — in-class default member initializer (`= 0`), counts appended lines.

### `WAL.cpp`

```cpp
WAL::WAL(const std::string& path) : path_(path) {
    // Open in append mode; the file is created if it doesn't exist yet.
    out_.open(path_, std::ios::app);
}
```
Constructor: stores `path_`, opens `out_` in `std::ios::app` (append) mode — writes always go to the end of the file, and the file is created automatically if it doesn't already exist (this is how the very first run of the program starts with a fresh, empty WAL).

```cpp
WAL::~WAL() {
    if (out_.is_open()) out_.close();
}
```
Destructor: closes the file stream if it's open (an `ofstream`'s destructor would do this automatically anyway, but it's made explicit here).

```cpp
void WAL::append(const std::string& command) {
    std::lock_guard<std::mutex> lock(mtx_);
    out_ << command << "\n";
    out_.flush(); // fsync-lite: durability over raw throughput, fine for a demo/prototype
    lineCount_++;
}
```
`append(command) -> void`. Under the lock: writes the raw command text plus a newline, then explicitly `flush()`es — forces the OS to actually write the buffered data out rather than leaving it sitting in the C++ stream buffer, so a crash immediately after this call still has the command durably on disk (the comment notes this trades some raw throughput for that durability guarantee — a real production system might batch/group commits, but for a demo this simple approach is fine). `lineCount_` is incremented to track how many entries have been appended.

```cpp
void WAL::replay(const std::function<void(const std::string&)>& executor) {
    std::ifstream in(path_);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        executor(line);
    }
}
```
`replay(executor) -> void`. Opens a fresh *input* stream on the same path (independent of `out_`, which stays open for appending). If the file couldn't be opened (e.g. genuinely doesn't exist yet, first-ever run), just return — nothing to replay. Otherwise reads line by line with `std::getline`; skips any empty lines (defensive, e.g. against a trailing blank line); for every non-empty line, calls the caller-supplied `executor` callback with it. In `main.cpp`, that callback is a lambda that calls `processor.execute(cmd, false)` — the `false` is critical (see `CommandProcessor::execute` below): it prevents replay from re-appending the very commands it's reading back in, which would make the log grow unboundedly every restart.

---

## 6. `include/CommandProcessor.h` and `src/CommandProcessor.cpp`

The dispatcher: parses one line of REPL/replay input into tokens, matches
it against the supported command set, calls into `Store`/`ZSetStore`, and
formats a Redis-CLI-style reply string. Also decides when a successful
write should be durably logged to the `WAL`.

### Header

```cpp
#include "Store.h"
#include "ZSetStore.h"
#include "WAL.h"
#include <string>
#include <vector>

class CommandProcessor {
public:
    CommandProcessor(Store& store, ZSetStore& zstore, WAL& wal);
```
Constructor takes **references** to the three collaborating engines (`Store&`, `ZSetStore&`, `WAL&`) — `CommandProcessor` doesn't own or create them, it just orchestrates calls into objects `main` owns; references (rather than pointers) express "these must always be valid, non-null, and outlive this object" without needing null checks.

```cpp
    // Parses and runs one line of input. If logToWAL is true and the command
    // is a write, it's appended to the WAL after successful execution.
    // Set logToWAL=false when replaying the WAL itself, to avoid duplicating entries.
    std::string execute(const std::string& line, bool logToWAL = true);
```
`execute(line, logToWAL = true) -> string`. The single public entry point: takes one raw input line, returns the formatted reply string. `logToWAL` defaults to `true` for normal interactive use; `main.cpp` passes `false` specifically during WAL replay at startup, per the comment.

```cpp
private:
    Store& store_;
    ZSetStore& zstore_;
    WAL& wal_;

    static std::vector<std::string> tokenize(const std::string& line);
    static bool isWriteCommand(const std::string& cmd);
    static std::string toUpper(std::string s);
};
```
The three reference members mirror the constructor parameters. Three `static` private helpers (no `this`/instance state needed, callable without an object — and in fact called both as `CommandProcessor::toUpper(...)` internally and, notably, `tokenize`/`isWriteCommand` are pure functions of their arguments):
- `tokenize(line) -> vector<string>` — splits a line into command tokens.
- `isWriteCommand(cmd) -> bool` — is this command name one that mutates state (and thus WAL-loggable)?
- `toUpper(s) -> string` — uppercases a string (takes `s` **by value**, since it needs to mutate a local copy and return it — this avoids a separate explicit copy inside the function body).

### `CommandProcessor.cpp`

```cpp
#include "CommandProcessor.h"
#include <sstream>
#include <algorithm>
#include <cctype>
```
`<sstream>` → `std::ostringstream` for building the reply string piece by piece. `<algorithm>` → `std::transform`, `std::find`. `<cctype>` → `std::toupper`, `std::isspace`.

```cpp
CommandProcessor::CommandProcessor(Store& store, ZSetStore& zstore, WAL& wal)
    : store_(store), zstore_(zstore), wal_(wal) {}
```
Constructor: binds the three reference members to the objects passed in (references must be initialized in the member-initializer list — they can't be assigned in the body, since a reference can't be rebound after initialization).

```cpp
std::string CommandProcessor::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}
```
`toUpper(s) -> string`. `std::transform` applies a function to every element in `[s.begin(), s.end())` and writes results starting at `s.begin()` (in place, here). The lambda takes `unsigned char c` (not plain `char`) before calling `std::toupper` — this matters because `std::toupper(int)` has undefined behavior if passed a negative value, which a plain (possibly signed) `char` holding a byte `>= 0x80` could produce; casting to `unsigned char` first avoids that pitfall. Returns the now-uppercased copy.

```cpp
// Splits on whitespace but respects "double quoted strings" so values like
//   SET greeting "hello world"
// are treated as a single token.
std::vector<std::string> CommandProcessor::tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}
```
`tokenize(line) -> vector<string>`. A small hand-written state-machine tokenizer.
- `tokens` — the accumulating result.
- `cur` — the token currently being built up character by character.
- `inQuotes` — tracks whether we're currently inside a `"..."` span.
- Loop over every character `c`:
  - If `c` is a `"`: this **toggles** `inQuotes` (doesn't add the quote character itself to `cur` — quotes are structural, not part of the token's content) and `continue`s to the next character.
  - Else if `c` is whitespace *and* we're not inside quotes: this is a token boundary — if `cur` has accumulated anything, push it onto `tokens` and reset `cur` for the next token. (If `cur` is already empty, e.g. multiple consecutive spaces, nothing happens — avoids producing empty tokens.)
  - Else (either non-whitespace, or whitespace *inside* quotes — meaning a space inside `"hello world"` is preserved): append `c` to `cur`.
- After the loop, if `cur` still has content (the line didn't end with whitespace), push the final token.
- This lets `SET greeting "hello world"` tokenize to `["SET", "greeting", "hello world"]` — three tokens, with the quoted phrase kept together as one.

```cpp
bool CommandProcessor::isWriteCommand(const std::string& cmd) {
    static const std::vector<std::string> writes = {
        "SET", "DEL", "EXPIRE", "ZADD", "ZREM"
    };
    return std::find(writes.begin(), writes.end(), cmd) != writes.end();
}
```
`isWriteCommand(cmd) -> bool`. `writes` is a `static const` local — initialized once, the first time this function is ever called, and reused across all subsequent calls (not reconstructed every call) — the fixed list of command names that mutate state and are therefore WAL-loggable. `std::find` does a linear scan for `cmd`; the function returns whether it was found (comparing the returned iterator against `end()`, the standard "not found" sentinel for STL search algorithms).

### `CommandProcessor::execute` — the main dispatcher

```cpp
std::string CommandProcessor::execute(const std::string& line, bool logToWAL) {
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) return "";

    std::string cmd = toUpper(tokens[0]);
    std::ostringstream reply;
```
`execute(line, logToWAL) -> string`. Tokenizes the input; an empty/whitespace-only line produces no tokens, in which case the function returns immediately with an empty reply (nothing to do, nothing printed — see `main.cpp`, which only prints non-empty replies anyway). `cmd` is the first token, uppercased, so command matching is case-insensitive (`set`, `SET`, `Set` all work). `reply` is the string-building buffer for the eventual response.

```cpp
    auto wrongArgs = [&](const std::string& c) {
        return "(error) ERR wrong number of arguments for '" + c + "' command";
    };
```
`wrongArgs` — a local lambda capturing by reference (`[&]`, though it doesn't actually need any captures here — this is a minor stylistic choice; a `[]`/value-capture would work identically since `c` is a parameter). Given a command name, builds the standard "wrong arity" error message text used repeatedly below, avoiding duplicating that string-concatenation pattern for every command.

```cpp
    bool ok = true; // whether this write should be logged to the WAL
```
`ok` — starts `true`, and is flipped to `false` inside specific branches below whenever a command either (a) isn't a mutating command at all (reads like `GET`, `TTL`, `KEYS`, ...), or (b) *is* normally a write command but this particular invocation didn't actually change anything (e.g. `DEL` on a key that didn't exist, `EXPIRE` on a missing key, `ZREM` on a non-member) — in both cases there's no reason to append this line to the WAL. This is what makes the final WAL-logging decision (see the very end of the function) correct without needing a separate "did anything change" return channel from every branch.

The body is then a long `if / else if` chain dispatching on `cmd`. Each branch is explained below.

**`PING`**
```cpp
    if (cmd == "PING") {
        reply << "PONG";
        ok = false;
```
No arguments expected/checked. Standard liveness-check reply. `ok = false` — `PING` isn't a write command anyway (and isn't even in `isWriteCommand`'s list), but setting `ok = false` here makes the "don't log this" intent explicit and consistent with the rest of the branches (defensive/self-documenting, since the final `isWriteCommand(cmd)` check would already exclude `PING` regardless).

**`SET key value [EX seconds]`**
```cpp
    } else if (cmd == "SET") {
        // SET key value [EX seconds]
        if (tokens.size() < 3) return wrongArgs("SET");
        std::optional<long> ttl = std::nullopt;
        if (tokens.size() >= 5 && toUpper(tokens[3]) == "EX") {
            try {
                ttl = std::stol(tokens[4]);
            } catch (...) {
                return "(error) ERR value is not an integer or out of range";
            }
        } else if (tokens.size() != 3) {
            return wrongArgs("SET");
        }
        store_.set(tokens[1], tokens[2], ttl);
        reply << "OK";
```
Requires at least `SET key value` (3 tokens: command + key + value); fewer → wrong-args error. `ttl` defaults to no expiry. If there are at least 5 tokens and the 4th token (index 3: `SET key value EX ...`) is `"EX"` (case-insensitively, via `toUpper`), it tries to parse the 5th token (index 4) as the TTL seconds via `std::stol` (string-to-long); a malformed number throws an exception, caught by the catch-all `catch (...)`, producing a Redis-style "not an integer" error and returning immediately (this early `return` skips the WAL-logging step entirely, which is correct — the command never actually executed). If the token count is neither exactly 3 (`SET key value`) nor the valid 5-with-`EX` form, it's a malformed invocation → wrong-args error (this also catches e.g. `SET key value badword` where token 4 isn't `EX`, or `SET key value EX` missing the number). If all checks pass: calls `store_.set(key, value, ttl)` and replies `"OK"`.

**`GET key`**
```cpp
    } else if (cmd == "GET") {
        if (tokens.size() != 2) return wrongArgs("GET");
        auto v = store_.get(tokens[1]);
        ok = false;
        if (v.has_value()) reply << "\"" << *v << "\"";
        else reply << "(nil)";
```
Requires exactly 2 tokens (`GET key`). Fetches the value; `ok = false` since `GET` is a pure read (never WAL-logged). If present, replies with the value wrapped in escaped double quotes (`\"..\"`, mimicking `redis-cli`'s display convention for string replies); otherwise `(nil)` (Redis's standard "no such key" reply).

**`DEL key [key2 ...]`**
```cpp
    } else if (cmd == "DEL") {
        if (tokens.size() < 2) return wrongArgs("DEL");
        long count = 0;
        for (size_t i = 1; i < tokens.size(); i++) if (store_.del(tokens[i])) count++;
        reply << "(integer) " << count;
        if (count == 0) ok = false; // nothing actually changed, no need to log
```
Requires at least one key (2+ tokens). Loops over every token from index 1 onward (supports multiple keys in one `DEL` call, per the header comment `DEL key...`), calling `store_.del` on each and tallying how many actually existed and were removed into `count`. Replies with `(integer) N` (Redis integer-reply convention). If `count == 0` — none of the given keys existed, so nothing actually changed — `ok` is set `false`, skipping the WAL write (no point recording a no-op delete).

**`EXISTS key`**
```cpp
    } else if (cmd == "EXISTS") {
        if (tokens.size() != 2) return wrongArgs("EXISTS");
        reply << "(integer) " << (store_.exists(tokens[1]) ? 1 : 0);
        ok = false;
```
Exactly 2 tokens. Replies `1`/`0` as an integer reply. Pure read → `ok = false`.

**`EXPIRE key seconds`**
```cpp
    } else if (cmd == "EXPIRE") {
        if (tokens.size() != 3) return wrongArgs("EXPIRE");
        long seconds;
        try { seconds = std::stol(tokens[2]); } catch (...) {
            return "(error) ERR value is not an integer or out of range";
        }
        bool did = store_.expire(tokens[1], seconds);
        reply << "(integer) " << (did ? 1 : 0);
        if (!did) ok = false;
```
Exactly 3 tokens (`EXPIRE key seconds`). Parses `seconds` from token 2, with the same try/catch-and-early-return pattern as `SET`'s `EX` clause for a malformed number. Calls `store_.expire`; `did` tells whether the key existed and got the new TTL. Replies `1`/`0`. If it didn't actually happen (`!did`), `ok = false` — no WAL entry for a no-op.

**`TTL key`**
```cpp
    } else if (cmd == "TTL") {
        if (tokens.size() != 2) return wrongArgs("TTL");
        reply << "(integer) " << store_.ttl(tokens[1]);
        ok = false;
```
Exactly 2 tokens. Directly reports `Store::ttl`'s `-2`/`-1`/seconds-remaining encoding as an integer reply. Pure read → `ok = false`.

**`KEYS`**
```cpp
    } else if (cmd == "KEYS") {
        auto ks = store_.keys();
        ok = false;
        if (ks.empty()) { reply << "(empty array)"; }
        else {
            for (size_t i = 0; i < ks.size(); i++) {
                if (i) reply << "\n";
                reply << (i + 1) << ") \"" << ks[i] << "\"";
            }
        }
```
No arguments (note: unlike other commands, `tokens.size()` isn't validated here at all — any extra tokens after `KEYS` are silently ignored, which is a minor looseness compared to the stricter arity checks elsewhere). Fetches all live keys; pure read → `ok = false`. If none, replies `(empty array)` (Redis-CLI's convention for an empty multi-bulk reply). Otherwise formats each key on its own line, numbered from 1, in the `redis-cli` list-reply style: `1) "keyname"`, `2) "keyname2"`, etc. — `if (i)` (true for every index except `0`) prepends a newline before each entry except the first, so entries are newline-separated without a leading blank line.

**`DBSIZE`**
```cpp
    } else if (cmd == "DBSIZE") {
        reply << "(integer) " << store_.size();
        ok = false;
```
No arguments checked. Reports the raw entry count as an integer reply. Pure read → `ok = false`.

**`ZADD key score member`**
```cpp
    } else if (cmd == "ZADD") {
        // ZADD key score member
        if (tokens.size() != 4) return wrongArgs("ZADD");
        double score;
        try { score = std::stod(tokens[2]); } catch (...) {
            return "(error) ERR value is not a valid float";
        }
        bool isNew = zstore_.zadd(tokens[1], score, tokens[3]);
        reply << "(integer) " << (isNew ? 1 : 0);
```
Requires exactly 4 tokens. Parses the score as a `double` via `std::stod` (string-to-double), with the try/catch pattern producing a float-specific error message on failure. Delegates to `zstore_.zadd`; replies `1` if the member was newly added, `0` if it was an update to an existing member's score — matching Redis's `ZADD` integer-reply convention (note there's no explicit `ok = false` branch here — `ZADD` is always considered a "real" write worth logging, even when it's just updating an existing member's score, since the score value did change or at minimum the command is idempotent-safe to replay).

**`ZREM key member`**
```cpp
    } else if (cmd == "ZREM") {
        if (tokens.size() != 3) return wrongArgs("ZREM");
        bool removed = zstore_.zrem(tokens[1], tokens[2]);
        reply << "(integer) " << (removed ? 1 : 0);
        if (!removed) ok = false;
```
Exactly 3 tokens. Delegates to `zstore_.zrem`; replies `1`/`0`. If nothing was actually removed, `ok = false` (no WAL entry for a no-op, same pattern as `DEL`/`EXPIRE`).

**`ZSCORE key member`**
```cpp
    } else if (cmd == "ZSCORE") {
        if (tokens.size() != 3) return wrongArgs("ZSCORE");
        auto s = zstore_.zscore(tokens[1], tokens[2]);
        ok = false;
        if (s.has_value()) reply << "\"" << *s << "\"";
        else reply << "(nil)";
```
Exactly 3 tokens. Pure read → `ok = false`. Formats the score (if present) quoted like `GET`'s string reply; `(nil)` if the member/key doesn't exist.

**`ZRANGE key start stop`**
```cpp
    } else if (cmd == "ZRANGE") {
        if (tokens.size() != 4) return wrongArgs("ZRANGE");
        long start, stop;
        try { start = std::stol(tokens[2]); stop = std::stol(tokens[3]); } catch (...) {
            return "(error) ERR value is not an integer or out of range";
        }
        ok = false;
        auto members = zstore_.zrange(tokens[1], start, stop);
        if (members.empty()) reply << "(empty array)";
        else {
            for (size_t i = 0; i < members.size(); i++) {
                if (i) reply << "\n";
                reply << (i + 1) << ") \"" << members[i].second << "\" (score: " << members[i].first << ")";
            }
        }
```
Exactly 4 tokens. Parses both `start` and `stop` as `long`s in one `try`, sharing one catch block. Pure read → `ok = false`. Formats results the same numbered-list style as `KEYS`, but each line also shows `(score: N)` after the member name — `members[i].second` is the member string, `members[i].first` is the score (recall the pair type is `pair<double, string>`, i.e. `.first` = score, `.second` = member).

**`ZRANGEBYSCORE key min max`**
```cpp
    } else if (cmd == "ZRANGEBYSCORE") {
        if (tokens.size() != 4) return wrongArgs("ZRANGEBYSCORE");
        double lo, hi;
        try { lo = std::stod(tokens[2]); hi = std::stod(tokens[3]); } catch (...) {
            return "(error) ERR value is not a valid float";
        }
        ok = false;
        auto members = zstore_.zrangebyscore(tokens[1], lo, hi);
        if (members.empty()) reply << "(empty array)";
        else {
            for (size_t i = 0; i < members.size(); i++) {
                if (i) reply << "\n";
                reply << (i + 1) << ") \"" << members[i].second << "\" (score: " << members[i].first << ")";
            }
        }
```
Same structure as `ZRANGE`, but parses `lo`/`hi` as `double`s (score bounds, not index bounds) and delegates to `zrangebyscore`. Identical output formatting.

**`ZRANK key member`**
```cpp
    } else if (cmd == "ZRANK") {
        if (tokens.size() != 3) return wrongArgs("ZRANK");
        long r = zstore_.zrank(tokens[1], tokens[2]);
        ok = false;
        if (r < 0) reply << "(nil)";
        else reply << "(integer) " << r;
```
Exactly 3 tokens. Pure read → `ok = false`. `zrank` returns `-1` for "not found" — translated here to `(nil)`, matching how `GET`/`ZSCORE` represent absence, rather than leaking the raw `-1` sentinel to the user.

**`ZCARD key`**
```cpp
    } else if (cmd == "ZCARD") {
        if (tokens.size() != 2) return wrongArgs("ZCARD");
        reply << "(integer) " << zstore_.zcard(tokens[1]);
        ok = false;
```
Exactly 2 tokens. Pure read → `ok = false`. Reports member count as an integer reply.

**`HELP`**
```cpp
    } else if (cmd == "HELP") {
        ok = false;
        reply <<
            "Supported commands:\n"
            "  Strings:  SET key value [EX seconds] | GET key | DEL key... | EXISTS key\n"
            "            EXPIRE key seconds | TTL key | KEYS | DBSIZE\n"
            "  Sorted sets: ZADD key score member | ZREM key member | ZSCORE key member\n"
            "               ZRANGE key start stop | ZRANGEBYSCORE key min max\n"
            "               ZRANK key member | ZCARD key\n"
            "  Other:    PING | HELP | EXIT / QUIT";
```
No arguments checked. Not a write → `ok = false`. Just streams a static multi-line usage summary into `reply` (adjacent string literals are concatenated by the compiler at compile time into one string).

**Unknown command**
```cpp
    } else {
        return "(error) ERR unknown command '" + tokens[0] + "'";
    }
```
Fallback `else` for any `cmd` that matched none of the above branches — returns an error immediately (note: uses the **original** `tokens[0]`, not the uppercased `cmd`, so the error message echoes back exactly what the user typed). This early `return` also means unknown commands never reach the WAL-logging step at the end, which is correct (nothing happened, there's nothing to log).

**Finishing up**
```cpp
    if (ok && logToWAL && isWriteCommand(cmd)) {
        wal_.append(line);
    }

    return reply.str();
}
```
After whichever branch ran (and didn't early-`return`), this final check decides whether to persist the command: all three conditions must hold — `ok` (this specific invocation actually changed state, or wasn't explicitly marked as a no-op/read), `logToWAL` (the caller allows logging — `false` during replay, so replaying the log doesn't re-append the very lines being replayed), and `isWriteCommand(cmd)` (the command is structurally one of `SET`/`DEL`/`EXPIRE`/`ZADD`/`ZREM`). If all three are true, the **original raw `line`** (not the tokenized/reconstructed version) is appended to the WAL verbatim — this preserves exact original formatting/casing for replay. Finally, `reply.str()` extracts the accumulated string from the `ostringstream` and returns it to the caller (`main`'s REPL loop, which prints it).

---

## 7. `src/main.cpp`

The program's entry point: wires the four components together, replays
any existing WAL to restore prior state, then runs an interactive
read-eval-print loop (REPL) over standard input.

```cpp
#include "Store.h"
#include "ZSetStore.h"
#include "WAL.h"
#include "CommandProcessor.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
```
`main(argc, argv) -> int` — the standard C++ program entry point. `argc` is the argument count (including the program name itself as `argv[0]`), `argv` is the array of C-string arguments.

```cpp
    std::string walPath = (argc > 1) ? argv[1] : "miniredis.aof";
```
If the program was invoked with at least one extra argument (`argc > 1`, i.e. `./miniredis somefile.aof`), use `argv[1]` as the WAL file path; otherwise default to `"miniredis.aof"` in the current working directory. `.aof` mirrors Redis's own "Append Only File" naming convention.

```cpp
    Store store;
    ZSetStore zstore;
    WAL wal(walPath);
    CommandProcessor processor(store, zstore, wal);
```
Constructs the four core objects, in dependency order: the two data engines first (`store` starts its background sweeper thread immediately here), then `wal` (opens/creates the log file), then `processor`, which takes references to all three — this is why `store`/`zstore`/`wal` must be declared (and thus constructed, and remain alive) before `processor` is constructed and for as long as `processor` is used; as local variables in `main`, they're all destroyed in reverse order automatically when `main` returns, which is also the correct shutdown order (processor stops being used first, then wal/store/zstore clean up).

```cpp
    // Rebuild in-memory state from whatever was persisted last time.
    // logToWAL=false so replaying doesn't re-append what we just read.
    size_t before = store.size();
    wal.replay([&](const std::string& cmd) { processor.execute(cmd, false); });
    size_t after = store.size();
```
`before`/`after` bracket the replay so the program can report how much was restored. `wal.replay(...)` is passed a lambda that captures its enclosing scope by reference (`[&]`) — giving it access to `processor` — and for each logged line calls `processor.execute(cmd, false)`. The `false` for `logToWAL` is essential: without it, replaying past commands would immediately re-append each one right back to the same WAL file, doubling it (and, worse, doubling it again on the *next* startup, growing without bound).

```cpp
    std::cout << "mini-redis :: terminal key-value store with TTL + sorted sets\n";
    std::cout << "persistence file: " << walPath << "\n";
    if (after > before) {
        std::cout << "restored " << after << " key(s) from previous session\n";
    }
    std::cout << "type HELP for commands, EXIT to quit\n\n";
```
Startup banner. Reports the WAL path, and — only if replay actually added keys (`after > before`) — a "restored N key(s)" message (skipped entirely on a genuinely fresh first run where there was nothing to restore).

```cpp
    std::string line;
    while (true) {
        std::cout << "miniredis> ";
        if (!std::getline(std::cin, line)) break; // EOF (e.g. piped input, Ctrl+D)
```
The REPL loop proper. `line` holds each input line. Prints the `miniredis> ` prompt (no trailing newline, so the user types on the same line). `std::getline(std::cin, line)` reads one line from standard input into `line`; it returns a reference to the stream, which is then implicitly converted to `bool` — `false` on failure/EOF (e.g. input is piped from a file/script and has run out, or the user pressed Ctrl+D on Unix/Ctrl+Z-Enter on Windows). `!...` negates that, so the loop `break`s cleanly on EOF instead of looping forever on a stream that can no longer produce input.

```cpp
        // trim leading/trailing whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
```
Manual whitespace trimming (handles spaces, tabs, carriage returns — relevant on Windows where lines can end in `\r\n` — and newlines, though `getline` itself already strips the trailing `\n`). `find_first_not_of` finds the index of the first character that *isn't* one of those whitespace characters; if the entire line is whitespace (or empty), it returns `std::string::npos` (the "not found" sentinel), in which case the loop `continue`s — skips straight to prompting again, without ever calling `execute` on a blank line. Otherwise, `find_last_not_of` finds the last non-whitespace character's index, and `line.substr(start, end - start + 1)` extracts just the "core" substring from `start` through `end` inclusive (length `end - start + 1`), reassigning it back to `line` — this is what strips leading/trailing whitespace.

```cpp
        std::string upper = line;
        for (auto& c : upper) c = std::toupper(static_cast<unsigned char>(c));
        if (upper == "EXIT" || upper == "QUIT") break;
```
Makes an uppercased copy `upper` (via a range-for that mutates each character in place through the reference `c`, casting through `unsigned char` for the same `std::toupper` safety reason discussed in `toUpper` above). If the whole trimmed line is exactly `EXIT` or `QUIT` (case-insensitive), the loop `break`s, ending the REPL. This check happens on `upper`/`line` directly rather than going through `CommandProcessor` — a deliberate special case, since exiting the program isn't really a "command" the processor needs to know about.

```cpp
        std::string reply = processor.execute(line, true);
        if (!reply.empty()) std::cout << reply << "\n";
    }
```
For any other input, delegates to `processor.execute(line, true)` — `true` here means "this is live interactive input, log writes to the WAL normally" (as opposed to the `false` used during startup replay). If the reply isn't empty (recall `execute` returns `""` for a genuinely empty tokenized line, though that case is already filtered out by the trim-and-`continue` above, making this check somewhat defensive/redundant but harmless), print it followed by a newline.

```cpp
    std::cout << "bye\n";
    return 0;
}
```
After the loop ends (either EOF or `EXIT`/`QUIT`), print a farewell message. `return 0` signals successful process exit to the OS. As `main` returns, `processor`, `wal`, `zstore`, and `store` are destroyed in reverse declaration order — notably, `store`'s destructor (see `Store::~Store`) is what cleanly stops the background sweeper thread before the process actually exits.

---

## 8. `Makefile`

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
TARGET := miniredis
```
- `CXX` — which compiler to invoke; `:=` is a "simply expanded" (immediate) assignment in Make, evaluated once at parse time.
- `CXXFLAGS` — compiler flags: `-std=c++17` (the C++ standard version this code requires — it uses `std::optional`, structured bindings, etc., all C++17 features); `-Wall -Wextra` (enable broad sets of compiler warnings, to catch bugs early); `-O2` (moderate optimization level); `-Iinclude` (add the `include/` directory to the header search path, so `#include "Store.h"` etc. resolve); `-pthread` (link the POSIX threads library and enable thread-safety-related compiler/library behavior — required because the code uses `std::thread`, `std::mutex`, `std::condition_variable`).
- `SRC` — `$(wildcard src/*.cpp)` expands to every `.cpp` file in `src/` at Make's parse time (currently: `SkipList.cpp`, `Store.cpp`, `WAL.cpp`, `ZSetStore.cpp`, `CommandProcessor.cpp`, `main.cpp`).
- `OBJ` — a Make pattern substitution: for each path in `SRC` matching `src/%.cpp`, produce the corresponding `build/%.o` — i.e. `src/main.cpp` → `build/main.o`, etc. This is how source files map to their compiled object files.
- `TARGET` — the final executable's name, `miniredis` (on Windows/MinGW, the actual produced file will be `miniredis.exe`, since MinGW's `g++` automatically appends `.exe`).

```makefile
.PHONY: all clean run

all: $(TARGET)
```
`.PHONY` declares `all`, `clean`, `run` as targets that don't correspond to real files with those names — prevents Make from getting confused if a file literally named `clean` or `run` ever existed in the directory. `all` (the default goal, since it's the first target defined) depends on `$(TARGET)` — running plain `make` builds the executable.

```makefile
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^
```
Link rule: the executable depends on all object files (`$(OBJ)`); rebuilding is triggered if any of them are newer than the existing executable (or it doesn't exist yet). The recipe invokes the compiler as the linker: `$@` is an automatic variable meaning "this rule's target" (`miniredis`), `$^` means "all prerequisites" (every `.o` file) — so this expands to something like `g++ -std=c++17 ... -o miniredis build/SkipList.o build/Store.o ...`.

```makefile
build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
```
Pattern rule: how to build any `build/X.o` from the corresponding `src/X.cpp`. The `| build` is an **order-only prerequisite** — it ensures the `build` directory exists before compiling, without making Make think the object file needs rebuilding just because the directory's timestamp changed. Recipe: `-c` compiles to an object file without linking; `$<` is "the first prerequisite" (the `.cpp` file), `$@` is "this rule's target" (the `.o` file).

```makefile
build:
	mkdir -p build
```
Simple rule that creates the `build/` directory (`-p` avoids an error if it already exists, and creates parent directories as needed — though there's only one level here).

```makefile
run: all
	./$(TARGET)
```
`run` depends on `all` (so it always builds first if needed), then executes the compiled program directly. Note the explicit `./` prefix — required on Linux/macOS to run an executable in the current directory (on native Windows `cmd`/PowerShell this isn't required, but also isn't harmful when using `make` inside an MSYS2/MinGW shell — see the build instructions in `README.md` for the Windows-specific equivalent).

```makefile
clean:
	rm -rf build $(TARGET) miniredis.aof
```
Removes all build artifacts: the `build/` directory (and everything in it), the compiled executable, and — notably — the WAL/persistence file `miniredis.aof` too, meaning `make clean` wipes any saved data, not just compiled output (worth knowing before running it if you want to keep your stored keys!).

---

## Summary: how a command flows through the system

1. `main.cpp`'s REPL loop reads and trims a line, hands it to `CommandProcessor::execute`.
2. `execute` tokenizes it (`tokenize`, quote-aware), uppercases the command name, and dispatches via the `if/else if` chain.
3. The matched branch validates argument count, parses any numeric arguments, and calls into `Store` (strings/TTL) or `ZSetStore` → `SkipList` (sorted sets).
4. `Store` methods take `mtx_`, do lazy-expiry checks via `isExpiredLocked`, mutate `data_`/`expireAt_`/`version_`/`heap_` as needed, and — for TTL changes — wake the background `activeExpireLoop` thread via `cv_.notify_all()`.
5. `ZSetStore` methods take its own `mtx_` and delegate to the right `SkipList`'s `insert`/`remove`/`rangeByIndex`/`rangeByScore`/`rank`/`getScore`, each of which walks the multi-level `forward` pointers top-down for `O(log n)` behavior, consulting the `memberScore_` side index for O(1) existence/score checks.
6. `execute` formats a Redis-CLI-style reply string, and — if the command was a genuine, successful write (`ok && logToWAL && isWriteCommand(cmd)`) — appends the original raw line to the `WAL`, which immediately flushes it to disk.
7. `main` prints the reply and loops back for the next line, until `EXIT`/`QUIT`/EOF.
8. On the *next* process startup, `WAL::replay` reads every previously-logged line back in order and re-executes each one through `CommandProcessor::execute(cmd, false)` — rebuilding `Store`'s and `ZSetStore`'s in-memory state to match whatever was durably logged, without re-logging those same lines again.
