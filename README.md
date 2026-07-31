# mini-redis

A terminal-based, single-node in-memory key-value store inspired by Redis —
built to demonstrate applied data structures, not just implement them in
isolation.

## Why this project

Every piece of Redis maps to a specific DSA decision. This project rebuilds
the core ones from scratch in C++:

| Redis feature      | Data structure used here                         | Why                                                              |
|---------------------|---------------------------------------------------|-------------------------------------------------------------------|
| String GET/SET      | `std::unordered_map`                              | O(1) average lookup/insert                                        |
| Key expiry (TTL)     | Min-heap (`priority_queue`) + lazy versioning     | Active background reclamation without scanning the whole keyspace |
| Sorted sets (ZSET)  | Custom **skip list**                              | O(log n) insert/rank/range — the same structure Redis itself uses |
| Durability          | Append-only write-ahead log (WAL), replayed on boot | Survive a crash/restart without a full snapshot                   |

## Architecture

```
include/            Headers
  SkipList.h           generic skip list backing each ZSET
  Store.h              string KV store + TTL engine
  ZSetStore.h          name -> SkipList map (multiple sorted sets)
  WAL.h                append-only log + replay
  CommandProcessor.h   parses REPL input, dispatches, formats output

src/                 Matching .cpp implementations + main.cpp (REPL loop)
```

### TTL expiry: lazy + active, with version tagging

Two mechanisms work together, same as real Redis:

1. **Lazy expiry** — every `GET`/`EXISTS`/`TTL` checks the key's expiry time
   before returning anything, so an expired key never appears alive even if
   the background sweep hasn't gotten to it yet.
2. **Active expiry** — a background thread wakes up (either periodically or
   exactly when the next key is due) and pops the soonest-to-expire entries
   off a min-heap, so idle expired keys still get reclaimed from memory.

The tricky part: a key's TTL can be overwritten before the old timer fires
(e.g. `SET k v EX 100` then `EXPIRE k 5`). Old heap entries can't be removed
from a `priority_queue` in place, so each key has a monotonically increasing
**version number**. A heap entry is only acted on if its version still
matches the key's current version — otherwise it's a stale leftover and gets
silently discarded. This is the standard "lazy deletion from a heap" pattern.

### Skip list for sorted sets

Redis itself backs ZSETs with a skip list rather than a balanced tree,
because it gives O(log n) search/insert/range operations with much simpler
code and good cache behavior. `SkipList.h/.cpp` implements insert, remove,
score lookup, rank, and both index-based (`ZRANGE`) and score-based
(`ZRANGEBYSCORE`) range queries.

### Persistence (WAL)

Every successful write command (`SET`, `DEL`, `EXPIRE`, `ZADD`, `ZREM`) is
appended as plain text to `miniredis.aof` immediately after it succeeds. On
startup, the file is replayed line-by-line to rebuild state — deletions and
overwrites replay in original order, so the final state is correct.

## Code walkthrough

See [`CODE_EXPLAINED.md`](./CODE_EXPLAINED.md) for a line-by-line explanation
of every file, class, function (with parameters/return types), variable,
condition, and data structure in this codebase.

## Building

Requires g++ with C++17 and pthreads (standard on Linux/macOS). For a
Windows/MinGW setup, see the dedicated section below.

```bash
make          # builds ./miniredis
make run      # builds and starts the REPL
make clean    # removes build artifacts and the persistence file
```

## Building on Windows with MinGW (g++)

