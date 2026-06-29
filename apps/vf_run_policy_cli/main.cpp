#include "vf_policy/policy.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct Manifest {
  std::filesystem::path surface_diagnostics;
  std::filesystem::path filtered_quotes;
  std::filesystem::path execution_fills;
  std::filesystem::path synthetic_policy_fills;
  std::filesystem::path quote_decisions_output;
  std::filesystem::path pnl_attribution_output;
  std::filesystem::path risk_gate_events_output;
  std::filesystem::path ablation_table_output;
  std::filesystem::path ablation_source_output;
  vf_policy::PolicyConfig config;
  vf_policy::Inventory inventory;
};

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read manifest: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t ValuePos(const std::string& text, const std::string& key) {
  const auto needle = "\"" + key + "\":";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing manifest key: " + key);
  }
  return pos + needle.size();
}

bool HasField(const std::string& text, const std::string& key) {
  return text.find("\"" + key + "\":") != std::string::npos;
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
    throw std::runtime_error("expected string manifest field: " + key);
  }
  ++pos;
  std::string out;
  for (; pos < text.size(); ++pos) {
    if (text[pos] == '"') {
      return out;
    }
    out.push_back(text[pos]);
  }
  throw std::runtime_error("unterminated manifest string: " + key);
}

std::string OptionalStringField(const std::string& text, const std::string& key, const std::string& fallback) {
  return HasField(text, key) ? StringField(text, key) : fallback;
}

double DoubleField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stod(text.substr(pos, end - pos));
}

double OptionalDoubleField(const std::string& text, const std::string& key, double fallback) {
  return HasField(text, key) ? DoubleField(text, key) : fallback;
}

std::int64_t IntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

std::int64_t OptionalIntField(const std::string& text, const std::string& key, std::int64_t fallback) {
  return HasField(text, key) ? IntField(text, key) : fallback;
}

bool OptionalBoolField(const std::string& text, const std::string& key, bool fallback) {
  if (!HasField(text, key)) {
    return fallback;
  }
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  if (text.compare(pos, 4, "true") == 0) {
    return true;
  }
  if (text.compare(pos, 5, "false") == 0) {
    return false;
  }
  throw std::runtime_error("expected bool manifest field: " + key);
}

