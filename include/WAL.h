#pragma once
// WAL (Write-Ahead Log): every successful write command is appended to a
// plaintext file *before* we consider it durable. On startup, we replay the
// file line-by-line through the command processor to rebuild state - the
// same idea as Redis's AOF (Append-Only File) persistence mode.

#include <string>
#include <fstream>
#include <functional>
#include <mutex>

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    void append(const std::string& command);

    // Reads the log file (if it exists) and calls executor(line) for each
    // command, in order, to rebuild in-memory state.
    void replay(const std::function<void(const std::string&)>& executor);

    size_t lineCount() const { return lineCount_; }

private:
    std::string path_;
    std::ofstream out_;
    std::mutex mtx_;
    size_t lineCount_ = 0;
};
