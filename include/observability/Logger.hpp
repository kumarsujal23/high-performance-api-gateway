#pragma once
#include <string>
#include <sstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iostream>

namespace gw::observability {

// Structured (JSON-lines) logger. One line per event so logs are easy to
// grep/pipe into jq during a demo. Thread-safe via a single mutex; logging is
// not on the byte-copy hot path so contention here is acceptable.
enum class Level { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void setLevel(Level lvl) { minLevel_ = lvl; }

    // fields: pre-formatted "key":"value" or "key":value fragments, comma separated.
    void log(Level lvl, const std::string& msg, const std::string& fields = "") {
        if (lvl < minLevel_) return;
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << "{\"ts\":\"" << isoTime(t, ms.count()) << "\","
            << "\"level\":\"" << levelName(lvl) << "\","
            << "\"msg\":\"" << escape(msg) << "\"";
        if (!fields.empty()) oss << "," << fields;
        oss << "}";

        std::lock_guard<std::mutex> lock(mu_);
        std::cout << oss.str() << std::endl;
    }

    void debug(const std::string& msg, const std::string& fields = "") { log(Level::DEBUG, msg, fields); }
    void info(const std::string& msg, const std::string& fields = "")  { log(Level::INFO, msg, fields); }
    void warn(const std::string& msg, const std::string& fields = "")  { log(Level::WARN, msg, fields); }
    void error(const std::string& msg, const std::string& fields = "") { log(Level::ERROR, msg, fields); }

private:
    Logger() = default;
    std::mutex mu_;
    Level minLevel_ = Level::INFO;

    static std::string levelName(Level lvl) {
        switch (lvl) {
            case Level::DEBUG: return "DEBUG";
            case Level::INFO: return "INFO";
            case Level::WARN: return "WARN";
            case Level::ERROR: return "ERROR";
        }
        return "?";
    }

    static std::string isoTime(std::time_t t, long ms) {
        char buf[64];
        std::tm tmv{};
        gmtime_r(&t, &tmv);
        // Values are all range-bounded (tm_year+1900 fits comfortably in 64
        // bytes), so the truncation warning here is a false positive from
        // gcc's conservative snprintf width analysis; the explicit cast
        // narrows the estimated range it considers.
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms));
        return buf;
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }
};

} // namespace gw::observability
