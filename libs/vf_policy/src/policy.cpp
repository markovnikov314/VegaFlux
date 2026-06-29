#include "vf_policy/policy.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace vf_policy {
namespace {

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
    }
  }
  return out;
}

std::string JsonString(const std::string& value) {
  return "\"" + Escape(value) + "\"";
}

std::string JsonDouble(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  std::ostringstream out;
  out << std::setprecision(15) << value;
  return out.str();
}

std::string JsonOptionalDouble(std::optional<double> value) {
  return value.has_value() ? JsonDouble(*value) : "null";
}

void AddJsonField(std::ostringstream& out, const std::string& name, const std::string& value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << '"' << name << "\":" << value;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t ValuePos(const std::string& text, const std::string& key) {
  const auto needle = "\"" + key + "\":";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON key: " + key);
  }
  return pos + needle.size();
}

void SkipSpaces(const std::string& text, std::size_t& pos) {
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t')) {
    ++pos;
  }
}

std::string StringField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  if (pos >= text.size() || text[pos] != '"') {
    throw std::runtime_error("expected string JSON field: " + key);
  }
  ++pos;
  std::string out;
  for (; pos < text.size(); ++pos) {
    if (text[pos] == '"') {
      return out;
    }
    if (text[pos] == '\\' && pos + 1 < text.size()) {
      ++pos;
    }
    out.push_back(text[pos]);
  }
  throw std::runtime_error("unterminated string JSON field: " + key);
}

double DoubleField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stod(text.substr(pos, end - pos));
}

std::int64_t IntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

std::uint64_t UInt64Field(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoull(text.substr(pos, end - pos));
}

bool HasField(const std::string& text, const std::string& key) {
  return text.find("\"" + key + "\":") != std::string::npos;
}

bool AllowedFillStatus(const std::string& status) {
  return status == "FULL" || status == "PARTIAL_CENSORED" || status == "UNFILLED_CENSORED";
}

bool AllowedPolicyModel(const std::string& policy_model_id) {
  return policy_model_id == "passive_fixed_spread_v1" || policy_model_id == "fair_value_only_v1" ||
         policy_model_id == "inventory_skew_v1" || policy_model_id == "inventory_skew_risk_v1";
}

std::int64_t AbsAge(std::int64_t left, std::int64_t right) {
  return left > right ? left - right : right - left;
}

std::int64_t QuoteAgeLimit(const PolicyConfig& config, const SurfaceInput& surface) {
  return config.risk.max_quote_age_ns > 0 ? config.risk.max_quote_age_ns : surface.max_quote_age_ns;
}

double SelectedFairValue(const SurfaceInput& surface, const QuoteInput& quote) {
  return surface.interpolated_fair_value_present && surface.interpolation_status == "PASS"
             ? surface.interpolated_fair_value
             : quote.mid;
}

std::string SelectedFairValueSource(const SurfaceInput& surface) {
  return surface.interpolated_fair_value_present && surface.interpolation_status == "PASS"
             ? "interpolated_surface"
             : "accepted_quote_mid";
}

QuoteDecision BlockedDecision(const PolicyConfig& config,
                              const Inventory& inventory,
                              const SurfaceInput& surface,
                              const QuoteInput& quote,
                              const ExecutionInput& execution,
                              const std::string& status,
                              const std::string& reason) {
  QuoteDecision decision;
  decision.component_id = config.component_id;
  decision.decision_id = config.decision_id;
  decision.status = status;
  decision.reason = reason;
  decision.policy_model_id = config.policy_model_id;
  decision.queue_model_id = execution.queue_model_id;
  decision.latency_model_id = execution.latency_model_id;
  decision.surface_model_id = surface.surface_model_id;
  decision.surface_id = config.surface_id;
  decision.quote_id = config.quote_id;
  decision.decision_ts_ns = config.decision_ts_ns;
  decision.surface_age_ns = config.decision_ts_ns - surface.evaluation_ts_ns;
  decision.execution_age_ns = AbsAge(config.execution_reference_ts_ns, execution.decision_ts_ns);
  decision.selected_quote_ts_ns = quote.quote_ts_ns;
  decision.quote_age_ns = config.decision_ts_ns - quote.quote_ts_ns;
  decision.fair_value = SelectedFairValue(surface, quote);
  decision.fair_value_source = SelectedFairValueSource(surface);
  decision.fair_bid = decision.fair_value - config.half_spread;
  decision.fair_ask = decision.fair_value + config.half_spread;
  decision.inventory_skew = -inventory.delta * config.inventory_skew_per_delta;
  decision.toxicity_penalty = config.toxicity_penalty;
  decision.fill_probability_adjustment = config.fill_probability_adjustment;
  decision.quantity = config.quote_quantity;
  decision.hedge_eligible = false;
  decision.hedge_status = "QUOTE_BLOCKED";
  return decision;
}

}  // namespace

