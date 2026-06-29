#include "vf_execution/execution.hpp"

#include "aegis_lob/book.hpp"
#include "aegis_replay/replay.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Manifest {
  std::filesystem::path input;
  std::filesystem::path features;
  std::filesystem::path fills_output;
  std::filesystem::path metadata_output;
  std::optional<std::filesystem::path> scenario_sweep_output;
  std::optional<std::filesystem::path> scenario_sweep_csv_output;
  std::optional<std::filesystem::path> examples_output;
  std::optional<std::filesystem::path> calibration_status_output;
  vf_execution::SimulationConfig config;
  vf_execution::SimOrder order;
};

struct ScenarioRow {
  std::string scenario_id;
  vf_execution::FillResult result;
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

bool HasKey(const std::string& text, const std::string& key) {
  return text.find("\"" + key + "\":") != std::string::npos;
}

void SkipSpaces(const std::string& text, std::size_t& pos) {
  while (pos < text.size() && text[pos] == ' ') {
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

std::optional<std::string> OptionalStringField(const std::string& text, const std::string& key) {
  if (!HasKey(text, key)) {
    return std::nullopt;
  }
  return StringField(text, key);
}

std::int64_t IntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

std::uint64_t UintField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoull(text.substr(pos, end - pos));
}

double DoubleField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stod(text.substr(pos, end - pos));
}

std::optional<double> OptionalDoubleField(const std::string& text, const std::string& key) {
  if (!HasKey(text, key)) {
    return std::nullopt;
  }
  return DoubleField(text, key);
}

std::optional<std::filesystem::path> OptionalPathField(const std::string& text, const std::string& key) {
  const auto value = OptionalStringField(text, key);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::filesystem::path(*value);
}

Manifest ReadManifest(const std::filesystem::path& path) {
  const auto text = ReadFile(path);
  Manifest manifest;
  manifest.input = StringField(text, "input");
  manifest.features = StringField(text, "features");
  manifest.fills_output = StringField(text, "fills_output");
  manifest.metadata_output = StringField(text, "metadata_output");
  manifest.config.component_id = OptionalStringField(text, "component").value_or(manifest.config.component_id);
  manifest.config.simulation_id = StringField(text, "simulation_id");
  manifest.config.queue_model_id = StringField(text, "queue_model_id");
  manifest.config.latency_model_id = StringField(text, "latency_model_id");
  manifest.config.fixed_latency_ns = IntField(text, "fixed_latency_ns");
  manifest.config.horizon_ns = IntField(text, "horizon_ns");
  manifest.config.markout_horizon_ns = IntField(text, "markout_horizon_ns");
  manifest.config.depth = static_cast<std::uint32_t>(UintField(text, "depth"));
  manifest.config.hidden_ahead_multiplier =
      OptionalDoubleField(text, "hidden_ahead_multiplier").value_or(manifest.config.hidden_ahead_multiplier);
  manifest.config.latency_multiplier =
      OptionalDoubleField(text, "latency_multiplier").value_or(manifest.config.latency_multiplier);
  manifest.config.cancel_rate_multiplier =
      OptionalDoubleField(text, "cancel_rate_multiplier").value_or(manifest.config.cancel_rate_multiplier);
  manifest.order.order_id = StringField(text, "order_id");
  manifest.order.side = vf_execution::ParseSide(StringField(text, "side"));
  manifest.order.price_ticks = IntField(text, "price_ticks");
  manifest.order.quantity = IntField(text, "quantity");
  manifest.order.decision_sequence_number = UintField(text, "decision_sequence_number");
  manifest.scenario_sweep_output = OptionalPathField(text, "scenario_sweep_output");
  manifest.scenario_sweep_csv_output = OptionalPathField(text, "scenario_sweep_csv_output");
  manifest.examples_output = OptionalPathField(text, "examples_output");
  manifest.calibration_status_output = OptionalPathField(text, "calibration_status_output");
  return manifest;
}

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    out += ch == '\\' ? "\\\\" : ch == '"' ? "\\\"" : std::string(1, ch);
  }
  return out;
}

std::string JsonInt(std::optional<std::int64_t> value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string CsvInt(std::optional<std::int64_t> value) {
  return value.has_value() ? std::to_string(*value) : "";
}

void EnsureParent(const std::filesystem::path& path) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
}

aegis_lob::CanonicalEvent Event(std::uint64_t seq,
                                std::int64_t receive_ts_ns,
                                const std::string& type,
                                aegis_lob::Side side,
                                std::int64_t price,
                                std::int64_t qty,
                                std::uint64_t order_id) {
  const auto reference_order_id = type == "ADD" ? 0 : order_id;
  return {"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-EXEC", seq, receive_ts_ns - 10, receive_ts_ns,
          "VGFX", type, side, price, qty, order_id, reference_order_id};
}

std::vector<aegis_lob::CanonicalEvent> SyntheticScenarioEvents() {
  return {
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 10, 1),
      Event(2, 160, "CANCEL", aegis_lob::Side::kBid, 100, 4, 1),
      Event(3, 170, "ADD", aegis_lob::Side::kBid, 100, 3, 2),
      Event(4, 220, "EXECUTE", aegis_lob::Side::kBid, 100, 6, 1),
      Event(5, 320, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 2),
  };
}

