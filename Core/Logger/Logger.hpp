#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace quant_sev::core {

enum class LogLevel { Info, Warn, Error, Ctp };

class Logger {
public:
    static Logger& instance();

    void set_persist_dir(const std::filesystem::path& dir);

    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
    void ctp(const std::string& message);

    std::vector<std::string> snapshot() const;
    void clear();

private:
    Logger() = default;
    void append(LogLevel level, const std::string& message);
    void persist_line(const std::string& line);
    std::filesystem::path current_log_file() const;

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::filesystem::path persist_dir_;
    std::string persist_day_;
    static constexpr std::size_t kMaxLines = 5000;
};

}  // namespace quant_sev::core
