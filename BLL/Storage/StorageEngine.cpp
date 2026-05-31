#include "Storage/StorageEngine.hpp"

#include <deque>
#include <fstream>
#include <sstream>

#include <algorithm>
#include <array>
#include <sstream>

#include "Logger/Logger.hpp"

namespace quant_sev::bll {

namespace fs = std::filesystem;

void StorageEngine::set_project_root(const fs::path& root) { root_ = root; }

bool StorageEngine::initialize() {
    const auto rules_path = root_ / "config" / "Contract_Rules.json";
    if (!rules_.load(rules_path.string())) {
        quant_sev::core::Logger::instance().warn("Contract_Rules.json 加载失败: " + rules_path.string());
        return false;
    }
    quant_sev::core::Logger::instance().info("Storage 初始化，根目录: " + root_.string());
    return true;
}

fs::path StorageEngine::slot_dir_path(const ParsedInstrument& instrument) const {
    return root_ / "data" / instrument.exchange / instrument.product / instrument.month_slot;
}

fs::path StorageEngine::historical_bar_path(const ParsedInstrument& instrument,
                                            const std::string& period) const {
    return slot_dir_path(instrument) / (period + ".csv");
}

fs::path StorageEngine::tick_path(const ParsedInstrument& instrument) const {
    return slot_dir_path(instrument) / "tick.csv";
}

void StorageEngine::ensure_header(std::ofstream& out, const std::string& header, const fs::path& path) {
    if (!fs::exists(path) || fs::file_size(path) == 0) {
        out << header << '\n';
    }
}

void StorageEngine::write_tick(const nlohmann::json& tick) {
    const std::string instrument_id = tick.value("instrument_id", "");
    const auto parsed = rules_.parse(instrument_id);
    if (!parsed) {
        return;
    }

    const std::string trading_day = tick.value("trading_day", "");
    if (trading_day.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = tick_path(*parsed);
    fs::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::app);
    if (!out) {
        return;
    }
    const std::string header =
        "trading_day,update_time,update_millisec,last_price,volume,turnover,open_interest,bid_price1,bid_volume1,ask_price1,ask_volume1";
    ensure_header(out, header, path);

    out << trading_day << ','
        << tick.value("update_time", "") << ','
        << tick.value("update_millisec", 0) << ','
        << tick.value("last_price", 0.0) << ','
        << tick.value("volume", 0) << ','
        << tick.value("turnover", 0.0) << ','
        << tick.value("open_interest", 0) << ','
        << tick.value("bid_price1", 0.0) << ','
        << tick.value("bid_volume1", 0) << ','
        << tick.value("ask_price1", 0.0) << ','
        << tick.value("ask_volume1", 0) << '\n';
}

void StorageEngine::append_bar(const ParsedInstrument& instrument, const std::string& period,
                               const BarRecord& bar) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto bar_path = historical_bar_path(instrument, period);
    fs::create_directories(bar_path.parent_path());

    const std::string header = "date,time,open,high,low,close,volume,turnover,open_interest";
    std::ofstream out(bar_path, std::ios::app);
    if (!out) {
        return;
    }
    ensure_header(out, header, bar_path);
    out << bar.date << ',' << bar.time << ',' << bar.open << ',' << bar.high << ',' << bar.low << ','
        << bar.close << ',' << bar.volume << ',' << bar.turnover << ',' << bar.open_interest << '\n';
}

