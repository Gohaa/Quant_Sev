#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quant_sev::bll {

struct BarRecord {
    std::string date;
    std::string time;
    double open{0};
    double high{0};
    double low{0};
    double close{0};
    long long volume{0};
    double turnover{0};
    long long open_interest{0};
};

struct BarQuery {
    std::string instrument_id;
    std::string period{"m1"};
    int limit{500};
    bool prefer_historical{true};
};

struct BarFileInfo {
    std::string path;
    bool exists{false};
    int bar_count{0};
    std::optional<BarRecord> last;
};

struct DataFileEntry {
    std::string file_type;
    std::string period;
    std::string exchange;
    std::string product;
    std::string month_slot;
    std::string instrument_id;
    std::string name;
    std::string path;
    int row_count{-1};
    std::uintmax_t size_bytes{0};
    std::string start_time;
    std::string end_time;
    bool exists{false};
};

struct TickRecord {
    std::string trading_day;
    std::string update_time;
    int update_millisec{0};
    double last_price{0};
    long long volume{0};
    double turnover{0};
    long long open_interest{0};
};

struct TickQuery {
    std::string instrument_id;
    int limit{200000};
};

struct DataFileMutationResult {
    bool ok{false};
    std::string message;
    std::string path;
    int row_count{0};
};

}  // namespace quant_sev::bll
