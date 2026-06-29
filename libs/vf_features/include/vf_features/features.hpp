#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vf_features {

struct FeatureConfig {
  std::filesystem::path input_path;
  std::optional<std::filesystem::path> fill_labels_path;
  std::uint32_t depth{1};
  std::size_t label_horizon_events{1};
  std::size_t feature_window_events{3};
  std::string dataset_id{"synthetic_features_v1"};
};

struct FeatureRow {
  std::string schema_version;
  std::string dataset_id;
  std::string venue;
  std::string symbol;
  std::string session;
  std::uint64_t row_index{};
  std::string split;
  std::uint64_t sequence_number{};
  std::uint64_t source_sequence_number{};
  std::uint64_t feature_source_sequence_number{};
  std::string event_type;
  std::string side;
  std::int64_t event_ts_ns{};
  std::int64_t receive_ts_ns{};
  std::int64_t available_at_ns{};
  std::int64_t feature_max_source_available_at_ns{};
  std::uint64_t snapshot_state_checksum{};
  std::uint64_t replay_checksum{};
  std::optional<std::int64_t> best_bid_ticks;
  std::optional<std::int64_t> best_ask_ticks;
  std::optional<std::int64_t> spread_ticks;
  std::optional<std::int64_t> mid_ticks_x2;
  std::optional<std::int64_t> microprice_ticks_x2;
  std::int64_t bid_qty_top1{};
  std::int64_t ask_qty_top1{};
  std::int64_t depth_bid_qty{};
  std::int64_t depth_ask_qty{};
  std::int64_t depth_imbalance_x1e6{};
  std::int64_t order_flow_imbalance_qty{};
  std::optional<std::int64_t> event_rate_per_second_x1e6;
  std::optional<std::int64_t> quote_age_ns;
  std::optional<std::int64_t> short_window_realized_volatility_x1e6;
  std::optional<std::int64_t> future_mid_move_ticks_x2;
  std::optional<std::int64_t> realized_spread_proxy_ticks_x2;
  std::optional<std::int64_t> adverse_selection_proxy_ticks_x2;
  std::optional<std::int64_t> fill_within_horizon_proxy;
  std::string label_status;
};

struct Dataset {
  FeatureConfig config;
  std::vector<FeatureRow> rows;
  std::uint64_t replay_checksum{};
};

Dataset BuildDataset(const FeatureConfig& config);
bool PassesLeakageGuard(const std::vector<FeatureRow>& rows);
std::string FeatureRowJson(const FeatureRow& row);
std::string DatasetMetadataJson(const Dataset& dataset);
void WriteFeaturesJsonl(const std::filesystem::path& path, const std::vector<FeatureRow>& rows);
void WriteDatasetMetadata(const std::filesystem::path& path, const Dataset& dataset);

}  // namespace vf_features