std::vector<BarRecord> StorageEngine::read_bar_csv(const fs::path& path, int limit) {
    std::vector<BarRecord> rows;
    if (!fs::exists(path)) {
        return rows;
    }
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) {
        return rows;
    }
    std::deque<std::string> tail;
    const std::size_t keep = limit > 0 ? static_cast<std::size_t>(limit) : 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        tail.push_back(line);
        if (keep > 0 && tail.size() > keep) {
            tail.pop_front();
        }
    }
    for (const auto& row_line : tail) {
        std::stringstream ss(row_line);
        BarRecord row;
        std::string token;
        std::getline(ss, row.date, ',');
        std::getline(ss, row.time, ',');
        std::getline(ss, token, ',');
        row.open = std::stod(token);
        std::getline(ss, token, ',');
        row.high = std::stod(token);
        std::getline(ss, token, ',');
        row.low = std::stod(token);
        std::getline(ss, token, ',');
        row.close = std::stod(token);
        std::getline(ss, token, ',');
        row.volume = std::stoll(token);
        std::getline(ss, token, ',');
        row.turnover = std::stod(token);
        std::getline(ss, token, ',');
        row.open_interest = std::stoll(token);
        rows.push_back(row);
    }
    return rows;
}

std::optional<ParsedInstrument> StorageEngine::parse_instrument(const std::string& instrument_id) const {
    return rules_.parse(instrument_id);
}

fs::path StorageEngine::config_path(const std::string& filename) const {
    return root_ / "config" / filename;
}

std::vector<BarRecord> StorageEngine::query_bars(const BarQuery& query) const {
    const auto parsed = rules_.parse(query.instrument_id);
    if (!parsed) {
        return {};
    }
    const auto bar_file = historical_bar_path(*parsed, query.period);
    return read_bar_csv(bar_file, query.limit);
}

BarFileInfo StorageEngine::bar_file_info(const std::string& instrument_id,
                                         const std::string& period) const {
    BarFileInfo info;
    const auto parsed = rules_.parse(instrument_id);
    if (!parsed) {
        return info;
    }
    const auto bar_file = historical_bar_path(*parsed, period);
    info.path = bar_file.string();
    info.exists = fs::exists(bar_file);
    if (!info.exists) {
        return info;
    }
    std::ifstream in(bar_file);
    std::string line;
    if (!std::getline(in, line)) {
        return info;
    }
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        info.bar_count += 1;
    }
    const auto tail = read_bar_csv(bar_file, 1);
    if (!tail.empty()) {
        info.last = tail.back();
    }
    return info;
}

namespace {

int count_csv_data_rows(const fs::path& path) {
    if (!fs::exists(path)) {
        return 0;
    }
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) {
        return 0;
    }
    int count = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            count += 1;
        }
    }
    return count;
}

std::string bar_line_time_label(const std::string& line) {
    const auto c1 = line.find(',');
    if (c1 == std::string::npos) {
        return line;
    }
    const auto c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) {
        return line.substr(0, c1);
    }
    return line.substr(0, c2);
}

std::string tick_line_time_label(const std::string& line) {
    const auto c1 = line.find(',');
    if (c1 == std::string::npos) {
        return line;
    }
    const auto c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) {
        return line.substr(0, c1);
    }
    return line.substr(0, c2);
}

std::string read_last_non_empty_line(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return {};
    }
    const std::streamoff max_scan = std::min(size, static_cast<std::streamoff>(262144));
    in.seekg(-max_scan, std::ios::end);
    std::string chunk(static_cast<std::size_t>(max_scan), '\0');
    in.read(chunk.data(), max_scan);
    if (!in && !in.eof()) {
        return {};
    }
    const auto first_nl = chunk.find('\n');
    if (first_nl != std::string::npos) {
        chunk = chunk.substr(first_nl + 1);
    }
    while (!chunk.empty() && (chunk.back() == '\n' || chunk.back() == '\r')) {
        chunk.pop_back();
    }
    if (chunk.empty()) {
        return {};
    }
    const auto last_nl = chunk.rfind('\n');
    std::string line = last_nl == std::string::npos ? chunk : chunk.substr(last_nl + 1);
    while (!line.empty() && (line.back() == '\r')) {
        line.pop_back();
    }
    if (line.rfind("date,", 0) == 0 || line.rfind("trading_day,", 0) == 0) {
        return {};
    }
    return line;
}

