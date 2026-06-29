#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vf_policy {

struct RiskLimits {
  double max_abs_delta{};
  double max_abs_gamma{};
  double max_abs_vega{};
  double max_notional{};
  double max_daily_loss{};
  double max_drawdown{};
  std::int64_t max_surface_age_ns{};
  std::int64_t max_execution_age_ns{};
  std::int64_t max_quote_age_ns{};
};

struct PolicyConfig {
  std::string component_id{"policy"};
  std::string decision_id{"policy-smoke-1"};
  std::string policy_model_id{"inventory_skew_v1"};
  std::string surface_id{"clean_smile_v1"};
  std::string quote_id{"clean_1p0_100"};
  std::int64_t decision_ts_ns{};
  std::int64_t execution_reference_ts_ns{};
  std::int64_t quote_quantity{1};
  double half_spread{};
  double inventory_skew_per_delta{};
  double min_hedge_delta{};
  double hedge_delta_limit{};
  double pnl_residual_tolerance{1e-9};
  double toxicity_penalty{};
  double fill_probability_adjustment{};
  double simulated_daily_pnl{};
  double simulated_peak_pnl{};
  bool feed_gap_detected{};
  bool corrupt_snapshot_detected{};
  RiskLimits risk;
};

struct Inventory {
  double delta{};
  double gamma{};
  double vega{};
  double cash{};
  double notional{};
};

struct SurfaceInput {
  bool present{};
  std::string surface_id;
  std::string surface_model_id;
  std::string status;
  std::string reason;
  std::int64_t evaluation_ts_ns{};
  std::int64_t max_quote_age_ns{};
  bool interpolated_fair_value_present{};
  std::string interpolation_status;
  double interpolated_fair_value{};
};

struct QuoteInput {
  bool present{};
  std::string surface_id;
  std::string quote_id;
  std::string status;
  std::int64_t quote_ts_ns{};
  double mid{};
};

struct ExecutionInput {
  bool present{};
  bool safe{};
  std::string status;
  std::string reason;
  std::string queue_model_id;
  std::string latency_model_id;
  std::int64_t decision_ts_ns{};
  std::int64_t effective_arrival_ts_ns{};
  bool has_source_replay_checksum{};
  bool has_decision_snapshot_state_checksum{};
  std::uint64_t source_replay_checksum{};
  std::uint64_t decision_snapshot_state_checksum{};
};

struct QuoteDecision {
  std::string schema_version{"vegaflux.canonical_market.v0.1"};
  std::string component_id{"policy"};
  std::string decision_id;
  std::string status;
  std::string reason;
  std::string policy_model_id;
  std::string queue_model_id;
  std::string latency_model_id;
  std::string surface_model_id;
  std::string surface_id;
  std::string quote_id;
  std::int64_t decision_ts_ns{};
  std::int64_t surface_age_ns{};
  std::int64_t execution_age_ns{};
  std::int64_t selected_quote_ts_ns{};
  std::int64_t quote_age_ns{};
  std::string fair_value_source;
  double fair_value{};
  double fair_bid{};
  double fair_ask{};
  double inventory_skew{};
  double toxicity_penalty{};
  double fill_probability_adjustment{};
  std::optional<double> bid;
  std::optional<double> ask;
  std::int64_t quantity{};
  bool hedge_eligible{};
  std::string hedge_status;
};

struct RiskGateEvent {
  std::string gate;
  std::string status;
  std::string reason;
};

struct SyntheticFill {
  std::string fill_id;
  std::string side;
  double quantity{};
  double fill_price{};
  double fair_value{};
  double mark_price{};
  double fee{};
  double slippage{};
};

struct PnlAttribution {
  std::string schema_version{"vegaflux.canonical_market.v0.1"};
  std::string component_id{"policy"};
  std::string status;
  std::size_t fill_count{};
  double ending_inventory{};
  double total_pnl{};
  double quoted_spread_capture{};
  double inventory_revaluation{};
  double delta_hedge_pnl{};
  double vega_explain{};
  double gamma_explain{};
  double theta_explain{};
  double fees{};
  double slippage{};
  double residual{};
  double residual_tolerance{};
  bool within_tolerance{};
};

SurfaceInput LoadSurfaceInput(const std::filesystem::path& path, const std::string& surface_id);
QuoteInput LoadQuoteInput(const std::filesystem::path& path, const std::string& surface_id, const std::string& quote_id);
ExecutionInput LoadExecutionInput(const std::filesystem::path& path);
void ValidateConfig(const PolicyConfig& config);
QuoteDecision MakeQuoteDecision(const PolicyConfig& config,
                                const Inventory& inventory,
                                const SurfaceInput& surface,
                                const QuoteInput& quote,
                                const ExecutionInput& execution);
std::vector<RiskGateEvent> EvaluateRiskGates(const PolicyConfig& config,
                                             const Inventory& inventory,
                                             const SurfaceInput& surface,
                                             const QuoteInput& quote,
                                             const ExecutionInput& execution);
std::vector<SyntheticFill> LoadSyntheticFills(const std::filesystem::path& path);
PnlAttribution AttributePnl(const std::vector<SyntheticFill>& fills, double tolerance);
std::string QuoteDecisionJson(const QuoteDecision& decision);
std::string RiskGateEventJson(const RiskGateEvent& event);
std::string PnlAttributionJson(const PnlAttribution& attribution);
void WriteQuoteDecisionsJsonl(const std::filesystem::path& path, const std::vector<QuoteDecision>& decisions);
void WriteRiskGateEventsJsonl(const std::filesystem::path& path, const std::vector<RiskGateEvent>& events);
void WriteAblationArtifacts(const std::filesystem::path& csv_path,
                            const std::filesystem::path& json_path,
                            const PolicyConfig& config,
                            const Inventory& inventory,
                            const SurfaceInput& surface,
                            const QuoteInput& quote,
                            const ExecutionInput& execution);
void WritePnlAttribution(const std::filesystem::path& path, const PnlAttribution& attribution);

}  // namespace vf_policy
