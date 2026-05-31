#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Common/ContractRules.hpp"
#include "Storage/StorageTypes.hpp"

namespace quant_sev::bll {

class StorageEngine {
public:
    void set_project_root(const std::filesystem::path& root);
    bool initialize();

    void write_tick(const nlohmann::json& tick);
    void append_bar(const ParsedInstrument& instrument, const std::string& period, const BarRecord& bar);
    std::vector<BarRecord> query_bars(const BarQuery& query) const;
    BarFileInfo bar_file_info(const std::string& instrument_id, const std::string& period) const;
    std::vector<DataFileEntry> list_data_inventory() const;
    nlohmann::json browse_data(const std::string& exchange, const std::string& product,
                               const std::string& month_slot) const;
    std::string resolve_storage_file_path(const std::string& exchange, const std::string& product,
                                          const std::string& month_slot, const std::string& period) const;
    nlohmann::json scan_data_catalog() const;
    std::vector<TickRecord> query_ticks(const TickQuery& query) const;
    DataFileMutationResult delete_data_file(const std::string& path) const;
    DataFileMutationResult import_data_csv(const std::string& exchange, const std::string& product,
                                           const std::string& month_slot, const std::string& period,
                                           const std::string& csv_content, const std::string& mode) const;
    std::vector<BarRecord> read_bars_at_path(const std::string& path, int limit) const;
    std::vector<TickRecord> read_ticks_at_path(const std::string& path, int limit) const;
    std::optional<ParsedInstrument> parse_instrument(const std::string& instrument_id) const;
    std::filesystem::path config_path(const std::string& filename) const;

private:
    bool is_safe_data_path(const std::filesystem::path& path) const;
    static bool is_safe_path_component(const std::string& value);
    static std::filesystem::path resolve_data_file_path(const std::filesystem::path& root,
                                                      const std::string& exchange,
                                                      const std::string& product,
                                                      const std::string& month_slot,
                                                      const std::string& period);
    static std::string expected_csv_header(const std::string& period);
    std::filesystem::path slot_dir_path(const ParsedInstrument& instrument) const;
    std::filesystem::path historical_bar_path(const ParsedInstrument& instrument,
                                             const std::string& period) const;
    std::filesystem::path tick_path(const ParsedInstrument& instrument) const;
    static std::vector<BarRecord> read_bar_csv(const std::filesystem::path& path, int limit);
    static std::vector<TickRecord> read_tick_csv(const std::filesystem::path& path, int limit);
    static void ensure_header(std::ofstream& out, const std::string& header, const std::filesystem::path& path);

    std::filesystem::path root_{"."};
    ContractRules rules_;
    mutable std::mutex mutex_;
};

}  // namespace quant_sev::bll
