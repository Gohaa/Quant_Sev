#include "Rollover/RolloverEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Logger/Logger.hpp"

namespace quant_sev::bll {

namespace {

int parse_month(const std::string& delivery_month) {
    if (delivery_month.empty()) {
        return 0;
    }
    return std::atoi(delivery_month.c_str());
}

std::string format_month(int month) {
    if (month < 1) {
        month = 1;
    }
    if (month > 12) {
        month = 12;
    }
    std::ostringstream oss;
    if (month < 10) {
        oss << '0';
    }
    oss << month;
    return oss.str();
}

std::string format_year_suffix(int year, int digits) {
    const int mod = digits == 1 ? 10 : 100;
    const int value = year % mod;
    std::ostringstream oss;
    if (digits == 2 && value < 10) {
        oss << '0';
    }
    oss << value;
    return oss.str();
}

int decode_year(const ParsedInstrument& parsed, int year_digits, int decade_base) {
    const int suffix = std::atoi(parsed.year_suffix.c_str());
    if (year_digits == 1) {
        return decade_base + suffix;
    }
    if (year_digits == 2) {
        return suffix >= 90 ? 1900 + suffix : 2000 + suffix;
    }
    return decade_base + suffix;
}

std::filesystem::path daily_store_path(const std::filesystem::path& project_root) {
    return project_root / "data" / "rollover" / "daily_metrics.json";
}

nlohmann::json load_daily_store(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {{"version", 1}, {"records", nlohmann::json::array()}};
    }
    nlohmann::json doc;
    in >> doc;
    if (!doc.contains("records") || !doc["records"].is_array()) {
        doc["records"] = nlohmann::json::array();
    }
    if (!doc.contains("version")) {
        doc["version"] = 1;
    }
    return doc;
}

bool save_daily_store(const std::filesystem::path& path, const nlohmann::json& doc) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << doc.dump(2);
    return true;
}

std::string iso_timestamp_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

nlohmann::json build_candidate_row(const nlohmann::json& item, const std::string& current_id,
                                   const std::string& next_id, double current_metric, double next_metric,
                                   int consecutive_days, int policy_days, bool confirmed) {
    return {{"symbol_id", item.value("id", 0)},
            {"name", item.value("name", "")},
            {"product", item.value("product", "")},
            {"exchange", item.value("exchange", "")},
            {"current_instrument_id", current_id},
            {"suggested_instrument_id", next_id},
            {"current_metric", current_metric},
            {"next_metric", next_metric},
            {"consecutive_leading_days", consecutive_days},
            {"policy_consecutive_days", policy_days},
            {"confirmed", confirmed},
            {"note", confirmed ? ("连续 " + std::to_string(consecutive_days) + " 日日终 vol+OI 次月领先")
                               : ("日终连续 " + std::to_string(consecutive_days) + "/" +
                                  std::to_string(policy_days) + " 天；即时快照次月领先")}};
}

}  // namespace

bool RolloverEngine::initialize(const std::filesystem::path& project_root) {
    project_root_ = project_root;
    const auto rules_path = project_root / "config" / "Contract_Rules.json";
    if (!rules_.load(rules_path.string())) {
        quant_sev::core::Logger::instance().warn("RolloverEngine: Contract_Rules 加载失败");
        return false;
    }
    std::ifstream in(rules_path);
    if (!in) {
        return true;
    }
    nlohmann::json doc;
    in >> doc;
    if (doc.contains("rollover") && doc["rollover"].contains("rolloverPolicy")) {
        consecutive_days_ = doc["rollover"]["rolloverPolicy"].value("consecutiveDays", 2);
        if (doc["rollover"]["rolloverPolicy"].contains("trigger") &&
            doc["rollover"]["rolloverPolicy"]["trigger"].contains("consecutiveDays")) {
            consecutive_days_ = doc["rollover"]["rolloverPolicy"]["trigger"].value("consecutiveDays", consecutive_days_);
        }
    }

    const auto store_path = daily_store_path(project_root_);
    if (!std::filesystem::exists(store_path)) {
        save_daily_store(store_path, load_daily_store(store_path));
    }
    return true;
}

std::optional<std::string> RolloverEngine::next_contract(const std::string& instrument_id) const {
    const auto parsed = rules_.parse(instrument_id);
    if (!parsed) {
        return std::nullopt;
    }

    int year_digits = 2;
    int decade_base = 2020;
    if (parsed->exchange == "CZCE") {
        year_digits = 1;
        decade_base = 2020;
    }

    int year = decode_year(*parsed, year_digits, decade_base);
    int month = parse_month(parsed->delivery_month);
    if (month <= 0) {
        return std::nullopt;
    }

    month += 1;
    if (month > 12) {
        month = 1;
        year += 1;
    }

    const std::string prefix = parsed->product;
    const std::string year_suffix = format_year_suffix(year, year_digits);
    const std::string delivery_month = format_month(month);
    return prefix + year_suffix + delivery_month;
}