std::vector<vf_execution::FeatureRow> SyntheticScenarioFeatures(const std::vector<aegis_lob::CanonicalEvent>& events) {
  aegis_replay::ReplayCursor cursor(events);
  std::vector<vf_execution::FeatureRow> rows;
  while (cursor.has_next()) {
    const auto& event = cursor.next_event();
    if (event.sequence_number != 1 && event.sequence_number != 5) {
      continue;
    }
    vf_execution::FeatureRow row;
    row.schema_version = "vegaflux.canonical_market.v0.1";
    row.dataset_id = "execution_sensitivity_truth_v1";
    row.row_index = rows.size();
    row.sequence_number = event.sequence_number;
    row.source_sequence_number = event.sequence_number;
    row.available_at_ns = event.sequence_number == 1 ? 100 : 330;
    row.snapshot_state_checksum = cursor.state_checksum();
    row.mid_ticks_x2 = event.sequence_number == 1 ? 200 : 204;
    rows.push_back(row);
  }
  const auto replay_checksum = cursor.state_checksum();
  for (auto& row : rows) {
    row.replay_checksum = replay_checksum;
  }
  return rows;
}

ScenarioRow RunSyntheticScenario(std::string scenario_id,
                                 double hidden_ahead_multiplier,
                                 double latency_multiplier,
                                 double cancel_rate_multiplier) {
  const auto events = SyntheticScenarioEvents();
  const auto features = SyntheticScenarioFeatures(events);
  vf_execution::SimulationConfig config;
  config.component_id = "execution-sensitivity";
  config.simulation_id = scenario_id;
  config.fixed_latency_ns = 50;
  config.horizon_ns = 1'000;
  config.markout_horizon_ns = 10;
  config.depth = 5;
  config.hidden_ahead_multiplier = hidden_ahead_multiplier;
  config.latency_multiplier = latency_multiplier;
  config.cancel_rate_multiplier = cancel_rate_multiplier;
  vf_execution::SimOrder order{"sensitivity-synthetic-buy-1", vf_execution::Side::kBuy, 100, 3, 1};
  return {scenario_id, vf_execution::SimulateVisibleFifo(config, order, events, features)};
}

std::vector<ScenarioRow> SyntheticScenarioRows() {
  return {
      RunSyntheticScenario("baseline", 0.0, 1.0, 1.0),
      RunSyntheticScenario("hidden_ahead_x1", 1.0, 1.0, 1.0),
      RunSyntheticScenario("latency_x4", 0.0, 4.0, 1.0),
      RunSyntheticScenario("cancel_rate_x0_5", 0.0, 1.0, 0.5),
  };
}

void WriteScenarioSweepJson(const std::filesystem::path& path, const std::vector<ScenarioRow>& rows) {
  EnsureParent(path);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write scenario sweep: " + path.string());
  }
  out << "{\n";
  out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  out << "  \"component_id\": \"execution-sensitivity\",\n";
  out << "  \"source\": \"synthetic analytical fixture\",\n";
  out << "  \"scenario_count\": " << rows.size() << ",\n";
  out << "  \"scenarios\": [\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    out << "    {\"scenario_id\":\"" << Escape(row.scenario_id) << "\",\"queue_model_id\":\""
        << Escape(row.result.queue_model_id) << "\",\"latency_model_id\":\"" << Escape(row.result.latency_model_id)
        << "\",\"hidden_ahead_multiplier\":" << row.result.hidden_ahead_multiplier << ",\"latency_multiplier\":"
        << row.result.latency_multiplier << ",\"cancel_rate_multiplier\":" << row.result.cancel_rate_multiplier
        << ",\"fill_status\":\"" << Escape(row.result.status) << "\",\"filled_quantity\":"
        << row.result.filled_quantity << ",\"markout_ticks_x2\":" << JsonInt(row.result.markout_ticks_x2) << "}";
    out << (i + 1 == rows.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
}

void WriteScenarioSweepCsv(const std::filesystem::path& path, const std::vector<ScenarioRow>& rows) {
  EnsureParent(path);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write scenario sweep CSV: " + path.string());
  }
  out << "scenario_id,queue_model_id,latency_model_id,hidden_ahead_multiplier,latency_multiplier,"
         "cancel_rate_multiplier,fill_status,filled_quantity,markout_ticks_x2\n";
  for (const auto& row : rows) {
    out << row.scenario_id << ',' << row.result.queue_model_id << ',' << row.result.latency_model_id << ','
        << row.result.hidden_ahead_multiplier << ',' << row.result.latency_multiplier << ','
        << row.result.cancel_rate_multiplier << ',' << row.result.status << ',' << row.result.filled_quantity << ','
        << CsvInt(row.result.markout_ticks_x2) << '\n';
  }
}

void WriteDeterministicExamples(const std::filesystem::path& path, const std::vector<ScenarioRow>& rows) {
  EnsureParent(path);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write deterministic examples: " + path.string());
  }
  for (const auto& row : rows) {
    out << vf_execution::FillResultJson(row.result) << '\n';
  }
}

