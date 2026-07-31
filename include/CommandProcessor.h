#pragma once
#include "Store.h"
#include "ZSetStore.h"
#include "WAL.h"
#include <string>
#include <vector>

class CommandProcessor {
public:
    CommandProcessor(Store& store, ZSetStore& zstore, WAL& wal);

    // Parses and runs one line of input. If logToWAL is true and the command
    // is a write, it's appended to the WAL after successful execution.
    // Set logToWAL=false when replaying the WAL itself, to avoid duplicating entries.
    std::string execute(const std::string& line, bool logToWAL = true);

private:
    Store& store_;
    ZSetStore& zstore_;
    WAL& wal_;

    static std::vector<std::string> tokenize(const std::string& line);
    static bool isWriteCommand(const std::string& cmd);
    static std::string toUpper(std::string s);
};
