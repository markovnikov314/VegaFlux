#include "vf_features/features.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

vf_features::Dataset Build(const std::filesystem::path& root) {
  vf_features::FeatureConfig config;
  config.input_path = root / "artifacts/decoder/decoded.normalized.jsonl";
  config.depth = 5;
  config.label_horizon_events = 1;
  return vf_features::BuildDataset(config);
}

vf_features::Dataset BuildValidation(const std::filesystem::path& root) {
  vf_features::FeatureConfig config;
  config.input_path = root / "artifacts/features-validation/source.normalized.jsonl";
  config.fill_labels_path = root / "artifacts/execution/fills.jsonl";
  config.depth = 5;
  config.label_horizon_events = 1;
  config.feature_window_events = 3;
  config.dataset_id = "synthetic_features_validation_v1";
  return vf_features::BuildDataset(config);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string FeaturesText(const std::vector<vf_features::FeatureRow>& rows) {
  std::ostringstream out;
  for (const auto& row : rows) {
    out << vf_features::FeatureRowJson(row) << '\n';
  }
  return out.str();
}

std::string NormalizeMetadataPaths(std::string text, const std::filesystem::path& root) {
  const auto prefix = root.generic_string() + "/";
  std::size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    text.replace(pos, prefix.size(), "./");
    pos += 2;
  }
  return text;
}

void CheckDeterministicRows(const std::filesystem::path& root) {
  const auto first = Build(root);
  const auto second = Build(root);
  Require(first.rows.size() == 5, "expected five features rows");
  Require(first.rows.size() == second.rows.size(), "row count changed across builds");
  for (std::size_t i = 0; i < first.rows.size(); ++i) {
    Require(vf_features::FeatureRowJson(first.rows[i]) == vf_features::FeatureRowJson(second.rows[i]),
            "feature row changed across builds");
  }
}

void CheckLineageAndFeatures(const std::filesystem::path& root) {
  const auto dataset = Build(root);
  const auto& row = dataset.rows[3];
  Require(row.available_at_ns == row.receive_ts_ns, "available_at_ns must equal receive timestamp");
  Require(row.source_sequence_number == row.sequence_number, "source sequence lineage mismatch");
  Require(row.feature_source_sequence_number == row.sequence_number, "feature source sequence lineage mismatch");
  Require(row.feature_max_source_available_at_ns == row.available_at_ns, "feature source time lineage mismatch");
  Require(row.snapshot_state_checksum != 0, "missing snapshot checksum lineage");
  Require(row.replay_checksum == dataset.replay_checksum, "missing replay checksum lineage");
  Require(row.best_bid_ticks == 10000, "best bid mismatch");
  Require(row.best_ask_ticks == 10005, "best ask mismatch");
  Require(row.spread_ticks == 5, "spread mismatch");
  Require(row.mid_ticks_x2 == 20005, "mid x2 mismatch");
  Require(row.bid_qty_top1 == 6, "bid top1 quantity mismatch");
  Require(row.ask_qty_top1 == 10, "ask top1 quantity mismatch");
  Require(row.depth_imbalance_x1e6 == -250000, "depth imbalance mismatch");
}

void CheckRicherStructuralFeatures(const std::filesystem::path& root) {
  const auto dataset = Build(root);
  const auto& row = dataset.rows[1];
  Require(row.side == "ASK", "side field mismatch");
  Require(row.microprice_ticks_x2 == 20004, "microprice mismatch");
  Require(row.order_flow_imbalance_qty == -12, "order-flow imbalance mismatch");
  Require(row.event_rate_per_second_x1e6 == 2'000'000'000'000LL, "event rate mismatch");
  Require(row.quote_age_ns == 0, "quote age mismatch");
  Require(row.short_window_realized_volatility_x1e6 == std::nullopt, "early realized volatility should be null");

  const auto& later = dataset.rows[2];
  Require(later.quote_age_ns == 1000, "quote age did not advance");
  Require(later.short_window_realized_volatility_x1e6 == 0, "flat-mid realized volatility mismatch");
}

