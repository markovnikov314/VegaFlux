#include "vf_policy/policy.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

void RequireNear(double actual, double expected, double tolerance, const std::string& message) {
  if (std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
  }
}

vf_policy::PolicyConfig Config() {
  vf_policy::PolicyConfig config;
  config.decision_id = "policy-test";
  config.policy_model_id = "inventory_skew_v1";
  config.surface_id = "clean_smile_v1";
  config.quote_id = "clean_1p0_100";
  config.decision_ts_ns = 1'050;
  config.execution_reference_ts_ns = 10'000;
  config.quote_quantity = 2;
  config.half_spread = 1.0;
  config.inventory_skew_per_delta = 0.5;
  config.min_hedge_delta = 1.0;
  config.hedge_delta_limit = 10.0;
  config.pnl_residual_tolerance = 1e-9;
  config.risk.max_abs_delta = 10.0;
  config.risk.max_abs_gamma = 5.0;
  config.risk.max_abs_vega = 100.0;
  config.risk.max_notional = 1'000.0;
  config.risk.max_surface_age_ns = 100;
  config.risk.max_execution_age_ns = 5;
  config.risk.max_quote_age_ns = 100;
  return config;
}

vf_policy::Inventory Inventory(double delta = 2.0) {
  vf_policy::Inventory inventory;
  inventory.delta = delta;
  inventory.gamma = 1.0;
  inventory.vega = 20.0;
  inventory.notional = 200.0;
  return inventory;
}

vf_policy::SurfaceInput Surface(const std::string& status = "PASS") {
  vf_policy::SurfaceInput surface;
  surface.present = true;
  surface.surface_id = "clean_smile_v1";
  surface.surface_model_id = "quadratic_total_variance_v1";
  surface.status = status;
  surface.reason = status == "PASS" ? "fit_ok" : "static_arb";
  surface.evaluation_ts_ns = 1'000;
  surface.max_quote_age_ns = 200;
  return surface;
}

vf_policy::QuoteInput Quote() {
  vf_policy::QuoteInput quote;
  quote.present = true;
  quote.surface_id = "clean_smile_v1";
  quote.quote_id = "clean_1p0_100";
  quote.status = "ACCEPTED";
  quote.quote_ts_ns = 1'000;
  quote.mid = 100.0;
  return quote;
}

vf_policy::ExecutionInput Execution() {
  vf_policy::ExecutionInput execution;
  execution.present = true;
  execution.safe = true;
  execution.status = "UNFILLED_CENSORED";
  execution.reason = "execution_input_ok";
  execution.queue_model_id = "visible_fifo_v1";
  execution.latency_model_id = "fixed_latency_ns_v1";
  execution.decision_ts_ns = 10'000;
  execution.effective_arrival_ts_ns = 10'250;
  return execution;
}

void CheckQuoteAndSkewDirection() {
  const auto decision = vf_policy::MakeQuoteDecision(Config(), Inventory(2.0), Surface(), Quote(), Execution());
  Require(decision.status == "QUOTE", "quote should pass");
  Require(decision.policy_model_id == "inventory_skew_v1", "missing policy model id");
  Require(decision.queue_model_id == "visible_fifo_v1", "missing queue model id");
  Require(decision.latency_model_id == "fixed_latency_ns_v1", "missing latency model id");
  Require(decision.surface_model_id == "quadratic_total_variance_v1", "missing surface model id");
  RequireNear(decision.fair_bid, 99.0, 1e-12, "fair bid mismatch");
  RequireNear(decision.fair_ask, 101.0, 1e-12, "fair ask mismatch");
  RequireNear(*decision.bid, 98.0, 1e-12, "positive inventory should lower bid");
  RequireNear(*decision.ask, 100.0, 1e-12, "positive inventory should lower ask");

  const auto short_decision = vf_policy::MakeQuoteDecision(Config(), Inventory(-2.0), Surface(), Quote(), Execution());
  RequireNear(*short_decision.bid, 100.0, 1e-12, "negative inventory should raise bid");
  RequireNear(*short_decision.ask, 102.0, 1e-12, "negative inventory should raise ask");

  auto adjusted_config = Config();
  adjusted_config.policy_model_id = "inventory_skew_risk_v1";
  adjusted_config.toxicity_penalty = 0.25;
  adjusted_config.fill_probability_adjustment = 0.5;
  const auto adjusted = vf_policy::MakeQuoteDecision(adjusted_config, Inventory(0.0), Surface(), Quote(), Execution());
  RequireNear(*adjusted.bid, 98.25, 1e-12, "toxicity/fill adjustment should lower bid");
  RequireNear(*adjusted.ask, 101.75, 1e-12, "toxicity/fill adjustment should raise ask");
}