SurfaceInput LoadSurfaceInput(const std::filesystem::path& path, const std::string& surface_id) {
  SurfaceInput input;
  input.surface_id = surface_id;
  try {
    const auto text = ReadFile(path);
    input.present = true;
    input.evaluation_ts_ns = IntField(text, "evaluation_ts_ns");
    input.max_quote_age_ns = IntField(text, "max_quote_age_ns");
    const auto needle = "\"surface_id\":\"" + surface_id + "\"";
    const auto pos = text.find(needle);
    if (pos == std::string::npos) {
      input.status = "MISSING_SURFACE";
      input.reason = "selected_surface_not_found";
      return input;
    }
    const auto segment = text.substr(pos, 5000);
    input.surface_model_id = StringField(segment, "surface_model_id");
    input.status = StringField(segment, "status");
    input.reason = StringField(segment, "reason");
    const auto interpolation_pos = segment.find("\"interpolation\":");
    if (interpolation_pos != std::string::npos) {
      const auto interpolation = segment.substr(interpolation_pos);
      input.interpolation_status = StringField(interpolation, "status");
      if (input.interpolation_status == "PASS" && HasField(interpolation, "fair_value")) {
        input.interpolated_fair_value = DoubleField(interpolation, "fair_value");
        input.interpolated_fair_value_present = true;
      }
    }
  } catch (const std::exception& error) {
    input.present = false;
    input.status = "MISSING_SURFACE";
    input.reason = error.what();
  }
  return input;
}

QuoteInput LoadQuoteInput(const std::filesystem::path& path, const std::string& surface_id, const std::string& quote_id) {
  std::ifstream in(path, std::ios::binary);
  QuoteInput input;
  input.surface_id = surface_id;
  input.quote_id = quote_id;
  if (!in) {
    input.status = "MISSING_QUOTE_FILE";
    return input;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (StringField(line, "surface_id") == surface_id && StringField(line, "quote_id") == quote_id) {
      input.present = true;
      input.status = StringField(line, "status");
      input.quote_ts_ns = IntField(line, "quote_ts_ns");
      input.mid = DoubleField(line, "mid");
      return input;
    }
  }
  input.status = "MISSING_QUOTE";
  return input;
}

ExecutionInput LoadExecutionInput(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  ExecutionInput input;
  if (!in) {
    input.status = "MISSING_EXECUTION";
    input.reason = "execution_artifact_not_found";
    return input;
  }
  std::string line;
  if (!std::getline(in, line) || line.empty()) {
    input.status = "MISSING_EXECUTION";
    input.reason = "execution_artifact_empty";
    return input;
  }
  try {
    input.present = true;
    input.queue_model_id = StringField(line, "queue_model_id");
    input.latency_model_id = StringField(line, "latency_model_id");
    input.status = StringField(line, "status");
    input.decision_ts_ns = IntField(line, "decision_ts_ns");
    input.effective_arrival_ts_ns = IntField(line, "effective_arrival_ts_ns");
    if (HasField(line, "source_replay_checksum")) {
      input.has_source_replay_checksum = true;
      input.source_replay_checksum = UInt64Field(line, "source_replay_checksum");
    }
    if (HasField(line, "decision_snapshot_state_checksum")) {
      input.has_decision_snapshot_state_checksum = true;
      input.decision_snapshot_state_checksum = UInt64Field(line, "decision_snapshot_state_checksum");
    }
    input.safe = !input.queue_model_id.empty() && !input.latency_model_id.empty() && AllowedFillStatus(input.status) &&
                 input.effective_arrival_ts_ns >= input.decision_ts_ns;
    input.reason = input.safe ? "execution_input_ok" : "unsafe_execution_input";
  } catch (const std::exception& error) {
    input.present = true;
    input.safe = false;
    input.status = "UNSAFE_EXECUTION";
    input.reason = error.what();
  }
  return input;
}

