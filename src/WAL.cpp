#include "WAL.h"
#include <fstream>
#include <sstream>

WAL::WAL(const std::string& path) : path_(path) {
    // Open in append mode; the file is created if it doesn't exist yet.
    out_.open(path_, std::ios::app);
}

WAL::~WAL() {
    if (out_.is_open()) out_.close();
}

void WAL::append(const std::string& command) {
    std::lock_guard<std::mutex> lock(mtx_);
    out_ << command << "\n";
    out_.flush(); // fsync-lite: durability over raw throughput, fine for a demo/prototype
    lineCount_++;
}

void WAL::replay(const std::function<void(const std::string&)>& executor) {
    std::ifstream in(path_);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        executor(line);
    }
}
