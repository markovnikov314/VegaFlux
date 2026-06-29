#include "vf_features/features.hpp"

#include "aegis_lob/book.hpp"
#include "aegis_replay/replay.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace vf_features {
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

std::string JsonInt(std::optional<std::int64_t> value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string JsonUint(std::optional<std::uint64_t> value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::int64_t SumQty(const std::vector<aegis_lob::PriceLevel>& levels) {
  std::int64_t sum = 0;
  for (const auto& level : levels) {
    sum += level.quantity;
  }
  return sum;
}

std::int64_t OrderFlowImbalance(const aegis_lob::CanonicalEvent& event) {
  if (event.side == aegis_lob::Side::kUnspecified || event.quantity <= 0) {
    return 0;
  }
  const auto same_side_add = event.event_type == "ADD" || event.event_type == "REPLACE" || event.event_type == "MODIFY";
  const auto same_side_reduce =
      event.event_type == "EXECUTE" || event.event_type == "CANCEL" || event.event_type == "DELETE";
  if (!same_side_add && !same_side_reduce) {
    return 0;
  }
  const auto sign = event.side == aegis_lob::Side::kBid ? 1 : -1;
  return (same_side_add ? sign : -sign) * event.quantity;
}

std::string SplitName(std::size_t index, std::size_t rows) {
  if (rows < 3) {
    return "train";
  }
  const auto train = std::max<std::size_t>(1, rows * 3 / 5);
  const auto valid = std::max<std::size_t>(1, rows / 5);
  if (index < train) {
    return "train";
  }
  if (index < train + valid && index + 1 < rows) {
    return "validation";
  }
  return "test";
}

void AddJsonField(std::ostringstream& out, const std::string& name, const std::string& value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << '"' << name << "\":" << value;
}

FeatureRow RowFromSnapshot(const FeatureConfig& config,
                           const aegis_lob::CanonicalEvent& event,
                           const aegis_lob::BookSnapshot& snapshot,
                           std::size_t row_index) {
  FeatureRow row;
  row.schema_version = event.schema_version;
  row.dataset_id = config.dataset_id;
  row.venue = event.venue;
  row.symbol = event.symbol;
  row.session = event.session;
  row.row_index = static_cast<std::uint64_t>(row_index);
  row.sequence_number = event.sequence_number;
  row.source_sequence_number = event.sequence_number;
  row.feature_source_sequence_number = event.sequence_number;
  row.event_type = event.event_type;
  row.side = aegis_lob::SideName(event.side);
  row.event_ts_ns = event.event_ts_ns;
  row.receive_ts_ns = event.receive_ts_ns;
  row.available_at_ns = event.receive_ts_ns;
  row.feature_max_source_available_at_ns = event.receive_ts_ns;
  row.snapshot_state_checksum = snapshot.state_checksum;
  if (!snapshot.bids.empty()) {
    row.best_bid_ticks = snapshot.bids.front().price_ticks;
    row.bid_qty_top1 = snapshot.bids.front().quantity;
  }
  if (!snapshot.asks.empty()) {
    row.best_ask_ticks = snapshot.asks.front().price_ticks;
    row.ask_qty_top1 = snapshot.asks.front().quantity;
  }
  row.depth_bid_qty = SumQty(snapshot.bids);
  row.depth_ask_qty = SumQty(snapshot.asks);
  if (row.best_bid_ticks.has_value() && row.best_ask_ticks.has_value()) {
    row.spread_ticks = *row.best_ask_ticks - *row.best_bid_ticks;
    row.mid_ticks_x2 = *row.best_bid_ticks + *row.best_ask_ticks;
    const auto top_qty = row.bid_qty_top1 + row.ask_qty_top1;
    if (top_qty > 0) {
      row.microprice_ticks_x2 =
          ((*row.best_ask_ticks * row.bid_qty_top1) + (*row.best_bid_ticks * row.ask_qty_top1)) * 2 / top_qty;
    }
  }
  const auto total_depth_qty = row.depth_bid_qty + row.depth_ask_qty;
  if (total_depth_qty > 0) {
    row.depth_imbalance_x1e6 = (row.depth_bid_qty - row.depth_ask_qty) * 1'000'000 / total_depth_qty;
  }
  row.order_flow_imbalance_qty = OrderFlowImbalance(event);
  row.label_status = "pending";
  return row;
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
  while (pos < text.size() && text[pos] == ' ') {
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

std::unordered_map<std::uint64_t, std::int64_t> LoadFillLabels(const std::optional<std::filesystem::path>& path) {
  std::unordered_map<std::uint64_t, std::int64_t> labels;
  if (!path.has_value() || !std::filesystem::exists(*path)) {
    return labels;
  }
  std::ifstream in(*path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read fill labels: " + path->string());
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto seq = UintField(line, "decision_sequence_number");
    const auto filled = IntField(line, "filled_quantity");
    const auto status = StringField(line, "status");
    labels[seq] = (filled > 0 || status == "FULL" || status == "PARTIAL_CENSORED") ? 1 : 0;
  }
  return labels;
}

void AddWindowFeatures(std::vector<FeatureRow>& rows, std::size_t window) {
  if (window == 0) {
    throw std::runtime_error("feature_window_events must be positive");
  }
  std::optional<std::int64_t> quote_started_at;
  std::optional<std::int64_t> previous_bid;
  std::optional<std::int64_t> previous_ask;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto start = i + 1 > window ? i + 1 - window : 0;
    const auto elapsed = rows[i].available_at_ns - rows[start].available_at_ns;
    if (elapsed > 0) {
      const auto count = static_cast<std::int64_t>(i - start + 1);
      rows[i].event_rate_per_second_x1e6 = count * 1'000'000'000'000'000LL / elapsed;
    }

    if (rows[i].best_bid_ticks.has_value() && rows[i].best_ask_ticks.has_value()) {
      if (!quote_started_at.has_value() || rows[i].best_bid_ticks != previous_bid || rows[i].best_ask_ticks != previous_ask) {
        quote_started_at = rows[i].available_at_ns;
      }
      rows[i].quote_age_ns = rows[i].available_at_ns - *quote_started_at;
    } else {
      quote_started_at.reset();
      rows[i].quote_age_ns.reset();
    }
    previous_bid = rows[i].best_bid_ticks;
    previous_ask = rows[i].best_ask_ticks;

    long double sum_sq = 0.0L;
    std::size_t moves = 0;
    for (std::size_t j = start + 1; j <= i; ++j) {
      if (rows[j].mid_ticks_x2.has_value() && rows[j - 1].mid_ticks_x2.has_value()) {
        const auto diff = *rows[j].mid_ticks_x2 - *rows[j - 1].mid_ticks_x2;
        sum_sq += static_cast<long double>(diff) * static_cast<long double>(diff);
        ++moves;
      }
    }
    if (moves > 0) {
      rows[i].short_window_realized_volatility_x1e6 =
          static_cast<std::int64_t>(std::llround(std::sqrt(sum_sq / moves) * 1'000'000.0L));
    }
  }
}

void AddLabels(std::vector<FeatureRow>& rows,
               std::size_t horizon,
               const std::unordered_map<std::uint64_t, std::int64_t>& fill_labels) {
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto future = i + horizon;
    const auto fill = fill_labels.find(rows[i].sequence_number);
    if (fill != fill_labels.end()) {
      rows[i].fill_within_horizon_proxy = fill->second;
    }
    if (!rows[i].mid_ticks_x2.has_value()) {
      rows[i].label_status = "missing_current_mid";
    } else if (future >= rows.size()) {
      rows[i].label_status = "insufficient_future";
    } else if (rows[i].split != rows[future].split) {
      rows[i].label_status = "purged_split_boundary";
    } else if (!rows[future].mid_ticks_x2.has_value()) {
      rows[i].label_status = "missing_future_mid";
    } else {
      rows[i].future_mid_move_ticks_x2 = *rows[future].mid_ticks_x2 - *rows[i].mid_ticks_x2;
      if (rows[i].side == "BID" && rows[i].best_bid_ticks.has_value()) {
        rows[i].realized_spread_proxy_ticks_x2 = *rows[future].mid_ticks_x2 - 2 * *rows[i].best_bid_ticks;
        rows[i].adverse_selection_proxy_ticks_x2 = *rows[i].mid_ticks_x2 - *rows[future].mid_ticks_x2;
      } else if (rows[i].side == "ASK" && rows[i].best_ask_ticks.has_value()) {
        rows[i].realized_spread_proxy_ticks_x2 = 2 * *rows[i].best_ask_ticks - *rows[future].mid_ticks_x2;
        rows[i].adverse_selection_proxy_ticks_x2 = *rows[future].mid_ticks_x2 - *rows[i].mid_ticks_x2;
      }
      rows[i].label_status = "ok";
    }
  }
}

}  // namespace

Dataset BuildDataset(const FeatureConfig& config) {
  if (config.label_horizon_events == 0) {
    throw std::runtime_error("label_horizon_events must be positive");
  }
  auto cursor = aegis_replay::ReplayCursor::FromJsonl(config.input_path);
  Dataset dataset;
  dataset.config = config;
  while (cursor.has_next()) {
    const auto& event = cursor.next_event();
    dataset.rows.push_back(RowFromSnapshot(config, event, cursor.snapshot(config.depth), dataset.rows.size()));
  }
  dataset.replay_checksum = cursor.state_checksum();
  for (std::size_t i = 0; i < dataset.rows.size(); ++i) {
    dataset.rows[i].replay_checksum = dataset.replay_checksum;
    dataset.rows[i].split = SplitName(i, dataset.rows.size());
  }
  AddWindowFeatures(dataset.rows, config.feature_window_events);
  AddLabels(dataset.rows, config.label_horizon_events, LoadFillLabels(config.fill_labels_path));
  if (!PassesLeakageGuard(dataset.rows)) {
    throw std::runtime_error("feature leakage guard failed");
  }
  return dataset;
}

bool PassesLeakageGuard(const std::vector<FeatureRow>& rows) {
  for (const auto& row : rows) {
    if (row.available_at_ns != row.receive_ts_ns) {
      return false;
    }
    if (row.event_ts_ns > row.available_at_ns) {
      return false;
    }
    if (row.feature_source_sequence_number == 0 || row.feature_source_sequence_number > row.source_sequence_number) {
      return false;
    }
    if (row.feature_max_source_available_at_ns > row.available_at_ns) {
      return false;
    }
  }
  return true;
}

std::string FeatureRowJson(const FeatureRow& row) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "schema_version", JsonString(row.schema_version), first);
  AddJsonField(out, "dataset_id", JsonString(row.dataset_id), first);
  AddJsonField(out, "venue", JsonString(row.venue), first);
  AddJsonField(out, "symbol", JsonString(row.symbol), first);
  AddJsonField(out, "session", JsonString(row.session), first);
  AddJsonField(out, "row_index", std::to_string(row.row_index), first);
  AddJsonField(out, "split", JsonString(row.split), first);
  AddJsonField(out, "sequence_number", std::to_string(row.sequence_number), first);
  AddJsonField(out, "source_sequence_number", std::to_string(row.source_sequence_number), first);
  AddJsonField(out, "feature_source_sequence_number", std::to_string(row.feature_source_sequence_number), first);
  AddJsonField(out, "event_type", JsonString(row.event_type), first);
  AddJsonField(out, "side", JsonString(row.side), first);
  AddJsonField(out, "event_ts_ns", std::to_string(row.event_ts_ns), first);
  AddJsonField(out, "receive_ts_ns", std::to_string(row.receive_ts_ns), first);
  AddJsonField(out, "available_at_ns", std::to_string(row.available_at_ns), first);
  AddJsonField(out, "feature_max_source_available_at_ns", std::to_string(row.feature_max_source_available_at_ns), first);
  AddJsonField(out, "snapshot_state_checksum", std::to_string(row.snapshot_state_checksum), first);
  AddJsonField(out, "replay_checksum", std::to_string(row.replay_checksum), first);
  AddJsonField(out, "best_bid_ticks", JsonInt(row.best_bid_ticks), first);
  AddJsonField(out, "best_ask_ticks", JsonInt(row.best_ask_ticks), first);
  AddJsonField(out, "spread_ticks", JsonInt(row.spread_ticks), first);
  AddJsonField(out, "mid_ticks_x2", JsonInt(row.mid_ticks_x2), first);
  AddJsonField(out, "microprice_ticks_x2", JsonInt(row.microprice_ticks_x2), first);
  AddJsonField(out, "bid_qty_top1", std::to_string(row.bid_qty_top1), first);
  AddJsonField(out, "ask_qty_top1", std::to_string(row.ask_qty_top1), first);
  AddJsonField(out, "depth_bid_qty", std::to_string(row.depth_bid_qty), first);
  AddJsonField(out, "depth_ask_qty", std::to_string(row.depth_ask_qty), first);
  AddJsonField(out, "depth_imbalance_x1e6", std::to_string(row.depth_imbalance_x1e6), first);
  AddJsonField(out, "order_flow_imbalance_qty", std::to_string(row.order_flow_imbalance_qty), first);
  AddJsonField(out, "event_rate_per_second_x1e6", JsonInt(row.event_rate_per_second_x1e6), first);
  AddJsonField(out, "quote_age_ns", JsonInt(row.quote_age_ns), first);
  AddJsonField(out, "short_window_realized_volatility_x1e6", JsonInt(row.short_window_realized_volatility_x1e6), first);
  AddJsonField(out, "future_mid_move_ticks_x2", JsonInt(row.future_mid_move_ticks_x2), first);
  AddJsonField(out, "realized_spread_proxy_ticks_x2", JsonInt(row.realized_spread_proxy_ticks_x2), first);
  AddJsonField(out, "adverse_selection_proxy_ticks_x2", JsonInt(row.adverse_selection_proxy_ticks_x2), first);
  AddJsonField(out, "fill_within_horizon_proxy", JsonInt(row.fill_within_horizon_proxy), first);
  AddJsonField(out, "label_status", JsonString(row.label_status), first);
  out << '}';
  return out.str();
}

