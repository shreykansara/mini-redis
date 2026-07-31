#include "Store.h"
#include "ZSetStore.h"
#include "WAL.h"
#include "CommandProcessor.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string walPath = (argc > 1) ? argv[1] : "miniredis.aof";

    Store store;
    ZSetStore zstore;
    WAL wal(walPath);
    CommandProcessor processor(store, zstore, wal);

    // Rebuild in-memory state from whatever was persisted last time.
    // logToWAL=false so replaying doesn't re-append what we just read.
    size_t before = store.size();
    wal.replay([&](const std::string& cmd) { processor.execute(cmd, false); });
    size_t after = store.size();

    std::cout << "mini-redis :: terminal key-value store with TTL + sorted sets\n";
    std::cout << "persistence file: " << walPath << "\n";
    if (after > before) {
        std::cout << "restored " << after << " key(s) from previous session\n";
    }
    std::cout << "type HELP for commands, EXIT to quit\n\n";

    std::string line;
    while (true) {
        std::cout << "miniredis> ";
        if (!std::getline(std::cin, line)) break; // EOF (e.g. piped input, Ctrl+D)

        // trim leading/trailing whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        std::string upper = line;
        for (auto& c : upper) c = std::toupper(static_cast<unsigned char>(c));
        if (upper == "EXIT" || upper == "QUIT") break;

        std::string reply = processor.execute(line, true);
        if (!reply.empty()) std::cout << reply << "\n";
    }

    std::cout << "bye\n";
    return 0;
}