void ValidateConfig(const PolicyConfig& config) {
  const auto finite = [](double value) { return std::isfinite(value); };
  if (config.quote_quantity <= 0) {
    throw std::runtime_error("quote_quantity_must_be_positive");
  }
  if (!finite(config.half_spread) || config.half_spread < 0.0) {
    throw std::runtime_error("half_spread_must_be_finite_non_negative");
  }
  if (!finite(config.inventory_skew_per_delta) || config.inventory_skew_per_delta < 0.0) {
    throw std::runtime_error("inventory_skew_per_delta_must_be_finite_non_negative");
  }
  if (!finite(config.pnl_residual_tolerance) || config.pnl_residual_tolerance < 0.0) {
    throw std::runtime_error("pnl_residual_tolerance_must_be_finite_non_negative");
  }
  if (!finite(config.toxicity_penalty) || config.toxicity_penalty < 0.0) {
    throw std::runtime_error("toxicity_penalty_must_be_finite_non_negative");
  }
  if (!finite(config.fill_probability_adjustment) || config.fill_probability_adjustment < 0.0) {
    throw std::runtime_error("fill_probability_adjustment_must_be_finite_non_negative");
  }
  if (!finite(config.risk.max_abs_delta) || config.risk.max_abs_delta < 0.0 ||
      !finite(config.risk.max_abs_gamma) || config.risk.max_abs_gamma < 0.0 ||
      !finite(config.risk.max_abs_vega) || config.risk.max_abs_vega < 0.0 ||
      !finite(config.risk.max_notional) || config.risk.max_notional < 0.0 ||
      !finite(config.risk.max_daily_loss) || config.risk.max_daily_loss < 0.0 ||
      !finite(config.risk.max_drawdown) || config.risk.max_drawdown < 0.0) {
    throw std::runtime_error("risk_limits_must_be_finite_non_negative");
  }
  if (config.risk.max_surface_age_ns < 0 || config.risk.max_execution_age_ns < 0 ||
      config.risk.max_quote_age_ns < 0) {
    throw std::runtime_error("age_limits_must_be_non_negative");
  }
  if (!finite(config.min_hedge_delta) || !finite(config.hedge_delta_limit) || config.min_hedge_delta < 0.0 ||
      config.hedge_delta_limit < 0.0 || config.min_hedge_delta > config.hedge_delta_limit) {
    throw std::runtime_error("hedge_limits_invalid");
  }
}

