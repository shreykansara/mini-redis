#include "CommandProcessor.h"
#include <sstream>
#include <algorithm>
#include <cctype>

CommandProcessor::CommandProcessor(Store& store, ZSetStore& zstore, WAL& wal)
    : store_(store), zstore_(zstore), wal_(wal) {}

std::string CommandProcessor::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

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

bool CommandProcessor::isWriteCommand(const std::string& cmd) {
    static const std::vector<std::string> writes = {
        "SET", "DEL", "EXPIRE", "ZADD", "ZREM"
    };
    return std::find(writes.begin(), writes.end(), cmd) != writes.end();
}

std::string CommandProcessor::execute(const std::string& line, bool logToWAL) {
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) return "";

    std::string cmd = toUpper(tokens[0]);
    std::ostringstream reply;

    auto wrongArgs = [&](const std::string& c) {
        return "(error) ERR wrong number of arguments for '" + c + "' command";
    };

    bool ok = true; // whether this write should be logged to the WAL

    if (cmd == "PING") {
        reply << "PONG";
        ok = false;
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
    } else if (cmd == "GET") {
        if (tokens.size() != 2) return wrongArgs("GET");
        auto v = store_.get(tokens[1]);
        ok = false;
        if (v.has_value()) reply << "\"" << *v << "\"";
        else reply << "(nil)";
    } else if (cmd == "DEL") {
        if (tokens.size() < 2) return wrongArgs("DEL");
        long count = 0;
        for (size_t i = 1; i < tokens.size(); i++) if (store_.del(tokens[i])) count++;
        reply << "(integer) " << count;
        if (count == 0) ok = false; // nothing actually changed, no need to log
    } else if (cmd == "EXISTS") {
        if (tokens.size() != 2) return wrongArgs("EXISTS");
        reply << "(integer) " << (store_.exists(tokens[1]) ? 1 : 0);
        ok = false;
    } else if (cmd == "EXPIRE") {
        if (tokens.size() != 3) return wrongArgs("EXPIRE");
        long seconds;
        try { seconds = std::stol(tokens[2]); } catch (...) {
            return "(error) ERR value is not an integer or out of range";
        }
        bool did = store_.expire(tokens[1], seconds);
        reply << "(integer) " << (did ? 1 : 0);
        if (!did) ok = false;
    } else if (cmd == "TTL") {
        if (tokens.size() != 2) return wrongArgs("TTL");
        reply << "(integer) " << store_.ttl(tokens[1]);
        ok = false;
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
    } else if (cmd == "DBSIZE") {
        reply << "(integer) " << store_.size();
        ok = false;
    } else if (cmd == "ZADD") {
        // ZADD key score member
        if (tokens.size() != 4) return wrongArgs("ZADD");
        double score;
        try { score = std::stod(tokens[2]); } catch (...) {
            return "(error) ERR value is not a valid float";
        }
        bool isNew = zstore_.zadd(tokens[1], score, tokens[3]);
        reply << "(integer) " << (isNew ? 1 : 0);
    } else if (cmd == "ZREM") {
        if (tokens.size() != 3) return wrongArgs("ZREM");
        bool removed = zstore_.zrem(tokens[1], tokens[2]);
        reply << "(integer) " << (removed ? 1 : 0);
        if (!removed) ok = false;
    } else if (cmd == "ZSCORE") {
        if (tokens.size() != 3) return wrongArgs("ZSCORE");
        auto s = zstore_.zscore(tokens[1], tokens[2]);
        ok = false;
        if (s.has_value()) reply << "\"" << *s << "\"";
        else reply << "(nil)";
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
    } else if (cmd == "ZRANK") {
        if (tokens.size() != 3) return wrongArgs("ZRANK");
        long r = zstore_.zrank(tokens[1], tokens[2]);
        ok = false;
        if (r < 0) reply << "(nil)";
        else reply << "(integer) " << r;
    } else if (cmd == "ZCARD") {
        if (tokens.size() != 2) return wrongArgs("ZCARD");
        reply << "(integer) " << zstore_.zcard(tokens[1]);
        ok = false;
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
    } else {
        return "(error) ERR unknown command '" + tokens[0] + "'";
    }

    if (ok && logToWAL && isWriteCommand(cmd)) {
        wal_.append(line);
    }

    return reply.str();
}