double RolloverEngine::quote_metric(const nlohmann::json& quote_row) {
    const double volume = quote_row.value("volume", 0.0);
    const double open_interest = quote_row.value("open_interest", 0.0);
    return volume + open_interest;
}

std::optional<nlohmann::json> RolloverEngine::find_quote(const nlohmann::json& quote_board,
                                                         const std::string& instrument_id) {
    auto extract = [](const nlohmann::json& row) -> nlohmann::json {
        if (row.contains("quote") && row["quote"].is_object()) {
            return row["quote"];
        }
        return row;
    };

    if (quote_board.is_array()) {
        for (const auto& row : quote_board) {
            if (row.value("instrument_id", "") == instrument_id) {
                return extract(row);
            }
        }
        return std::nullopt;
    }
    if (quote_board.contains("quotes") && quote_board["quotes"].is_array()) {
        for (const auto& row : quote_board["quotes"]) {
            if (row.value("instrument_id", "") == instrument_id) {
                return extract(row);
            }
        }
    }
    if (quote_board.contains(instrument_id)) {
        return quote_board[instrument_id];
    }
    return std::nullopt;
}

int RolloverEngine::count_consecutive_leading_days(const nlohmann::json& store, int symbol_id) {
    if (!store.contains("records") || !store["records"].is_array()) {
        return 0;
    }

    std::vector<nlohmann::json> rows;
    for (const auto& row : store["records"]) {
        if (row.value("symbol_id", 0) == symbol_id) {
            rows.push_back(row);
        }
    }
    if (rows.empty()) {
        return 0;
    }

    std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("trading_day", "") > b.value("trading_day", "");
    });

    int count = 0;
    for (const auto& row : rows) {
        if (!row.value("next_leading", false)) {
            break;
        }
        count += 1;
    }
    return count;
}