std::vector<RiskGateEvent> EvaluateRiskGates(const PolicyConfig& config,
                                             const Inventory& inventory,
                                             const SurfaceInput& surface,
                                             const QuoteInput& quote,
                                             const ExecutionInput& execution) {
  ValidateConfig(config);
  std::vector<RiskGateEvent> events;
  const auto add = [&events](std::string gate, std::string status, std::string reason) {
    events.push_back({std::move(gate), std::move(status), std::move(reason)});
  };

  add("policy_model_id", AllowedPolicyModel(config.policy_model_id) ? "PASS" : "NO_QUOTE_BAD_POLICY_MODEL",
      AllowedPolicyModel(config.policy_model_id) ? "implemented_policy_model" : config.policy_model_id);
  add("surface_status", surface.present && surface.status == "PASS" ? "PASS" : "NO_QUOTE_SURFACE_STATUS",
      surface.present ? surface.status : "missing_surface");
  add("quote_status", quote.present && quote.status == "ACCEPTED" ? "PASS" : "NO_QUOTE_QUOTE_STATUS",
      quote.present ? quote.status : "missing_quote");
  add("execution_present", execution.present ? "PASS" : "NO_QUOTE_EXECUTION_MISSING", execution.reason);
  add("execution_safe", execution.safe ? "PASS" : "NO_QUOTE_EXECUTION_UNSAFE", execution.reason);
  add("feed_gap", config.feed_gap_detected ? "NO_QUOTE_FEED_GAP" : "PASS",
      config.feed_gap_detected ? "feed_gap_detected" : "feed_metadata_ok");

  const bool corrupt_snapshot =
      config.corrupt_snapshot_detected ||
      (execution.has_source_replay_checksum && execution.source_replay_checksum == 0) ||
      (execution.has_decision_snapshot_state_checksum && execution.decision_snapshot_state_checksum == 0);
  add("corrupt_snapshot", corrupt_snapshot ? "NO_QUOTE_CORRUPT_SNAPSHOT" : "PASS",
      corrupt_snapshot ? "corrupt_or_missing_snapshot_checksum" : "snapshot_metadata_ok");

  const auto quote_age = config.decision_ts_ns - quote.quote_ts_ns;
  const auto quote_age_limit = QuoteAgeLimit(config, surface);
  add("quote_age", quote_age < 0 ? "NO_QUOTE_QUOTE_FUTURE"
                                 : quote_age_limit > 0 && quote_age > quote_age_limit ? "NO_QUOTE_QUOTE_STALE"
                                                                                       : "PASS",
      quote_age < 0 ? "quote_timestamp_after_decision" : "quote_age_checked");

  const auto execution_age = AbsAge(config.execution_reference_ts_ns, execution.decision_ts_ns);
  add("execution_age", execution_age > config.risk.max_execution_age_ns ? "NO_QUOTE_EXECUTION_STALE" : "PASS",
      execution_age > config.risk.max_execution_age_ns ? "execution_age_exceeds_limit" : "execution_age_checked");

  const auto surface_age = config.decision_ts_ns - surface.evaluation_ts_ns;
  add("surface_age", surface_age < 0 ? "NO_QUOTE_SURFACE_FUTURE"
                                     : surface_age > config.risk.max_surface_age_ns ? "NO_QUOTE_SURFACE_STALE"
                                                                                    : "PASS",
      surface_age < 0 ? "surface_timestamp_after_decision" : "surface_age_checked");

  add("delta", std::abs(inventory.delta) > config.risk.max_abs_delta ? "NO_QUOTE_RISK_DELTA" : "PASS",
      "delta_limit_checked");
  add("gamma", std::abs(inventory.gamma) > config.risk.max_abs_gamma ? "NO_QUOTE_RISK_GAMMA" : "PASS",
      "gamma_limit_checked");
  add("vega", std::abs(inventory.vega) > config.risk.max_abs_vega ? "NO_QUOTE_RISK_VEGA" : "PASS",
      "vega_limit_checked");

  const auto decision_notional = std::abs(SelectedFairValue(surface, quote) * static_cast<double>(config.quote_quantity));
  add("notional", std::max(std::abs(inventory.notional), decision_notional) > config.risk.max_notional
                      ? "NO_QUOTE_RISK_NOTIONAL"
                      : "PASS",
      "notional_limit_checked");

  add("daily_loss", config.risk.max_daily_loss > 0.0 && config.simulated_daily_pnl < -config.risk.max_daily_loss
                        ? "NO_QUOTE_DAILY_LOSS"
                        : "PASS",
      "simulated_daily_loss_checked");

  const auto drawdown = std::max(0.0, config.simulated_peak_pnl - config.simulated_daily_pnl);
  add("drawdown", config.risk.max_drawdown > 0.0 && drawdown > config.risk.max_drawdown ? "NO_QUOTE_DRAWDOWN"
                                                                                        : "PASS",
      "simulated_drawdown_checked");
  return events;
}

QuoteDecision MakeQuoteDecision(const PolicyConfig& config,
                                const Inventory& inventory,
                                const SurfaceInput& surface,
                                const QuoteInput& quote,
                                const ExecutionInput& execution) {
  const auto gates = EvaluateRiskGates(config, inventory, surface, quote, execution);
  for (const auto& gate : gates) {
    if (gate.status != "PASS") {
      return BlockedDecision(config, inventory, surface, quote, execution, gate.status, gate.reason);
    }
  }

  const auto execution_age = AbsAge(config.execution_reference_ts_ns, execution.decision_ts_ns);
  const auto surface_age = config.decision_ts_ns - surface.evaluation_ts_ns;

  QuoteDecision decision = BlockedDecision(config, inventory, surface, quote, execution, "QUOTE", "ok");
  decision.surface_age_ns = surface_age;
  decision.execution_age_ns = execution_age;
  double skew = 0.0;
  double penalty = 0.0;
  if (config.policy_model_id == "inventory_skew_v1" || config.policy_model_id == "inventory_skew_risk_v1") {
    skew = decision.inventory_skew;
  }
  if (config.policy_model_id == "inventory_skew_risk_v1") {
    penalty = config.toxicity_penalty + config.fill_probability_adjustment;
  }
  decision.bid = decision.fair_bid + skew - penalty;
  decision.ask = decision.fair_ask + skew + penalty;
  decision.hedge_eligible = std::abs(inventory.delta) >= config.min_hedge_delta &&
                            std::abs(inventory.delta) <= config.hedge_delta_limit;
  decision.hedge_status = decision.hedge_eligible ? "ELIGIBLE" : "DELTA_BELOW_MIN_HEDGE";
  if (std::abs(inventory.delta) > config.hedge_delta_limit) {
    decision.hedge_status = "HEDGE_DELTA_LIMIT";
  }
  return decision;
}

