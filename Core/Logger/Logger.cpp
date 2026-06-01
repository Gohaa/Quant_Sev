#include "Logger/Logger.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace quant_sev::core {

namespace fs = std::filesystem;

namespace {

std::string now_string() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;

    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

std::string today_string() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Ctp:
            return "CTP";
    }
    return "INFO";
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_persist_dir(const fs::path& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    persist_dir_ = dir;
    persist_day_.clear();
    if (!persist_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(persist_dir_, ec);
    }
}

fs::path Logger::current_log_file() const {
    if (persist_dir_.empty()) {
        return {};
    }
    return persist_dir_ / ("quant_sev_" + today_string() + ".log");
}

void Logger::persist_line(const std::string& line) {
    if (persist_dir_.empty()) {
        return;
    }
    const std::string day = today_string();
    if (persist_day_ != day) {
        persist_day_ = day;
    }
    const fs::path file = current_log_file();
    std::ofstream out(file, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << line << '\n';
}

void Logger::info(const std::string& message) { append(LogLevel::Info, message); }
void Logger::warn(const std::string& message) { append(LogLevel::Warn, message); }
void Logger::error(const std::string& message) { append(LogLevel::Error, message); }
void Logger::ctp(const std::string& message) { append(LogLevel::Ctp, message); }

void Logger::append(LogLevel level, const std::string& message) {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        oss << '[' << now_string() << "] [" << level_tag(level) << "] " << message;
        line = oss.str();
        lines_.push_back(line);
        if (lines_.size() > kMaxLines) {
            lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - kMaxLines));
        }
    }
    persist_line(line);
}

std::vector<std::string> Logger::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}

void Logger::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}

}  // namespace quant_sev::core