std::string DatasetMetadataJson(const Dataset& dataset) {
  const auto row_count = dataset.rows.size();
  const auto future_mid_labeled = static_cast<std::size_t>(
      std::count_if(dataset.rows.begin(), dataset.rows.end(), [](const FeatureRow& row) {
        return row.future_mid_move_ticks_x2.has_value();
      }));
  const auto proxy_labeled = static_cast<std::size_t>(
      std::count_if(dataset.rows.begin(), dataset.rows.end(), [](const FeatureRow& row) {
        return row.realized_spread_proxy_ticks_x2.has_value() || row.adverse_selection_proxy_ticks_x2.has_value() ||
               row.fill_within_horizon_proxy.has_value();
      }));
  const auto purged = static_cast<std::size_t>(
      std::count_if(dataset.rows.begin(), dataset.rows.end(), [](const FeatureRow& row) {
        return row.label_status == "purged_split_boundary";
      }));

  struct SplitSummary {
    std::size_t rows{};
    std::size_t future_mid_labels{};
    std::size_t proxy_labels{};
    std::size_t purged_horizon_count{};
    std::optional<std::uint64_t> first_row;
    std::optional<std::uint64_t> last_row;
    std::optional<std::uint64_t> first_sequence;
    std::optional<std::uint64_t> last_sequence;
    std::optional<std::int64_t> first_event_ts;
    std::optional<std::int64_t> last_event_ts;
    std::optional<std::int64_t> first_available_at;
    std::optional<std::int64_t> last_available_at;
    std::map<std::string, std::size_t> label_status_counts;
  };

  auto summarize = [&](const std::string& split) {
    SplitSummary summary;
    for (const auto& row : dataset.rows) {
      if (row.split != split) {
        continue;
      }
      ++summary.rows;
      summary.first_row = summary.first_row.value_or(row.row_index);
      summary.last_row = row.row_index;
      summary.first_sequence = summary.first_sequence.value_or(row.sequence_number);
      summary.last_sequence = row.sequence_number;
      summary.first_event_ts = summary.first_event_ts.value_or(row.event_ts_ns);
      summary.last_event_ts = row.event_ts_ns;
      summary.first_available_at = summary.first_available_at.value_or(row.available_at_ns);
      summary.last_available_at = row.available_at_ns;
      summary.future_mid_labels += row.future_mid_move_ticks_x2.has_value() ? 1 : 0;
      summary.proxy_labels += row.realized_spread_proxy_ticks_x2.has_value() ||
                                      row.adverse_selection_proxy_ticks_x2.has_value() ||
                                      row.fill_within_horizon_proxy.has_value()
                                  ? 1
                                  : 0;
      summary.purged_horizon_count += row.label_status == "purged_split_boundary" ? 1 : 0;
      ++summary.label_status_counts[row.label_status];
    }
    return summary;
  };

  auto write_status_counts = [](std::ostringstream& out, const SplitSummary& summary) {
    out << "{";
    bool first = true;
    for (const auto& [status, count] : summary.label_status_counts) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << JsonString(status) << ": " << count;
    }
    out << "}";
  };

  auto write_split = [&](std::ostringstream& out, const std::string& name, const SplitSummary& summary, bool comma) {
    out << "    " << JsonString(name) << ": {";
    out << "\"rows\": " << summary.rows;
    out << ", \"row_range\": [" << JsonUint(summary.first_row) << ", " << JsonUint(summary.last_row) << "]";
    out << ", \"sequence_range\": [" << JsonUint(summary.first_sequence) << ", "
        << JsonUint(summary.last_sequence) << "]";
    out << ", \"event_time_range_ns\": [" << JsonInt(summary.first_event_ts) << ", "
        << JsonInt(summary.last_event_ts) << "]";
    out << ", \"available_at_range_ns\": [" << JsonInt(summary.first_available_at) << ", "
        << JsonInt(summary.last_available_at) << "]";
    out << ", \"future_mid_labels\": " << summary.future_mid_labels;
    out << ", \"proxy_labels\": " << summary.proxy_labels;
    out << ", \"purged_horizon_count\": " << summary.purged_horizon_count;
    out << ", \"label_status_counts\": ";
    write_status_counts(out, summary);
    out << "}" << (comma ? "," : "") << "\n";
  };

  const auto component_id =
      dataset.config.dataset_id.find("validation") == std::string::npos ? "features" : "features-validation";
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  out << "  \"component_id\": " << JsonString(component_id) << ",\n";
  out << "  \"dataset_id\": " << JsonString(dataset.config.dataset_id) << ",\n";
  out << "  \"format\": \"jsonl\",\n";
  out << "  \"source_input\": " << JsonString(dataset.config.input_path.generic_string()) << ",\n";
  out << "  \"fill_labels_input\": "
      << (dataset.config.fill_labels_path.has_value() ? JsonString(dataset.config.fill_labels_path->generic_string())
                                                      : "null")
      << ",\n";
  out << "  \"row_count\": " << row_count << ",\n";
  out << "  \"rows_with_future_mid_labels\": " << future_mid_labeled << ",\n";
  out << "  \"rows_with_proxy_labels\": " << proxy_labeled << ",\n";
  out << "  \"rows_without_future_mid_labels\": " << row_count - future_mid_labeled << ",\n";
  out << "  \"label_horizon_events\": " << dataset.config.label_horizon_events << ",\n";
  out << "  \"feature_window_events\": " << dataset.config.feature_window_events << ",\n";
  out << "  \"purged_horizon_count\": " << purged << ",\n";
  out << "  \"available_at_semantics\": \"receive_ts_ns after the source event is replayed\",\n";
  out << "  \"feature_source_semantics\": \"feature_source_sequence_number and feature_max_source_available_at_ns must not exceed the row source event\",\n";
  out << "  \"replay_checksum\": " << dataset.replay_checksum << ",\n";
  out << "  \"feature_columns\": [\"best_bid_ticks\", \"best_ask_ticks\", \"spread_ticks\", \"mid_ticks_x2\", "
         "\"microprice_ticks_x2\", \"bid_qty_top1\", \"ask_qty_top1\", \"depth_bid_qty\", \"depth_ask_qty\", "
         "\"depth_imbalance_x1e6\", \"order_flow_imbalance_qty\", \"event_rate_per_second_x1e6\", "
         "\"quote_age_ns\", \"short_window_realized_volatility_x1e6\", \"event_type\", \"side\", "
         "\"sequence_number\", \"event_ts_ns\", \"receive_ts_ns\", \"available_at_ns\"],\n";
  out << "  \"lineage_columns\": [\"source_sequence_number\", \"feature_source_sequence_number\", "
         "\"feature_max_source_available_at_ns\", \"snapshot_state_checksum\", \"replay_checksum\"],\n";
  out << "  \"label_columns\": [\"future_mid_move_ticks_x2\", \"realized_spread_proxy_ticks_x2\", "
         "\"adverse_selection_proxy_ticks_x2\", \"fill_within_horizon_proxy\", \"label_status\"],\n";
  out << "  \"splits\": {\n";
  write_split(out, "train", summarize("train"), true);
  write_split(out, "validation", summarize("validation"), true);
  write_split(out, "test", summarize("test"), false);
  out << "  },\n";
  out << "  \"limitations\": [\"synthetic fixture\", \"JSONL output\", \"fill proxy labels depend on optional execution artifacts\"]\n";
  out << "}\n";
  return out.str();
}

void WriteFeaturesJsonl(const std::filesystem::path& path, const std::vector<FeatureRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write features: " + path.string());
  }
  for (const auto& row : rows) {
    out << FeatureRowJson(row) << '\n';
  }
}

void WriteDatasetMetadata(const std::filesystem::path& path, const Dataset& dataset) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write dataset metadata: " + path.string());
  }
  out << DatasetMetadataJson(dataset);
}

}  // namespace vf_features