nlohmann::json RolloverEngine::record_daily_snapshot(const std::filesystem::path& project_root,
                                                       const std::string& trading_day,
                                                       const nlohmann::json& symbol_list,
                                                       const nlohmann::json& quote_board) const {
    if (trading_day.empty()) {
        return {{"ok", false}, {"error", "trading_day 为空"}};
    }
    if (!symbol_list.contains("symbols") || !symbol_list["symbols"].is_array()) {
        return {{"ok", false}, {"error", "Symbol_list 无 symbols 数组"}};
    }

    const auto store_path = daily_store_path(project_root);
    nlohmann::json store = load_daily_store(store_path);
    auto& records = store["records"];
    int written = 0;
    int skipped = 0;

    for (const auto& item : symbol_list["symbols"]) {
        const int symbol_id = item.value("id", 0);
        const std::string current_id = item.value("instrument_id", "");
        if (symbol_id <= 0 || current_id.empty()) {
            skipped += 1;
            continue;
        }
        const auto next_id = next_contract(current_id);
        if (!next_id) {
            skipped += 1;
            continue;
        }
        const auto current_quote = find_quote(quote_board, current_id);
        const auto next_quote = find_quote(quote_board, *next_id);
        if (!current_quote || !next_quote) {
            skipped += 1;
            continue;
        }

        const double current_metric = quote_metric(*current_quote);
        const double next_metric = quote_metric(*next_quote);
        const bool next_leading = next_metric > current_metric;

        nlohmann::json record = {{"trading_day", trading_day},
                                 {"symbol_id", symbol_id},
                                 {"product", item.value("product", "")},
                                 {"current_instrument_id", current_id},
                                 {"next_instrument_id", *next_id},
                                 {"current_metric", current_metric},
                                 {"next_metric", next_metric},
                                 {"current_volume", current_quote->value("volume", 0.0)},
                                 {"next_volume", next_quote->value("volume", 0.0)},
                                 {"current_open_interest", current_quote->value("open_interest", 0.0)},
                                 {"next_open_interest", next_quote->value("open_interest", 0.0)},
                                 {"next_leading", next_leading},
                                 {"recorded_at", iso_timestamp_now()}};

        bool replaced = false;
        for (auto& existing : records) {
            if (existing.value("trading_day", "") == trading_day && existing.value("symbol_id", 0) == symbol_id) {
                existing = record;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            records.push_back(record);
        }
        written += 1;
    }

    store["updated_at"] = iso_timestamp_now();
    if (!save_daily_store(store_path, store)) {
        return {{"ok", false}, {"error", "daily_metrics.json 写入失败"}};
    }

    quant_sev::core::Logger::instance().info("换月日终快照: day=" + trading_day + " written=" +
                                             std::to_string(written) + " skipped=" + std::to_string(skipped));

    return {{"ok", true},
            {"trading_day", trading_day},
            {"written", written},
            {"skipped", skipped},
            {"store_path", store_path.string()}};
}

nlohmann::json RolloverEngine::daily_view(const std::filesystem::path& project_root) const {
    const auto store_path = daily_store_path(project_root);
    const auto store = load_daily_store(store_path);
    return {{"store_path", store_path.string()},
            {"policy_consecutive_days", consecutive_days_},
            {"updated_at", store.value("updated_at", "")},
            {"records", store.value("records", nlohmann::json::array())},
            {"count", store.contains("records") && store["records"].is_array() ? store["records"].size() : 0}};
}

nlohmann::json RolloverEngine::suggest(const nlohmann::json& symbol_list, const nlohmann::json& quote_board) const {
    nlohmann::json confirmed = nlohmann::json::array();
    nlohmann::json intraday = nlohmann::json::array();
    if (!symbol_list.contains("symbols") || !symbol_list["symbols"].is_array()) {
        return {{"confirmed_candidates", confirmed},
                {"intraday_candidates", intraday},
                {"candidates", confirmed},
                {"count", 0},
                {"note", "Symbol_list 无 symbols 数组"}};
    }

    const auto store = load_daily_store(daily_store_path(project_root_));

    for (const auto& item : symbol_list["symbols"]) {
        const int symbol_id = item.value("id", 0);
        const std::string current_id = item.value("instrument_id", "");
        if (current_id.empty()) {
            continue;
        }
        const auto next_id = next_contract(current_id);
        if (!next_id) {
            continue;
        }

        const auto current_quote = find_quote(quote_board, current_id);
        const auto next_quote = find_quote(quote_board, *next_id);
        if (!current_quote || !next_quote) {
            continue;
        }

        const double current_metric = quote_metric(*current_quote);
        const double next_metric = quote_metric(*next_quote);
        if (next_metric <= current_metric) {
            continue;
        }

        const int leading_days = count_consecutive_leading_days(store, symbol_id);
        const bool is_confirmed = leading_days >= consecutive_days_;
        const auto row =
            build_candidate_row(item, current_id, *next_id, current_metric, next_metric, leading_days,
                                consecutive_days_, is_confirmed);
        if (is_confirmed) {
            confirmed.push_back(row);
        } else {
            intraday.push_back(row);
        }
    }

    return {{"confirmed_candidates", confirmed},
            {"intraday_candidates", intraday},
            {"candidates", confirmed},
            {"confirmed_count", confirmed.size()},
            {"intraday_count", intraday.size()},
            {"count", confirmed.size()},
            {"policy_consecutive_days", consecutive_days_},
            {"note", "confirmed=日终连续 " + std::to_string(consecutive_days_) +
                         " 天次月 vol+OI 领先；intraday=仅即时快照领先"}};
}

nlohmann::json RolloverEngine::apply(const std::filesystem::path& project_root,
                                     const nlohmann::json& payload) const {
    const int symbol_id = payload.value("symbol_id", payload.value("id", 0));
    const std::string new_instrument_id = payload.value("new_instrument_id", payload.value("instrument_id", ""));
    if (symbol_id <= 0 || new_instrument_id.empty()) {
        return {{"ok", false}, {"error", "需要 symbol_id 与 new_instrument_id"}};
    }

    const auto parsed = rules_.parse(new_instrument_id);
    if (!parsed) {
        return {{"ok", false}, {"error", "无法解析 new_instrument_id: " + new_instrument_id}};
    }

    const auto symbol_path = project_root / "config" / "Symbol_list.json";
    std::ifstream in(symbol_path);
    if (!in) {
        return {{"ok", false}, {"error", "无法读取 Symbol_list.json"}};
    }
    nlohmann::json doc;
    in >> doc;
    if (!doc.contains("symbols") || !doc["symbols"].is_array()) {
        return {{"ok", false}, {"error", "Symbol_list.json 格式无效"}};
    }

    std::string old_instrument_id;
    bool found = false;
    for (auto& item : doc["symbols"]) {
        if (item.value("id", 0) != symbol_id) {
            continue;
        }
        old_instrument_id = item.value("instrument_id", "");
        item["instrument_id"] = new_instrument_id;
        item["exchange"] = parsed->exchange;
        item["product"] = parsed->product;
        item["year_suffix"] = parsed->year_suffix;
        item["delivery_month"] = parsed->delivery_month;
        item["month_slot"] = parsed->month_slot;
        const std::string base = "data/" + parsed->exchange + "/" + parsed->product + "/" + parsed->month_slot;
        item["storage_bar_path"] = base + "/m1.csv";
        item["storage_tick_path"] = base + "/tick.csv";
        found = true;
        break;
    }

    if (!found) {
        return {{"ok", false}, {"error", "未找到 symbol_id=" + std::to_string(symbol_id)}};
    }

    std::ofstream out(symbol_path);
    if (!out) {
        return {{"ok", false}, {"error", "Symbol_list.json 写入失败"}};
    }
    out << doc.dump(2);

    quant_sev::core::Logger::instance().info("换月已更新 Symbol_list: id=" + std::to_string(symbol_id) + " " +
                                             old_instrument_id + " -> " + new_instrument_id);

    return {{"ok", true},
            {"symbol_id", symbol_id},
            {"old_instrument_id", old_instrument_id},
            {"new_instrument_id", new_instrument_id},
            {"resubscribe",
             {{"old", old_instrument_id},
              {"new", new_instrument_id},
              {"hint", "POST /api/unsubscribe/symbol + /api/load/symbol 完成行情切换"}}}};
}

}  // namespace quant_sev::bll