std::vector<SyntheticFill> LoadSyntheticFills(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read synthetic policy fills: " + path.string());
  }
  std::vector<SyntheticFill> fills;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    SyntheticFill fill;
    fill.fill_id = StringField(line, "fill_id");
    fill.side = StringField(line, "side");
    fill.quantity = DoubleField(line, "quantity");
    fill.fill_price = DoubleField(line, "fill_price");
    fill.fair_value = DoubleField(line, "fair_value");
    fill.mark_price = DoubleField(line, "mark_price");
    fill.fee = DoubleField(line, "fee");
    fill.slippage = DoubleField(line, "slippage");
    fills.push_back(fill);
  }
  return fills;
}

PnlAttribution AttributePnl(const std::vector<SyntheticFill>& fills, double tolerance) {
  if (fills.empty()) {
    throw std::runtime_error("attribution requires at least one fill");
  }
  PnlAttribution result;
  result.fill_count = fills.size();
  result.residual_tolerance = tolerance;
  double cash = 0.0;
  double fair = fills.back().fair_value;
  double mark = fills.back().mark_price;
  for (const auto& fill : fills) {
    if (fill.side == "BUY") {
      cash -= fill.quantity * fill.fill_price;
      result.ending_inventory += fill.quantity;
      result.quoted_spread_capture += fill.quantity * (fill.fair_value - fill.fill_price);
    } else if (fill.side == "SELL") {
      cash += fill.quantity * fill.fill_price;
      result.ending_inventory -= fill.quantity;
      result.quoted_spread_capture += fill.quantity * (fill.fill_price - fill.fair_value);
    } else {
      throw std::runtime_error("synthetic fill side must be BUY or SELL");
    }
    result.fees += fill.fee;
    result.slippage += fill.slippage;
    fair = fill.fair_value;
    mark = fill.mark_price;
  }
  result.inventory_revaluation = result.ending_inventory * (mark - fair);
  result.total_pnl = cash + result.ending_inventory * mark - result.fees - result.slippage;
  result.residual = result.total_pnl - result.quoted_spread_capture - result.inventory_revaluation -
                    result.delta_hedge_pnl - result.vega_explain - result.gamma_explain - result.theta_explain +
                    result.fees + result.slippage;
  result.within_tolerance = std::abs(result.residual) <= tolerance;
  result.status = result.within_tolerance ? "PASS" : "RESIDUAL_BREACH";
  return result;
}