void WriteCalibrationStatus(const std::filesystem::path& path, const std::vector<ScenarioRow>& rows) {
  EnsureParent(path);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write calibration status: " + path.string());
  }
  out << "scenario_id,synthetic_truth_status,observed_status,synthetic_truth_filled_quantity,"
         "observed_filled_quantity,synthetic_truth_markout_ticks_x2,observed_markout_ticks_x2,result\n";
  for (const auto& row : rows) {
    std::string expected_status;
    std::int64_t expected_quantity = 0;
    std::optional<std::int64_t> expected_markout;
    if (row.scenario_id == "baseline") {
      expected_status = "FULL";
      expected_quantity = 3;
      expected_markout = 4;
    } else if (row.scenario_id == "cancel_rate_x0_5") {
      expected_status = "PARTIAL_CENSORED";
      expected_quantity = 1;
      expected_markout = 4;
    } else {
      expected_status = "UNFILLED_CENSORED";
      expected_quantity = 0;
    }
    const bool pass = row.result.status == expected_status && row.result.filled_quantity == expected_quantity &&
                      row.result.markout_ticks_x2 == expected_markout;
    out << row.scenario_id << ',' << expected_status << ',' << row.result.status << ',' << expected_quantity << ','
        << row.result.filled_quantity << ',' << CsvInt(expected_markout) << ',' << CsvInt(row.result.markout_ticks_x2)
        << ',' << (pass ? "pass" : "fail") << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> fills_override;
    std::optional<std::filesystem::path> metadata_override;
    std::optional<std::filesystem::path> scenario_sweep_override;
    std::optional<std::filesystem::path> scenario_sweep_csv_override;
    std::optional<std::filesystem::path> examples_override;
    std::optional<std::filesystem::path> calibration_status_override;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--manifest") {
        manifest_path = Arg(i, argc, argv);
      } else if (arg == "--fills-output") {
        fills_override = Arg(i, argc, argv);
      } else if (arg == "--metadata-output") {
        metadata_override = Arg(i, argc, argv);
      } else if (arg == "--scenario-sweep-output") {
        scenario_sweep_override = Arg(i, argc, argv);
      } else if (arg == "--scenario-sweep-csv-output") {
        scenario_sweep_csv_override = Arg(i, argc, argv);
      } else if (arg == "--examples-output") {
        examples_override = Arg(i, argc, argv);
      } else if (arg == "--calibration-status-output") {
        calibration_status_override = Arg(i, argc, argv);
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (!manifest_path.has_value()) {
      throw std::runtime_error("--manifest is required");
    }

    auto manifest = ReadManifest(Resolve(root, *manifest_path));
    if (fills_override.has_value()) {
      manifest.fills_output = *fills_override;
    }
    if (metadata_override.has_value()) {
      manifest.metadata_output = *metadata_override;
    }
    if (scenario_sweep_override.has_value()) {
      manifest.scenario_sweep_output = *scenario_sweep_override;
    }
    if (scenario_sweep_csv_override.has_value()) {
      manifest.scenario_sweep_csv_output = *scenario_sweep_csv_override;
    }
    if (examples_override.has_value()) {
      manifest.examples_output = *examples_override;
    }
    if (calibration_status_override.has_value()) {
      manifest.calibration_status_output = *calibration_status_override;
    }

    const auto input = Resolve(root, manifest.input);
    const auto features_path = Resolve(root, manifest.features);
    const auto fills_output = Resolve(root, manifest.fills_output);
    const auto metadata_output = Resolve(root, manifest.metadata_output);
    const auto events = aegis_lob::LoadCanonicalJsonl(input);
    const auto features = vf_execution::LoadFeatureRows(features_path);
    const auto result = vf_execution::SimulateVisibleFifo(manifest.config, manifest.order, events, features);
    vf_execution::WriteFillsJsonl(fills_output, {result});
    vf_execution::WriteSimulationMetadata(metadata_output, manifest.config, input, features_path, 1,
                                          result.source_replay_checksum);
    const auto scenario_rows = SyntheticScenarioRows();
    if (manifest.scenario_sweep_output.has_value()) {
      WriteScenarioSweepJson(Resolve(root, *manifest.scenario_sweep_output), scenario_rows);
    }
    if (manifest.scenario_sweep_csv_output.has_value()) {
      WriteScenarioSweepCsv(Resolve(root, *manifest.scenario_sweep_csv_output), scenario_rows);
    }
    if (manifest.examples_output.has_value()) {
      WriteDeterministicExamples(Resolve(root, *manifest.examples_output), scenario_rows);
    }
    if (manifest.calibration_status_output.has_value()) {
      WriteCalibrationStatus(Resolve(root, *manifest.calibration_status_output), scenario_rows);
    }

    std::cout << "{\"fills_output\":\"" << Escape(fills_output.string()) << "\",\"metadata_output\":\""
              << Escape(metadata_output.string()) << "\",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vf_run_execution_cli: " << error.what() << '\n';
    return 1;
  }
}