This project uses `std::thread`, `std::mutex`, and `std::condition_variable`
(in `Store`'s background TTL-expiry sweeper), so the compiler toolchain
**must** support POSIX threading. Plain MinGW-w64 ships two threading
models — `win32` and `posix` — and only the **posix** variant implements
`<thread>`/`<mutex>`/`<condition_variable>`. If you try to build with a
`win32`-threads MinGW, compilation will fail (or `std::thread` will be
missing) — check which one you have before building:

```bash
g++ --version
g++ -v 2>&1 | findstr /i thread   # PowerShell/cmd: look for "posix" in the output
```

If it reports `win32` threads, install a `posix`-threads build instead
(e.g. via [MSYS2](https://www.msys2.org/): `pacman -S mingw-w64-x86_64-gcc`,
which defaults to posix threads; or via
[WinLibs](https://winlibs.com/)/[Nuwen MinGW](https://nuwen.net/mingw.html),
picking the `posix` thread-model download).

### Option A — using `make` (MSYS2 / MSYS2-MinGW shell, or Git Bash + make)

If your MinGW install includes `mingw32-make` (or plain `make`, as in an
MSYS2 shell), the existing `Makefile` works as-is:

```bash
mingw32-make          # or: make
mingw32-make run
mingw32-make clean
```

This produces `miniredis.exe` in the project root (MinGW's `g++`
automatically appends `.exe` even though the `Makefile`'s `TARGET` is just
`miniredis`).

### Option B — compiling directly, no `make` required

If you're using a plain **Windows Command Prompt** or **PowerShell** with
MinGW's `g++` on your `PATH` (no `make`/`mingw32-make` installed), compile
and link in one step:

```powershell
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread ^
    src\CommandProcessor.cpp src\SkipList.cpp src\Store.cpp ^
    src\WAL.cpp src\ZSetStore.cpp src\main.cpp ^
    -o miniredis.exe
```

(`^` is the Command Prompt line-continuation character; in PowerShell use
a backtick `` ` `` instead, or put the whole command on one line.)

Then run it:

```powershell
.\miniredis.exe
```

or, in an MSYS2/Git-Bash-style shell:

```bash
./miniredis.exe
```

### Option C — build object files separately (mirrors what the Makefile does)

```powershell
mkdir build
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\SkipList.cpp -o build\SkipList.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\Store.cpp -o build\Store.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\WAL.cpp -o build\WAL.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\ZSetStore.cpp -o build\ZSetStore.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\CommandProcessor.cpp -o build\CommandProcessor.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -c src\main.cpp -o build\main.o
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread -o miniredis.exe build\*.o
```

### Cleaning up (Windows)

The `Makefile`'s `clean` target uses `rm -rf`, which isn't available in a
plain `cmd`/PowerShell session (it is available in MSYS2/Git Bash). Without
`make`, clean up manually:

```powershell
Remove-Item -Recurse -Force build, miniredis.exe, miniredis.aof -ErrorAction SilentlyContinue
```

### Testing the build (Windows)

Once `miniredis.exe` is built, exercise it exactly like the demo script
below, either interactively or by piping a script of commands in:

```powershell
# interactive
.\miniredis.exe

# non-interactive: pipe a sequence of commands from a file
Get-Content commands.txt | .\miniredis.exe
```

```powershell
# commands.txt example
@"
SET name Alice
GET name
SET session tok123 EX 5
TTL session
ZADD leaderboard 100 alice
ZADD leaderboard 250 bob
ZADD leaderboard 175 carol
ZRANGE leaderboard 0 -1
ZRANK leaderboard bob
EXIT
"@ | Set-Content commands.txt
```

Persistence can be verified the same way as on Linux/macOS: run
`miniredis.exe`, `SET`/`ZADD` some data, `EXIT`, then run `miniredis.exe`
again — it should print `restored N key(s) from previous session` and
`GET`/`ZRANGE` should return the same data, since it's all replayed from
`miniredis.aof` (created next to the executable, in the directory you ran
it from).

## Command reference

```
Strings:      SET key value [EX seconds]
              GET key
              DEL key [key2 ...]
              EXISTS key
              EXPIRE key seconds
              TTL key
              KEYS
              DBSIZE

Sorted sets:  ZADD key score member
              ZREM key member
              ZSCORE key member
              ZRANGE key start stop        (supports negative indices, e.g. 0 -1 = all)
              ZRANGEBYSCORE key min max
              ZRANK key member
              ZCARD key

Other:        PING | HELP | EXIT / QUIT
```

## Demo script

```
miniredis> SET name Alice
OK
miniredis> GET name
"Alice"
miniredis> SET session tok123 EX 5
OK
miniredis> TTL session
(integer) 5
...wait 6 seconds...
miniredis> GET session
(nil)

miniredis> ZADD leaderboard 100 alice
(integer) 1
miniredis> ZADD leaderboard 250 bob
(integer) 1
miniredis> ZADD leaderboard 175 carol
(integer) 1
miniredis> ZRANGE leaderboard 0 -1
1) "alice" (score: 100)
2) "carol" (score: 175)
3) "bob" (score: 250)
miniredis> ZRANK leaderboard bob
(integer) 2

miniredis> EXIT
bye
```

Restart the process afterward and run `GET name` / `ZRANGE leaderboard 0 -1`
again — the data is still there, restored from `miniredis.aof`.

## Possible extensions (good "future work" talking points in an interview)

- Snapshotting: periodically compact the WAL into a binary snapshot (like
  Redis RDB) instead of replaying an ever-growing log.
- LRU/LFU eviction policy once a max-memory limit is hit.
- A simple TCP server front-end instead of stdin, so multiple clients can
  connect (this is the natural next step toward "real" Redis).
- Pub/Sub using a topic -> subscriber-queue map.