void fill_csv_time_range(const fs::path& path, const std::string& file_type, std::string& start_time,
                         std::string& end_time) {
    if (!fs::exists(path)) {
        return;
    }
    std::ifstream in(path);
    std::string header;
    if (!std::getline(in, header)) {
        return;
    }
    std::string first;
    if (std::getline(in, first) && !first.empty()) {
        start_time = file_type == "tick" ? tick_line_time_label(first) : bar_line_time_label(first);
    }
    const std::string last = read_last_non_empty_line(path);
    if (!last.empty()) {
        end_time = file_type == "tick" ? tick_line_time_label(last) : bar_line_time_label(last);
    }
}

int quick_row_count(const fs::path& path, std::uintmax_t size_bytes) {
    constexpr std::uintmax_t kMaxCountBytes = 512 * 1024;
    if (size_bytes > kMaxCountBytes) {
        return -1;
    }
    return count_csv_data_rows(path);
}

}  // namespace

std::vector<DataFileEntry> StorageEngine::list_data_inventory() const {
    const nlohmann::json catalog = scan_data_catalog();
    std::vector<DataFileEntry> rows;
    if (!catalog.contains("files") || !catalog["files"].is_array()) {
        return rows;
    }
    rows.reserve(catalog["files"].size());
    for (const auto& item : catalog["files"]) {
        DataFileEntry row;
        row.file_type = item.value("file_type", "");
        row.period = item.value("period", "");
        row.exchange = item.value("exchange", "");
        row.product = item.value("product", "");
        row.month_slot = item.value("month_slot", "");
        row.instrument_id = item.value("instrument_id", "");
        row.name = item.value("name", "");
        row.path = item.value("path", "");
        row.row_count = item.value("row_count", -1);
        row.size_bytes = item.value("size_bytes", static_cast<std::uintmax_t>(0));
        row.start_time = item.value("start_time", "");
        row.end_time = item.value("end_time", "");
        row.exists = item.value("exists", true);
        rows.push_back(std::move(row));
    }
    return rows;
}

nlohmann::json StorageEngine::scan_data_catalog() const {
    nlohmann::json files = nlohmann::json::array();
    nlohmann::json tree = nlohmann::json::object();
    const fs::path data_root = root_ / "data";
    if (!fs::exists(data_root)) {
        return {{"files", files}, {"tree", tree}, {"count", 0}, {"layout", "data/{exchange}/{product}/{month_slot}/{period}.csv"}};
    }

    int index = 0;
    for (const auto& ex_entry : fs::directory_iterator(data_root)) {
        if (!ex_entry.is_directory()) {
            continue;
        }
        const std::string exchange = ex_entry.path().filename().string();
        for (const auto& prod_entry : fs::directory_iterator(ex_entry.path())) {
            if (!prod_entry.is_directory()) {
                continue;
            }
            const std::string product = prod_entry.path().filename().string();
            for (const auto& slot_entry : fs::directory_iterator(prod_entry.path())) {
                if (!slot_entry.is_directory()) {
                    continue;
                }
                const std::string month_slot = slot_entry.path().filename().string();
                for (const auto& file_entry : fs::directory_iterator(slot_entry.path())) {
                    if (!file_entry.is_regular_file()) {
                        continue;
                    }
                    const std::string fname = file_entry.path().filename().string();
                    if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".csv") {
                        continue;
                    }

                    std::string file_type;
                    std::string period;
                    if (fname == "tick.csv") {
                        file_type = "tick";
                        period = "tick";
                    } else {
                        file_type = "bar";
                        period = fname.substr(0, fname.size() - 4);
                    }

                    std::uintmax_t size_bytes = 0;
                    std::error_code ec;
                    size_bytes = fs::file_size(file_entry.path(), ec);

                    nlohmann::json item = {{"index", index},
                                           {"file_type", file_type},
                                           {"period", period},
                                           {"exchange", exchange},
                                           {"product", product},
                                           {"month_slot", month_slot},
                                           {"path", file_entry.path().string()},
                                           {"size_bytes", size_bytes},
                                           {"row_count", -1},
                                           {"start_time", ""},
                                           {"end_time", ""},
                                           {"exists", true},
                                           {"instrument_id", ""},
                                           {"name", ""}};
                    files.push_back(item);

                    if (!tree.contains(period)) {
                        tree[period] = nlohmann::json::object();
                    }
                    if (!tree[period].contains(exchange)) {
                        tree[period][exchange] = nlohmann::json::object();
                    }
                    if (!tree[period][exchange].contains(product)) {
                        tree[period][exchange][product] = nlohmann::json::array();
                    }
                    tree[period][exchange][product].push_back(item);
                    index += 1;
                }
            }
        }
    }

    return {{"files", files},
            {"tree", tree},
            {"count", index},
            {"layout", "data/{exchange}/{product}/{month_slot}/{period}.csv"}};
}

