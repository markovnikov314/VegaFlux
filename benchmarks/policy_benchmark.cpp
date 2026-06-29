#include "vf_policy/policy.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    out += ch == '\\' ? "\\\\" : ch == '"' ? "\\\"" : std::string(1, ch);
  }
  return out;
}

vf_policy::PolicyConfig Config() {
  vf_policy::PolicyConfig config;
  config.component_id = "policy-validation";
  config.decision_ts_ns = 1'050'000'000;
  config.execution_reference_ts_ns = 100;
  config.policy_model_id = "inventory_skew_risk_v1";
  config.quote_quantity = 1;
  config.half_spread = 0.05;
  config.inventory_skew_per_delta = 0.02;
  config.toxicity_penalty = 0.01;
  config.fill_probability_adjustment = 0.02;
  config.min_hedge_delta = 1.0;
  config.hedge_delta_limit = 10.0;
  config.pnl_residual_tolerance = 1e-9;
  config.risk.max_abs_delta = 10.0;
  config.risk.max_abs_gamma = 1.0;
  config.risk.max_abs_vega = 100.0;
  config.risk.max_notional = 1'000.0;
  config.risk.max_surface_age_ns = 200'000'000;
  config.risk.max_execution_age_ns = 1'000;
  config.risk.max_quote_age_ns = 200'000'000;
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path output;
    int iterations = 11;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--output") {
        output = Arg(i, argc, argv);
      } else if (arg == "--iterations") {
        iterations = std::stoi(Arg(i, argc, argv));
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (output.empty()) {
      throw std::runtime_error("--output is required");
    }

    const auto surface_path = std::filesystem::exists(Resolve(root, "artifacts/surface-validation/surface_diagnostics.json"))
                                  ? "artifacts/surface-validation/surface_diagnostics.json"
                                  : "artifacts/options/surface_diagnostics.json";
    const auto quotes_path = std::filesystem::exists(Resolve(root, "artifacts/surface-validation/filtered_quotes.jsonl"))
                                 ? "artifacts/surface-validation/filtered_quotes.jsonl"
                                 : "artifacts/options/filtered_quotes.jsonl";
    const auto execution_path = std::filesystem::exists(Resolve(root, "artifacts/execution-sensitivity/deterministic_fills.jsonl"))
                                    ? "artifacts/execution-sensitivity/deterministic_fills.jsonl"
                                    : "artifacts/execution/fills.jsonl";
    const auto surface = vf_policy::LoadSurfaceInput(Resolve(root, surface_path), "clean_smile_v1");
    const auto quote = vf_policy::LoadQuoteInput(Resolve(root, quotes_path), "clean_smile_v1", "clean_1p0_100");
    const auto execution = vf_policy::LoadExecutionInput(Resolve(root, execution_path));
    const auto fills = vf_policy::LoadSyntheticFills(Resolve(root, "data_contracts/fixtures/synthetic_policy.jsonl"));
    const auto config = Config();
    vf_policy::Inventory inventory;
    inventory.delta = 1.5;
    inventory.gamma = 0.02;
    inventory.vega = 5.0;
    inventory.notional = 11.2;

    std::vector<long long> durations;
    durations.reserve(static_cast<std::size_t>(iterations));
    vf_policy::QuoteDecision decision;
    vf_policy::PnlAttribution attribution;
    for (int i = 0; i < iterations; ++i) {
      const auto start = std::chrono::steady_clock::now();
      decision = vf_policy::MakeQuoteDecision(config, inventory, surface, quote, execution);
      attribution = vf_policy::AttributePnl(fills, config.pnl_residual_tolerance);
      const auto end = std::chrono::steady_clock::now();
      durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(durations.begin(), durations.end());

    const auto output_path = Resolve(root, output);
    if (!output_path.parent_path().empty()) {
      std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream out(output_path, std::ios::binary);
    out << "{\n";
    out << "  \"benchmark_id\": \"policy_smoke_cpp\",\n";
    out << "  \"iterations\": " << iterations << ",\n";
    out << "  \"median_policy_attribution_ns\": " << durations[durations.size() / 2] << ",\n";
    out << "  \"min_policy_attribution_ns\": " << durations.front() << ",\n";
    out << "  \"note\": \"smoke wiring only; not a performance claim\",\n";
    out << "  \"policy_model_id\": \"" << Escape(decision.policy_model_id) << "\",\n";
    out << "  \"queue_model_id\": \"" << Escape(decision.queue_model_id) << "\",\n";
    out << "  \"latency_model_id\": \"" << Escape(decision.latency_model_id) << "\",\n";
    out << "  \"surface_model_id\": \"" << Escape(decision.surface_model_id) << "\",\n";
    out << "  \"fair_value_source\": \"" << Escape(decision.fair_value_source) << "\",\n";
    out << "  \"decision_status\": \"" << Escape(decision.status) << "\",\n";
    out << "  \"attribution_status\": \"" << Escape(attribution.status) << "\",\n";
    out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
    out << "  \"status\": \"" << (decision.status == "QUOTE" && attribution.within_tolerance ? "pass" : "fail")
        << "\"\n";
    out << "}\n";
    std::cout << "{\"output\":\"" << Escape(output_path.string()) << "\",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