std::string QuoteDecisionJson(const QuoteDecision& decision) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "schema_version", JsonString(decision.schema_version), first);
  AddJsonField(out, "component_id", JsonString(decision.component_id), first);
  AddJsonField(out, "decision_id", JsonString(decision.decision_id), first);
  AddJsonField(out, "status", JsonString(decision.status), first);
  AddJsonField(out, "reason", JsonString(decision.reason), first);
  AddJsonField(out, "policy_model_id", JsonString(decision.policy_model_id), first);
  AddJsonField(out, "queue_model_id", JsonString(decision.queue_model_id), first);
  AddJsonField(out, "latency_model_id", JsonString(decision.latency_model_id), first);
  AddJsonField(out, "surface_model_id", JsonString(decision.surface_model_id), first);
  AddJsonField(out, "surface_id", JsonString(decision.surface_id), first);
  AddJsonField(out, "quote_id", JsonString(decision.quote_id), first);
  AddJsonField(out, "decision_ts_ns", std::to_string(decision.decision_ts_ns), first);
  AddJsonField(out, "surface_age_ns", std::to_string(decision.surface_age_ns), first);
  AddJsonField(out, "execution_age_ns", std::to_string(decision.execution_age_ns), first);
  AddJsonField(out, "selected_quote_ts_ns", std::to_string(decision.selected_quote_ts_ns), first);
  AddJsonField(out, "quote_age_ns", std::to_string(decision.quote_age_ns), first);
  AddJsonField(out, "fair_value_source", JsonString(decision.fair_value_source), first);
  AddJsonField(out, "fair_value", JsonDouble(decision.fair_value), first);
  AddJsonField(out, "fair_bid", JsonDouble(decision.fair_bid), first);
  AddJsonField(out, "fair_ask", JsonDouble(decision.fair_ask), first);
  AddJsonField(out, "inventory_skew", JsonDouble(decision.inventory_skew), first);
  AddJsonField(out, "toxicity_penalty", JsonDouble(decision.toxicity_penalty), first);
  AddJsonField(out, "fill_probability_adjustment", JsonDouble(decision.fill_probability_adjustment), first);
  AddJsonField(out, "bid", JsonOptionalDouble(decision.bid), first);
  AddJsonField(out, "ask", JsonOptionalDouble(decision.ask), first);
  AddJsonField(out, "quantity", std::to_string(decision.quantity), first);
  AddJsonField(out, "hedge_eligible", decision.hedge_eligible ? "true" : "false", first);
  AddJsonField(out, "hedge_status", JsonString(decision.hedge_status), first);
  out << '}';
  return out.str();
}

std::string RiskGateEventJson(const RiskGateEvent& event) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "schema_version", JsonString("vegaflux.canonical_market.v0.1"), first);
  AddJsonField(out, "component_id", JsonString("policy-validation"), first);
  AddJsonField(out, "gate", JsonString(event.gate), first);
  AddJsonField(out, "status", JsonString(event.status), first);
  AddJsonField(out, "reason", JsonString(event.reason), first);
  out << '}';
  return out.str();
}

std::string PnlAttributionJson(const PnlAttribution& attribution) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": " << JsonString(attribution.schema_version) << ",\n";
  out << "  \"component_id\": " << JsonString(attribution.component_id) << ",\n";
  out << "  \"status\": " << JsonString(attribution.status) << ",\n";
  out << "  \"fill_count\": " << attribution.fill_count << ",\n";
  out << "  \"ending_inventory\": " << JsonDouble(attribution.ending_inventory) << ",\n";
  out << "  \"total_pnl\": " << JsonDouble(attribution.total_pnl) << ",\n";
  out << "  \"quoted_spread_capture\": " << JsonDouble(attribution.quoted_spread_capture) << ",\n";
  out << "  \"inventory_revaluation\": " << JsonDouble(attribution.inventory_revaluation) << ",\n";
  out << "  \"delta_hedge_pnl\": " << JsonDouble(attribution.delta_hedge_pnl) << ",\n";
  out << "  \"delta_hedge_status\": \"UNIMPLEMENTED_ZERO_PLACEHOLDER\",\n";
  out << "  \"vega_explain\": " << JsonDouble(attribution.vega_explain) << ",\n";
  out << "  \"vega_explain_status\": \"UNIMPLEMENTED_ZERO_PLACEHOLDER\",\n";
  out << "  \"gamma_explain\": " << JsonDouble(attribution.gamma_explain) << ",\n";
  out << "  \"gamma_explain_status\": \"UNIMPLEMENTED_ZERO_PLACEHOLDER\",\n";
  out << "  \"theta_explain\": " << JsonDouble(attribution.theta_explain) << ",\n";
  out << "  \"theta_explain_status\": \"UNIMPLEMENTED_ZERO_PLACEHOLDER\",\n";
  out << "  \"fees\": " << JsonDouble(attribution.fees) << ",\n";
  out << "  \"slippage\": " << JsonDouble(attribution.slippage) << ",\n";
  out << "  \"residual\": " << JsonDouble(attribution.residual) << ",\n";
  out << "  \"residual_tolerance\": " << JsonDouble(attribution.residual_tolerance) << ",\n";
  out << "  \"within_tolerance\": " << (attribution.within_tolerance ? "true" : "false") << ",\n";
  out << "  \"formula\": \"total_pnl = quoted_spread_capture + inventory_revaluation + delta_hedge_pnl + vega_explain + gamma_explain + theta_explain - fees - slippage + residual\"\n";
  out << "}\n";
  return out.str();
}