std::vector<TickRecord> StorageEngine::read_tick_csv(const fs::path& path, int limit) {
    std::vector<TickRecord> rows;
    if (!fs::exists(path)) {
        return rows;
    }
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) {
        return rows;
    }
    std::deque<std::string> tail;
    const std::size_t keep = limit > 0 ? static_cast<std::size_t>(limit) : 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        tail.push_back(line);
        if (keep > 0 && tail.size() > keep) {
            tail.pop_front();
        }
    }
    for (const auto& row_line : tail) {
        std::stringstream ss(row_line);
        TickRecord row;
        std::string token;
        std::getline(ss, row.trading_day, ',');
        std::getline(ss, row.update_time, ',');
        std::getline(ss, token, ',');
        row.update_millisec = std::stoi(token);
        std::getline(ss, token, ',');
        row.last_price = std::stod(token);
        std::getline(ss, token, ',');
        row.volume = std::stoll(token);
        std::getline(ss, token, ',');
        row.turnover = std::stod(token);
        std::getline(ss, token, ',');
        row.open_interest = std::stoll(token);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<TickRecord> StorageEngine::query_ticks(const TickQuery& query) const {
    const auto parsed = rules_.parse(query.instrument_id);
    if (!parsed) {
        return {};
    }
    return read_tick_csv(tick_path(*parsed), query.limit);
}

bool StorageEngine::is_safe_path_component(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    if (value.find("..") != std::string::npos || value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos) {
        return false;
    }
    return true;
}

std::string StorageEngine::expected_csv_header(const std::string& period) {
    if (period == "tick") {
        return "trading_day,update_time,update_millisec,last_price,volume,turnover,open_interest,bid_price1,bid_volume1,ask_price1,ask_volume1";
    }
    return "date,time,open,high,low,close,volume,turnover,open_interest";
}

fs::path StorageEngine::resolve_data_file_path(const fs::path& root, const std::string& exchange,
                                               const std::string& product, const std::string& month_slot,
                                               const std::string& period) {
    const std::string filename = period == "tick" ? "tick.csv" : (period + ".csv");
    return root / "data" / exchange / product / month_slot / filename;
}

bool StorageEngine::is_safe_data_path(const fs::path& path) const {
    const fs::path data_root = fs::absolute(root_ / "data");
    fs::path target = fs::absolute(path);
    std::string root_str = data_root.generic_string();
    std::string target_str = target.generic_string();
    while (!root_str.empty() && (root_str.back() == '/' || root_str.back() == '\\')) {
        root_str.pop_back();
    }
    while (!target_str.empty() && (target_str.back() == '/' || target_str.back() == '\\')) {
        target_str.pop_back();
    }
    if (target_str.size() < root_str.size()) {
        return false;
    }
    if (target_str.rfind(root_str, 0) != 0) {
        return false;
    }
    if (target_str.size() > root_str.size()) {
        const char next = target_str[root_str.size()];
        return next == '/' || next == '\\';
    }
    return true;
}

std::string StorageEngine::resolve_storage_file_path(const std::string& exchange, const std::string& product,
                                                       const std::string& month_slot,
                                                       const std::string& period) const {
    if (!is_safe_path_component(exchange) || !is_safe_path_component(product) ||
        !is_safe_path_component(month_slot)) {
        return {};
    }
    static const std::array<const char*, 5> kPeriods = {"tick", "m1", "m15", "h1", "d1"};
    const bool valid_period =
        std::any_of(kPeriods.begin(), kPeriods.end(), [&](const char* p) { return period == p; });
    if (!valid_period) {
        return {};
    }
    return resolve_data_file_path(root_, exchange, product, month_slot, period).string();
}

namespace {

int period_sort_rank(const std::string& period) {
    if (period == "tick") {
        return 0;
    }
    if (period == "m1") {
        return 1;
    }
    if (period == "m15") {
        return 2;
    }
    if (period == "h1") {
        return 3;
    }
    if (period == "d1") {
        return 4;
    }
    return 100;
}

nlohmann::json make_name_item(const std::string& name) {
    return nlohmann::json{{"name", name}};
}

}  // namespace

nlohmann::json StorageEngine::browse_data(const std::string& exchange, const std::string& product,
                                          const std::string& month_slot) const {
    nlohmann::json result = {{"level", "exchange"},
                             {"items", nlohmann::json::array()},
                             {"layout", "data/{exchange}/{product}/{month_slot}/{period}.csv"},
                             {"exchange", exchange},
                             {"product", product},
                             {"month_slot", month_slot}};
    const fs::path data_root = root_ / "data";
    if (!fs::exists(data_root)) {
        return result;
    }

    if (exchange.empty()) {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(data_root)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            result["items"].push_back(make_name_item(name));
        }
        return result;
    }

    if (!is_safe_path_component(exchange)) {
        result["error"] = "exchange 非法";
        return result;
    }

    const fs::path exchange_path = data_root / exchange;
    if (!fs::exists(exchange_path)) {
        result["level"] = "product";
        return result;
    }

    if (product.empty()) {
        result["level"] = "product";
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(exchange_path)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            result["items"].push_back(make_name_item(name));
        }
        return result;
    }

    if (!is_safe_path_component(product)) {
        result["error"] = "product 非法";
        return result;
    }

    const fs::path product_path = exchange_path / product;
    if (!fs::exists(product_path)) {
        result["level"] = "month_slot";
        return result;
    }

    if (month_slot.empty()) {
        result["level"] = "month_slot";
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(product_path)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            result["items"].push_back(make_name_item(name));
        }
        return result;
    }

    if (!is_safe_path_component(month_slot)) {
        result["error"] = "month_slot 非法";
        return result;
    }

    const fs::path slot_path = product_path / month_slot;
    if (!fs::exists(slot_path)) {
        result["level"] = "file";
        return result;
    }

    result["level"] = "file";
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file_entry : fs::directory_iterator(slot_path)) {
        if (!file_entry.is_regular_file()) {
            continue;
        }
        const std::string fname = file_entry.path().filename().string();
        if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".csv") {
            continue;
        }

        std::string file_type;
        std::string period;
        if (fname == "tick.csv") {
            file_type = "tick";
            period = "tick";
        } else {
            file_type = "bar";
            period = fname.substr(0, fname.size() - 4);
        }

        std::error_code ec;
        const auto size_bytes = fs::file_size(file_entry.path(), ec);
        files.push_back({{"file_type", file_type},
                         {"period", period},
                         {"exchange", exchange},
                         {"product", product},
                         {"month_slot", month_slot},
                         {"path", file_entry.path().string()},
                         {"size_bytes", ec ? 0 : size_bytes},
                         {"row_count", -1},
                         {"instrument_id", ""},
                         {"name", ""}});
    }

    std::sort(files.begin(), files.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        const int ra = period_sort_rank(a.value("period", ""));
        const int rb = period_sort_rank(b.value("period", ""));
        if (ra != rb) {
            return ra < rb;
        }
        return a.value("period", "") < b.value("period", "");
    });
    result["items"] = files;
    return result;
}