void CheckLabelsAndSplits(const std::filesystem::path& root) {
  const auto dataset = Build(root);
  Require(dataset.rows[1].future_mid_move_ticks_x2 == 0, "sequence 2 label mismatch");
  Require(dataset.rows[1].realized_spread_proxy_ticks_x2 == 5, "realized spread proxy mismatch");
  Require(dataset.rows[1].adverse_selection_proxy_ticks_x2 == 0, "adverse selection proxy mismatch");
  Require(dataset.rows[2].label_status == "purged_split_boundary", "sequence 3 split purge missing");
  Require(dataset.rows[3].label_status == "purged_split_boundary", "sequence 4 split purge missing");
  Require(dataset.rows[4].label_status == "insufficient_future", "last row label status mismatch");
  Require(dataset.rows[0].split == "train", "train split missing");
  Require(dataset.rows[3].split == "validation", "validation split missing");
  Require(dataset.rows[4].split == "test", "test split missing");
}

void CheckLeakageGuard(const std::filesystem::path& root) {
  auto dataset = Build(root);
  Require(vf_features::PassesLeakageGuard(dataset.rows), "safe rows failed leakage guard");
  dataset.rows[0].available_at_ns = dataset.rows[1].receive_ts_ns;
  Require(!vf_features::PassesLeakageGuard(dataset.rows), "future availability was not rejected");
  dataset = Build(root);
  dataset.rows[1].microprice_ticks_x2 = dataset.rows[4].mid_ticks_x2;
  dataset.rows[1].feature_source_sequence_number = dataset.rows[4].sequence_number;
  dataset.rows[1].feature_max_source_available_at_ns = dataset.rows[4].available_at_ns;
  Require(!vf_features::PassesLeakageGuard(dataset.rows), "future-derived feature source was not rejected");

  vf_features::FeatureConfig bad;
  bad.input_path = root / "artifacts/decoder/decoded.normalized.jsonl";
  bad.label_horizon_events = 0;
  bool rejected = false;
  try {
    (void)vf_features::BuildDataset(bad);
  } catch (const std::exception&) {
    rejected = true;
  }
  Require(rejected, "zero-horizon label was not rejected");
}

void CheckVf3rArtifactRegression(const std::filesystem::path& root) {
  const auto dataset = BuildValidation(root);
  Require(dataset.rows.size() == 10, "expected ten features-validation rows");
  const auto labeled = std::count_if(dataset.rows.begin(), dataset.rows.end(), [](const auto& row) {
    return row.future_mid_move_ticks_x2.has_value();
  });
  Require(labeled > 1, "features-validation fixture should have more than one future-mid label");
  Require(dataset.rows[1].fill_within_horizon_proxy == 0, "execution fill proxy was not joined");
  const auto metadata = vf_features::DatasetMetadataJson(dataset);
  Require(metadata.find("\"row_range\"") != std::string::npos, "metadata missing row ranges");
  Require(metadata.find("\"sequence_range\"") != std::string::npos, "metadata missing sequence ranges");
  Require(metadata.find("\"available_at_range_ns\"") != std::string::npos, "metadata missing time ranges");
  Require(metadata.find("\"purged_horizon_count\"") != std::string::npos, "metadata missing purge counts");
  Require(FeaturesText(dataset.rows) == ReadFile(root / "artifacts/features-validation/features.jsonl"),
          "features-validation feature artifact drifted from builder output");
  Require(NormalizeMetadataPaths(metadata, root) == ReadFile(root / "artifacts/features-validation/dataset_metadata.json"),
          "features-validation metadata artifact drifted from builder output");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::filesystem::path root = argc > 1 ? argv[1] : ".";
    CheckDeterministicRows(root);
    CheckLineageAndFeatures(root);
    CheckRicherStructuralFeatures(root);
    CheckLabelsAndSplits(root);
    CheckLeakageGuard(root);
    CheckVf3rArtifactRegression(root);
    std::cout << "feature_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