void WriteQuoteDecisionsJsonl(const std::filesystem::path& path, const std::vector<QuoteDecision>& decisions) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write policy decisions: " + path.string());
  }
  for (const auto& decision : decisions) {
    out << QuoteDecisionJson(decision) << '\n';
  }
}

void WriteRiskGateEventsJsonl(const std::filesystem::path& path, const std::vector<RiskGateEvent>& events) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write policy-validation risk gate events: " + path.string());
  }
  for (const auto& event : events) {
    out << RiskGateEventJson(event) << '\n';
  }
}

void WriteAblationArtifacts(const std::filesystem::path& csv_path,
                            const std::filesystem::path& json_path,
                            const PolicyConfig& config,
                            const Inventory& inventory,
                            const SurfaceInput& surface,
                            const QuoteInput& quote,
                            const ExecutionInput& execution) {
  if (!csv_path.parent_path().empty()) {
    std::filesystem::create_directories(csv_path.parent_path());
  }
  if (!json_path.parent_path().empty()) {
    std::filesystem::create_directories(json_path.parent_path());
  }

  struct Row {
    std::string id;
    std::string components;
    QuoteDecision decision;
  };

  std::vector<Row> rows;
  auto with_model = [&](std::string id, std::string components, std::string model, double fill_adjustment) {
    auto cfg = config;
    cfg.policy_model_id = std::move(model);
    cfg.fill_probability_adjustment = fill_adjustment;
    cfg.toxicity_penalty = fill_adjustment > 0.0 ? config.toxicity_penalty : 0.0;
    rows.push_back({std::move(id), std::move(components), MakeQuoteDecision(cfg, inventory, surface, quote, execution)});
  };

  with_model("passive_fixed_spread", "fixed_spread", "passive_fixed_spread_v1", 0.0);
  with_model("fair_value_only", "fair_value", "fair_value_only_v1", 0.0);
  with_model("inventory_skew", "fair_value+inventory_skew", "inventory_skew_v1", 0.0);
  with_model("inventory_skew_risk_gates", "fair_value+inventory_skew+risk_gates", "inventory_skew_risk_v1", 0.0);
  if (config.fill_probability_adjustment > 0.0) {
    with_model("inventory_skew_fill_adjusted", "fair_value+inventory_skew+risk_gates+toxicity+fill_adjustment",
               "inventory_skew_risk_v1", config.fill_probability_adjustment);
  }

  std::ofstream csv(csv_path, std::ios::binary);
  if (!csv) {
    throw std::runtime_error("failed to write policy-validation ablation table: " + csv_path.string());
  }
  csv << "ablation_id,components,status,fair_value_source,bid,ask,quantity,note\n";
  for (const auto& row : rows) {
    csv << row.id << ',' << row.components << ',' << row.decision.status << ',' << row.decision.fair_value_source << ','
        << JsonDouble(row.decision.bid.value_or(0.0)) << ',' << JsonDouble(row.decision.ask.value_or(0.0)) << ','
        << row.decision.quantity << ",deterministic_component_check_no_performance_or_return_claim\n";
  }

  std::ofstream json(json_path, std::ios::binary);
  if (!json) {
    throw std::runtime_error("failed to write policy-validation ablation source: " + json_path.string());
  }
  json << "{\n";
  json << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  json << "  \"component_id\": \"policy-validation\",\n";
  json << "  \"note\": \"deterministic component ablation source only; no performance or trading-return claim\",\n";
  json << "  \"rows\": [\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    json << "    {\"ablation_id\":" << JsonString(row.id) << ",\"components\":" << JsonString(row.components)
         << ",\"decision\":" << QuoteDecisionJson(row.decision) << "}";
    json << (i + 1 == rows.size() ? "\n" : ",\n");
  }
  json << "  ]\n";
  json << "}\n";
}

void WritePnlAttribution(const std::filesystem::path& path, const PnlAttribution& attribution) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write policy attribution: " + path.string());
  }
  out << PnlAttributionJson(attribution);
}

}  // namespace vf_policy