void CheckGates() {
  auto bad_policy = Config();
  bad_policy.policy_model_id = "fixed_logic_mislabel";
  Require(vf_policy::MakeQuoteDecision(bad_policy, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_BAD_POLICY_MODEL",
          "bad policy model should block");
  Require(vf_policy::MakeQuoteDecision(Config(), Inventory(), Surface("STATIC_ARBITRAGE_VIOLATION"), Quote(),
                                       Execution())
              .status == "NO_QUOTE_SURFACE_STATUS",
          "non-PASS surface should block");
  Require(vf_policy::MakeQuoteDecision(Config(), Inventory(), Surface(), Quote(), vf_policy::ExecutionInput{})
              .status == "NO_QUOTE_EXECUTION_MISSING",
          "missing execution should block");
  auto unsafe = Execution();
  unsafe.safe = false;
  unsafe.reason = "unsafe";
  Require(vf_policy::MakeQuoteDecision(Config(), Inventory(), Surface(), Quote(), unsafe).status ==
              "NO_QUOTE_EXECUTION_UNSAFE",
          "unsafe execution should block");
  auto stale_config = Config();
  stale_config.execution_reference_ts_ns = 20'000;
  Require(vf_policy::MakeQuoteDecision(stale_config, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_EXECUTION_STALE",
          "stale execution should block");
  stale_config = Config();
  stale_config.decision_ts_ns = 1'500;
  stale_config.risk.max_quote_age_ns = 1'000;
  Require(vf_policy::MakeQuoteDecision(stale_config, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_SURFACE_STALE",
          "stale surface should block");
  stale_config = Config();
  stale_config.risk.max_quote_age_ns = 10;
  Require(vf_policy::MakeQuoteDecision(stale_config, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_QUOTE_STALE",
          "stale quote should block");
  auto quote = Quote();
  quote.quote_ts_ns = 2'000;
  Require(vf_policy::MakeQuoteDecision(Config(), Inventory(), Surface(), quote, Execution()).status ==
              "NO_QUOTE_QUOTE_FUTURE",
          "future quote should block");
  auto feed_gap = Config();
  feed_gap.feed_gap_detected = true;
  Require(vf_policy::MakeQuoteDecision(feed_gap, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_FEED_GAP",
          "feed gap should block");
  auto corrupt = Config();
  corrupt.corrupt_snapshot_detected = true;
  Require(vf_policy::MakeQuoteDecision(corrupt, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_CORRUPT_SNAPSHOT",
          "corrupt snapshot flag should block");
  auto corrupt_execution = Execution();
  corrupt_execution.has_decision_snapshot_state_checksum = true;
  corrupt_execution.decision_snapshot_state_checksum = 0;
  Require(vf_policy::MakeQuoteDecision(Config(), Inventory(), Surface(), Quote(), corrupt_execution).status ==
              "NO_QUOTE_CORRUPT_SNAPSHOT",
          "zero snapshot checksum should block");
}

void CheckRiskGates() {
  auto inventory = Inventory();
  auto config = Config();
  inventory.delta = 11.0;
  Require(vf_policy::MakeQuoteDecision(config, inventory, Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_RISK_DELTA",
          "delta risk should block");
  inventory = Inventory();
  inventory.gamma = 6.0;
  Require(vf_policy::MakeQuoteDecision(config, inventory, Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_RISK_GAMMA",
          "gamma risk should block");
  inventory = Inventory();
  inventory.vega = 101.0;
  Require(vf_policy::MakeQuoteDecision(config, inventory, Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_RISK_VEGA",
          "vega risk should block");
  inventory = Inventory();
  config.risk.max_notional = 10.0;
  Require(vf_policy::MakeQuoteDecision(config, inventory, Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_RISK_NOTIONAL",
          "notional risk should block");
  config = Config();
  config.risk.max_daily_loss = 5.0;
  config.simulated_daily_pnl = -6.0;
  Require(vf_policy::MakeQuoteDecision(config, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_DAILY_LOSS",
          "daily loss risk should block");
  config = Config();
  config.risk.max_drawdown = 2.0;
  config.simulated_peak_pnl = 5.0;
  config.simulated_daily_pnl = 2.0;
  Require(vf_policy::MakeQuoteDecision(config, Inventory(), Surface(), Quote(), Execution()).status ==
              "NO_QUOTE_DRAWDOWN",
          "drawdown risk should block");
}

void CheckConfigValidation() {
  auto config = Config();
  config.quote_quantity = 0;
  bool threw = false;
  try {
    (void)vf_policy::MakeQuoteDecision(config, Inventory(), Surface(), Quote(), Execution());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Require(threw, "non-positive quantity should fail validation");

  config = Config();
  config.half_spread = -0.01;
  threw = false;
  try {
    (void)vf_policy::MakeQuoteDecision(config, Inventory(), Surface(), Quote(), Execution());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Require(threw, "negative half spread should fail validation");
}

void CheckInterpolatedFairValue() {
  auto surface = Surface();
  surface.interpolation_status = "PASS";
  surface.interpolated_fair_value_present = true;
  surface.interpolated_fair_value = 88.0;
  const auto decision = vf_policy::MakeQuoteDecision(Config(), Inventory(0.0), surface, Quote(), Execution());
  Require(decision.fair_value_source == "interpolated_surface", "surface-validation interpolation should be preferred");
  RequireNear(decision.fair_value, 88.0, 1e-12, "interpolated fair value mismatch");
}

void CheckAttribution() {
  const std::vector<vf_policy::SyntheticFill> fills{
      {"buy", "BUY", 2.0, 99.0, 100.0, 103.0, 0.0, 0.0},
      {"sell", "SELL", 1.0, 101.0, 100.0, 103.0, 0.0, 0.0},
  };
  const auto attribution = vf_policy::AttributePnl(fills, 1e-9);
  Require(attribution.status == "PASS", "attribution should pass");
  RequireNear(attribution.quoted_spread_capture, 3.0, 1e-12, "spread capture mismatch");
  RequireNear(attribution.inventory_revaluation, 3.0, 1e-12, "inventory revaluation mismatch");
  RequireNear(attribution.delta_hedge_pnl, 0.0, 1e-12, "delta hedge placeholder mismatch");
  RequireNear(attribution.vega_explain, 0.0, 1e-12, "vega placeholder mismatch");
  RequireNear(attribution.gamma_explain, 0.0, 1e-12, "gamma placeholder mismatch");
  RequireNear(attribution.theta_explain, 0.0, 1e-12, "theta placeholder mismatch");
  RequireNear(attribution.total_pnl, 6.0, 1e-12, "total pnl mismatch");
  RequireNear(attribution.residual, 0.0, 1e-12, "residual mismatch");
}

void CheckSmokeArtifacts(const std::filesystem::path& root) {
  const auto surface = vf_policy::LoadSurfaceInput(root / "artifacts/options/surface_diagnostics.json", "clean_smile_v1");
  const auto quote = vf_policy::LoadQuoteInput(root / "artifacts/options/filtered_quotes.jsonl", "clean_smile_v1",
                                               "clean_1p0_100");
  const auto execution = vf_policy::LoadExecutionInput(root / "artifacts/execution/fills.jsonl");
  auto config = Config();
  config.decision_ts_ns = 1'050'000'000;
  config.execution_reference_ts_ns = 1'700'000'000'000'004'500;
  config.risk.max_surface_age_ns = 200'000'000;
  config.risk.max_execution_age_ns = 1'000;
  config.risk.max_quote_age_ns = 200'000'000;
  config.half_spread = 0.05;
  config.inventory_skew_per_delta = 0.02;
  config.quote_quantity = 1;
  config.risk.max_notional = 1'000.0;
  const auto decision = vf_policy::MakeQuoteDecision(config, Inventory(1.5), surface, quote, execution);
  Require(decision.status == "QUOTE", "real smoke artifacts should produce quote");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    CheckQuoteAndSkewDirection();
    CheckGates();
    CheckRiskGates();
    CheckConfigValidation();
    CheckInterpolatedFairValue();
    CheckAttribution();
    if (argc > 1) {
      CheckSmokeArtifacts(argv[1]);
    }
    std::cout << "policy_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