Manifest ReadManifest(const std::filesystem::path& path) {
  const auto text = ReadFile(path);
  Manifest manifest;
  manifest.surface_diagnostics = StringField(text, "surface_diagnostics");
  manifest.filtered_quotes = StringField(text, "filtered_quotes");
  manifest.execution_fills = StringField(text, "execution_fills");
  manifest.synthetic_policy_fills = StringField(text, "synthetic_policy_fills");
  manifest.quote_decisions_output = StringField(text, "quote_decisions_output");
  manifest.pnl_attribution_output = StringField(text, "pnl_attribution_output");
  manifest.risk_gate_events_output = OptionalStringField(text, "risk_gate_events_output", "");
  manifest.ablation_table_output = OptionalStringField(text, "ablation_table_output", "");
  manifest.ablation_source_output = OptionalStringField(text, "ablation_source_output", "");
  manifest.config.component_id = OptionalStringField(text, "component_id", "policy");
  manifest.config.decision_id = StringField(text, "decision_id");
  manifest.config.policy_model_id = StringField(text, "policy_model_id");
  manifest.config.surface_id = StringField(text, "surface_id");
  manifest.config.quote_id = StringField(text, "quote_id");
  manifest.config.decision_ts_ns = IntField(text, "decision_ts_ns");
  manifest.config.execution_reference_ts_ns = IntField(text, "execution_reference_ts_ns");
  manifest.config.quote_quantity = IntField(text, "quote_quantity");
  manifest.config.half_spread = DoubleField(text, "half_spread");
  manifest.config.inventory_skew_per_delta = DoubleField(text, "inventory_skew_per_delta");
  manifest.config.toxicity_penalty = OptionalDoubleField(text, "toxicity_penalty", 0.0);
  manifest.config.fill_probability_adjustment = OptionalDoubleField(text, "fill_probability_adjustment", 0.0);
  manifest.config.simulated_daily_pnl = OptionalDoubleField(text, "simulated_daily_pnl", 0.0);
  manifest.config.simulated_peak_pnl = OptionalDoubleField(text, "simulated_peak_pnl", 0.0);
  manifest.config.feed_gap_detected = OptionalBoolField(text, "feed_gap_detected", false);
  manifest.config.corrupt_snapshot_detected = OptionalBoolField(text, "corrupt_snapshot_detected", false);
  manifest.config.min_hedge_delta = DoubleField(text, "min_hedge_delta");
  manifest.config.hedge_delta_limit = DoubleField(text, "hedge_delta_limit");
  manifest.config.pnl_residual_tolerance = DoubleField(text, "pnl_residual_tolerance");
  manifest.config.risk.max_abs_delta = DoubleField(text, "max_abs_delta");
  manifest.config.risk.max_abs_gamma = DoubleField(text, "max_abs_gamma");
  manifest.config.risk.max_abs_vega = DoubleField(text, "max_abs_vega");
  manifest.config.risk.max_notional = DoubleField(text, "max_notional");
  manifest.config.risk.max_daily_loss = OptionalDoubleField(text, "max_daily_loss", 0.0);
  manifest.config.risk.max_drawdown = OptionalDoubleField(text, "max_drawdown", 0.0);
  manifest.config.risk.max_surface_age_ns = IntField(text, "max_surface_age_ns");
  manifest.config.risk.max_execution_age_ns = IntField(text, "max_execution_age_ns");
  manifest.config.risk.max_quote_age_ns = OptionalIntField(text, "max_quote_age_ns", 0);
  manifest.inventory.delta = DoubleField(text, "inventory_delta");
  manifest.inventory.gamma = DoubleField(text, "inventory_gamma");
  manifest.inventory.vega = DoubleField(text, "inventory_vega");
  manifest.inventory.cash = DoubleField(text, "inventory_cash");
  manifest.inventory.notional = DoubleField(text, "inventory_notional");
  return manifest;
}

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    out += ch == '\\' ? "\\\\" : ch == '"' ? "\\\"" : std::string(1, ch);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> decisions_override;
    std::optional<std::filesystem::path> pnl_override;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--manifest") {
        manifest_path = Arg(i, argc, argv);
      } else if (arg == "--quote-decisions-output") {
        decisions_override = Arg(i, argc, argv);
      } else if (arg == "--pnl-attribution-output") {
        pnl_override = Arg(i, argc, argv);
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (!manifest_path.has_value()) {
      throw std::runtime_error("--manifest is required");
    }

    auto manifest = ReadManifest(Resolve(root, *manifest_path));
    if (decisions_override.has_value()) {
      manifest.quote_decisions_output = *decisions_override;
    }
    if (pnl_override.has_value()) {
      manifest.pnl_attribution_output = *pnl_override;
    }

    const auto surface =
        vf_policy::LoadSurfaceInput(Resolve(root, manifest.surface_diagnostics), manifest.config.surface_id);
    const auto quote =
        vf_policy::LoadQuoteInput(Resolve(root, manifest.filtered_quotes), manifest.config.surface_id,
                                  manifest.config.quote_id);
    const auto execution = vf_policy::LoadExecutionInput(Resolve(root, manifest.execution_fills));
    const auto decision = vf_policy::MakeQuoteDecision(manifest.config, manifest.inventory, surface, quote, execution);
    const auto gates = vf_policy::EvaluateRiskGates(manifest.config, manifest.inventory, surface, quote, execution);
    const auto fills = vf_policy::LoadSyntheticFills(Resolve(root, manifest.synthetic_policy_fills));
    auto attribution = vf_policy::AttributePnl(fills, manifest.config.pnl_residual_tolerance);
    attribution.component_id = manifest.config.component_id;

    const auto decisions_output = Resolve(root, manifest.quote_decisions_output);
    const auto pnl_output = Resolve(root, manifest.pnl_attribution_output);
    vf_policy::WriteQuoteDecisionsJsonl(decisions_output, {decision});
    vf_policy::WritePnlAttribution(pnl_output, attribution);
    if (!manifest.risk_gate_events_output.empty()) {
      vf_policy::WriteRiskGateEventsJsonl(Resolve(root, manifest.risk_gate_events_output), gates);
    }
    if (!manifest.ablation_table_output.empty() && !manifest.ablation_source_output.empty()) {
      vf_policy::WriteAblationArtifacts(Resolve(root, manifest.ablation_table_output),
                                        Resolve(root, manifest.ablation_source_output), manifest.config,
                                        manifest.inventory, surface, quote, execution);
    }

    const bool pass = decision.status == "QUOTE" && attribution.within_tolerance;
    std::cout << "{\"quote_decisions_output\":\"" << Escape(decisions_output.string())
              << "\",\"pnl_attribution_output\":\"" << Escape(pnl_output.string()) << "\",\"status\":\""
              << (pass ? "pass" : "fail") << "\"}\n";
    return pass ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "vf_run_policy_cli: " << error.what() << '\n';
    return 1;
  }
}