DataFileMutationResult StorageEngine::delete_data_file(const std::string& path) const {
    DataFileMutationResult result;
    if (path.empty()) {
        result.message = "path required";
        return result;
    }
    const fs::path file_path(path);
    if (!is_safe_data_path(file_path)) {
        result.message = "路径不在 data/ 目录内";
        return result;
    }
    if (!fs::exists(file_path)) {
        result.message = "文件不存在";
        result.path = path;
        return result;
    }
    std::error_code ec;
    if (!fs::remove(file_path, ec)) {
        result.message = ec.message().empty() ? "删除失败" : ec.message();
        return result;
    }
    result.ok = true;
    result.message = "已删除";
    result.path = path;
    return result;
}

DataFileMutationResult StorageEngine::import_data_csv(const std::string& exchange, const std::string& product,
                                                      const std::string& month_slot, const std::string& period,
                                                      const std::string& csv_content, const std::string& mode) const {
    DataFileMutationResult result;
    if (!is_safe_path_component(exchange) || !is_safe_path_component(product) ||
        !is_safe_path_component(month_slot)) {
        result.message = "exchange/product/month_slot 非法";
        return result;
    }
    static const std::array<const char*, 5> kPeriods = {"tick", "m1", "m15", "h1", "d1"};
    const bool valid_period =
        std::any_of(kPeriods.begin(), kPeriods.end(), [&](const char* p) { return period == p; });
    if (!valid_period) {
        result.message = "period 必须为 tick|m1|m15|h1|d1";
        return result;
    }
    if (csv_content.empty()) {
        result.message = "csv_content 为空";
        return result;
    }

    const fs::path file_path = resolve_data_file_path(root_, exchange, product, month_slot, period);
    if (!is_safe_data_path(file_path)) {
        result.message = "目标路径非法";
        return result;
    }

    const std::string header = expected_csv_header(period);
    const bool append_mode = mode == "append";
    if (mode != "append" && mode != "replace") {
        result.message = "mode 必须为 replace 或 append";
        return result;
    }

    std::istringstream in(csv_content);
    std::string first_line;
    if (!std::getline(in, first_line)) {
        result.message = "CSV 内容无效";
        return result;
    }

    const bool first_is_header =
        first_line.find("date") != std::string::npos || first_line.find("trading_day") != std::string::npos;
    std::string body;
    if (append_mode && first_is_header) {
        body = csv_content.substr(first_line.size() + 1);
    } else if (!append_mode && first_is_header) {
        body = csv_content;
    } else if (append_mode) {
        body = csv_content;
    } else {
        body = header + '\n' + csv_content;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    fs::create_directories(file_path.parent_path());

    if (append_mode) {
        std::ofstream out(file_path, std::ios::app);
        if (!out) {
            result.message = "无法打开文件追加";
            return result;
        }
        ensure_header(out, header, file_path);
        if (!body.empty() && body.back() != '\n') {
            out << body << '\n';
        } else {
            out << body;
        }
    } else {
        std::ofstream out(file_path, std::ios::trunc);
        if (!out) {
            result.message = "无法写入文件";
            return result;
        }
        out << body;
        if (!body.empty() && body.back() != '\n') {
            out << '\n';
        }
    }

    result.ok = true;
    result.message = append_mode ? "已追加导入" : "已覆盖导入";
    result.path = file_path.string();
    result.row_count = count_csv_data_rows(file_path);
    return result;
}

std::vector<BarRecord> StorageEngine::read_bars_at_path(const std::string& path, int limit) const {
    const fs::path file_path(path);
    if (!is_safe_data_path(file_path)) {
        return {};
    }
    return read_bar_csv(file_path, limit);
}

std::vector<TickRecord> StorageEngine::read_ticks_at_path(const std::string& path, int limit) const {
    const fs::path file_path(path);
    if (!is_safe_data_path(file_path)) {
        return {};
    }
    return read_tick_csv(file_path, limit);
}

}  // namespace quant_sev::bll
