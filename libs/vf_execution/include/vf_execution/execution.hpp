#pragma once

#include "aegis_lob/book.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vf_execution {

enum class Side { kBuy, kSell };

struct FeatureRow {
  std::string schema_version;
  std::string dataset_id;
  std::uint64_t row_index{};
  std::uint64_t sequence_number{};
  std::uint64_t source_sequence_number{};
  std::int64_t available_at_ns{};
  std::uint64_t snapshot_state_checksum{};
  std::uint64_t replay_checksum{};
  std::optional<std::int64_t> mid_ticks_x2;
  std::int64_t bid_qty_top1{};
  std::int64_t ask_qty_top1{};
};

struct SimulationConfig {
  std::string component_id{"execution"};
  std::string simulation_id{"visible_fifo_smoke_v1"};
  std::string queue_model_id{"visible_fifo_v1"};
  std::string latency_model_id{"fixed_latency_ns_v1"};
  std::int64_t fixed_latency_ns{};
  std::int64_t horizon_ns{};
  std::int64_t markout_horizon_ns{100'000'000};
  std::uint32_t depth{5};
  double hidden_ahead_multiplier{};
  double latency_multiplier{1.0};
  double cancel_rate_multiplier{1.0};
};

struct SimOrder {
  std::string order_id;
  Side side{Side::kBuy};
  std::int64_t price_ticks{};
  std::int64_t quantity{};
  std::uint64_t decision_sequence_number{};
};

struct FillResult {
  std::string schema_version{"vegaflux.canonical_market.v0.1"};
  std::string simulation_id;
  std::string sim_order_id;
  Side side{Side::kBuy};
  std::int64_t price_ticks{};
  std::int64_t order_quantity{};
  std::uint64_t decision_sequence_number{};
  std::int64_t decision_ts_ns{};
  std::int64_t effective_arrival_ts_ns{};
  std::int64_t horizon_end_ts_ns{};
  std::int64_t visible_qty_ahead_at_decision{};
  double hidden_ahead_multiplier{};
  double latency_multiplier{1.0};
  double cancel_rate_multiplier{1.0};
  std::int64_t filled_quantity{};
  std::optional<std::int64_t> first_fill_ts_ns;
  std::optional<std::uint64_t> first_fill_sequence_number;
  std::string status;
  std::string queue_model_id;
  std::string latency_model_id;
  std::int64_t markout_horizon_ns{};
  std::optional<std::int64_t> markout_ticks_x2;
  std::uint64_t source_replay_checksum{};
  std::uint64_t decision_snapshot_state_checksum{};
  std::string source_dataset_id;
};

std::vector<FeatureRow> LoadFeatureRows(const std::filesystem::path& path);
FillResult SimulateVisibleFifo(const SimulationConfig& config,
                               const SimOrder& order,
                               const std::vector<aegis_lob::CanonicalEvent>& events,
                               const std::vector<FeatureRow>& features);
std::string FillResultJson(const FillResult& result);
std::string SimulationMetadataJson(const SimulationConfig& config,
                                   const std::filesystem::path& input,
                                   const std::filesystem::path& features,
                                   std::size_t result_count,
                                   std::uint64_t replay_checksum);
void WriteFillsJsonl(const std::filesystem::path& path, const std::vector<FillResult>& results);
void WriteSimulationMetadata(const std::filesystem::path& path,
                             const SimulationConfig& config,
                             const std::filesystem::path& input,
                             const std::filesystem::path& features,
                             std::size_t result_count,
                             std::uint64_t replay_checksum);
std::string SideName(Side side);
Side ParseSide(const std::string& value);

}  // namespace vf_execution
